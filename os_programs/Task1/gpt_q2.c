/*
 * threaded_cmp_mq.c
 *
 * Compare two files block-by-block using N threads and POSIX message queues.
 *
 * Build:
 *   gcc -std=c11 -O2 -Wall -pthread -lrt -o threaded_cmp_mq threaded_cmp_mq.c
 *
 * Usage:
 *   ./threaded_cmp_mq <file1> <file2> <num_threads> [block_size]
 *
 * Notes:
 * - If files differ in size, program reports mismatch immediately.
 * - Default block_size = 4096 bytes if not supplied.
 *
 * Design:
 * - Parent thread creates a parent MQ (name includes PID).
 * - Each worker thread creates a per-worker MQ and repeatedly:
 *     - Sends READY to parent (with its worker-MQ name).
 *     - Waits on its worker-MQ for a job (block index + length),
 *       or block_idx == -1 meaning "stop".
 *     - Reads block by pread() on shared file descriptors, compares,
 *       sends RESULT on parent MQ if mismatch or DONE when exiting.
 * - Parent listens on parent MQ, dispatches jobs to worker MQs on demand.
 * - On first mismatch, parent stops issuing new jobs, replies STOP to
 *   subsequent READY messages. Parent gathers per-thread processed counts.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mqueue.h>
#include <signal.h>
#include <inttypes.h>

#define DEFAULT_BLOCK_SIZE 4096
#define PARENT_MQ_NAME_FMT "/pcmp_parent_%d"
#define WORKER_MQ_NAME_FMT "/pcmp_w_%d_%d"
#define MAX_MQ_NAME_LEN 64
#define MQ_MSGSIZE 512
#define MQ_MAXMSG 16

/* Parent-message types */
enum { MSG_READY = 1, MSG_RESULT = 2, MSG_DONE = 3, MSG_INTERRUPT = 4 };

/* Message format placed on parent queue (workers -> parent) */
typedef struct {
    int type;                       /* MSG_READY / MSG_RESULT / MSG_DONE */
    int worker_idx;                 /* index passed to worker thread (0..N-1) */
    char worker_mq_name[MAX_MQ_NAME_LEN]; /* valid for MSG_READY so parent can reply */
    long long block_idx;            /* for MSG_RESULT: mismatched block index, or -1 */
    size_t len;                     /* length read (for debugging) */
    int mismatch;                   /* 1 if mismatch found (MSG_RESULT), else 0 */
    int blocks_processed;           /* worker's processed count (for MSG_DONE or MSG_RESULT) */
} parent_msg_t;

/* Job message parent -> worker (sent to specific worker queue) */
typedef struct {
    long long block_idx;            /* -1 -> stop */
    size_t len;
} job_msg_t;

/* Global shared state */
static volatile sig_atomic_t g_sigint = 0;
static char g_parent_mq_name[MAX_MQ_NAME_LEN];

static int g_num_threads = 0;
static pthread_t *g_threads = NULL;

/* File descriptors shared by all threads (safe with pread) */
static int g_fd1 = -1;
static int g_fd2 = -1;
static long long g_file_size = 0;
static size_t g_block_size = DEFAULT_BLOCK_SIZE;
static long long g_num_blocks = 0;

/* Orchestration state protected by mutex */
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static long long g_next_block = 0;
static int g_mismatch_found = 0;
static long long g_mismatch_block = -1;

/* Keep track of worker MQ names as they register (for signal cleanup) */
static char (*g_worker_mq_names)[MAX_MQ_NAME_LEN] = NULL;

/* Per-thread final counts (index -> processed blocks). Initialized to -1 for unknown. */
static int *g_blocks_processed = NULL;

/* Signal handler */
static void sigint_handler(int sig) {
    (void)sig;
    g_sigint = 1;
    /* Note: do not call non-async-signal-safe functions here. Parent loop will detect g_sigint. */
}

/* Helper: create an mq with given name (O_CREAT|O_RDONLY for worker, O_CREAT|O_RDONLY for parent) */
static mqd_t create_mq(const char *name, int oflag, mode_t mode) {
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = MQ_MAXMSG;
    attr.mq_msgsize = MQ_MSGSIZE;
    attr.mq_curmsgs = 0;
    mqd_t q = mq_open(name, oflag, mode, &attr);
    return q;
}

/* Worker thread function */
typedef struct {
    int idx;
} worker_arg_t;

static void *worker_thread(void *argv) {
    worker_arg_t *arg = (worker_arg_t*)argv;
    int idx = arg->idx;
    free(arg);

    /* Build worker mq name */
    char wqname[MAX_MQ_NAME_LEN];
    snprintf(wqname, sizeof(wqname), WORKER_MQ_NAME_FMT, (int)getpid(), idx);

    /* create and open worker queue (parent will open it for sending) */
    mqd_t wq = create_mq(wqname, O_CREAT | O_RDONLY, 0600);
    if (wq == (mqd_t)-1) {
        fprintf(stderr, "Worker %d: mq_open worker '%s' failed: %s\n", idx, wqname, strerror(errno));
        pthread_exit((void*)1);
    }

    /* open parent queue for sending messages */
    mqd_t pq = mq_open(g_parent_mq_name, O_WRONLY);
    if (pq == (mqd_t)-1) {
        fprintf(stderr, "Worker %d: mq_open parent '%s' failed: %s\n", idx, g_parent_mq_name, strerror(errno));
        mq_close(wq); mq_unlink(wqname);
        pthread_exit((void*)2);
    }

    /* register own MQ name with global array (so signal handler / parent cleanup can attempt unlink if needed) */
    pthread_mutex_lock(&g_state_lock);
    strncpy(g_worker_mq_names[idx], wqname, MAX_MQ_NAME_LEN);
    pthread_mutex_unlock(&g_state_lock);

    /* allocate buffers */
    char *buf1 = malloc(g_block_size);
    char *buf2 = malloc(g_block_size);
    if (!buf1 || !buf2) {
        fprintf(stderr, "Worker %d: malloc failed\n", idx);
        mq_close(wq); mq_unlink(wqname);
        mq_close(pq);
        free(buf1); free(buf2);
        pthread_exit((void*)3);
    }

    int blocks_processed = 0;

    for (;;) {
        /* 1) Send READY message to parent with our MQ name */
        parent_msg_t ready = {0};
        ready.type = MSG_READY;
        ready.worker_idx = idx;
        strncpy(ready.worker_mq_name, wqname, MAX_MQ_NAME_LEN - 1);
        if (mq_send(pq, (const char*)&ready, sizeof(parent_msg_t), 0) == -1) {
            /* if parent removed queue or error, exit */
            if (errno == EINVAL || errno == EBADF) {
                break;
            }
            fprintf(stderr, "Worker %d: mq_send READY failed: %s\n", idx, strerror(errno));
            break;
        }

        /* 2) Wait for job from parent on our worker queue */
        char job_buf[MQ_MSGSIZE];
        ssize_t r = mq_receive(wq, job_buf, MQ_MSGSIZE, NULL);
        if (r == -1) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Worker %d: mq_receive on %s failed: %s\n", idx, wqname, strerror(errno));
            break;
        }
        if ((size_t)r < sizeof(job_msg_t)) {
            /* malformed/short message; treat as stop */
            break;
        }
        job_msg_t job;
        memcpy(&job, job_buf, sizeof(job_msg_t));

        if (job.block_idx < 0) {
            /* stop requested */
            break;
        }

        off_t offset = (off_t)job.block_idx * (off_t)g_block_size;
        size_t to_read = job.len;

        ssize_t n1 = pread(g_fd1, buf1, to_read, offset);
        ssize_t n2 = pread(g_fd2, buf2, to_read, offset);
        if (n1 < 0 || n2 < 0) {
            fprintf(stderr, "Worker %d: pread error: %s\n", idx, strerror(errno));
            /* report done with what we have */
            parent_msg_t done = {0};
            done.type = MSG_DONE;
            done.worker_idx = idx;
            done.blocks_processed = blocks_processed;
            mq_send(pq, (const char*)&done, sizeof(parent_msg_t), 0);
            break;
        }

        blocks_processed++;

        if (n1 != n2 || (n1 > 0 && memcmp(buf1, buf2, (size_t)n1) != 0)) {
            /* mismatch found: notify parent and exit */
            parent_msg_t res = {0};
            res.type = MSG_RESULT;
            res.worker_idx = idx;
            res.block_idx = job.block_idx;
            res.len = (size_t)n1;
            res.mismatch = 1;
            res.blocks_processed = blocks_processed;
            mq_send(pq, (const char*)&res, sizeof(parent_msg_t), 0);

            /* cleanup and exit */
            break;
        }

        /* otherwise matched; loop to request next job */
    }

    /* send final DONE message (if we didn't already send a RESULT) */
    parent_msg_t final_done = {0};
    final_done.type = MSG_DONE;
    final_done.worker_idx = idx;
    final_done.blocks_processed = blocks_processed;
    mq_send(pq, (const char*)&final_done, sizeof(parent_msg_t), 0);

    /* cleanup */
    free(buf1);
    free(buf2);
    mq_close(wq);
    /* unlink worker queue so it doesn't persist */
    mq_unlink(wqname);
    mq_close(pq);

    pthread_exit((void*)0);
}

/* Parent orchestrator (main thread) - listens on parent MQ and sends jobs to worker MQs */
int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: %s <file1> <file2> <num_threads> [block_size]\n", argv[0]);
        return 2;
    }

    const char *file1 = argv[1];
    const char *file2 = argv[2];
    int num_threads = atoi(argv[3]);
    if (num_threads <= 0) num_threads = 1;
    g_num_threads = num_threads;

    long tmp = 0;
    if (argc == 5) tmp = atol(argv[4]);
    if (tmp > 0) g_block_size = (size_t)tmp;

    /* install SIGINT handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    /* stat files */
    struct stat st1, st2;
    if (stat(file1, &st1) == -1) { perror("stat file1"); return 3; }
    if (stat(file2, &st2) == -1) { perror("stat file2"); return 3; }

    if (st1.st_size != st2.st_size) {
        printf("Result: FAILURE (file sizes differ: %" PRId64 " vs %" PRId64 " bytes)\n",
               (int64_t)st1.st_size, (int64_t)st2.st_size);
        return 0;
    }

    g_file_size = (long long)st1.st_size;
    g_num_blocks = (g_file_size + (long long)g_block_size - 1) / (long long)g_block_size;
    if (g_num_blocks == 0) {
        printf("Files empty: SUCCESS\n");
        return 0;
    }

    /* open shared file descriptors for pread */
    g_fd1 = open(file1, O_RDONLY);
    if (g_fd1 == -1) { perror("open file1"); return 4; }
    g_fd2 = open(file2, O_RDONLY);
    if (g_fd2 == -1) { perror("open file2"); close(g_fd1); return 4; }

    /* create parent MQ name and MQ */
    snprintf(g_parent_mq_name, sizeof(g_parent_mq_name), PARENT_MQ_NAME_FMT, (int)getpid());
    mqd_t parent_q = create_mq(g_parent_mq_name, O_CREAT | O_RDONLY, 0600);
    if (parent_q == (mqd_t)-1) {
        fprintf(stderr, "Parent: mq_open '%s' failed: %s\n", g_parent_mq_name, strerror(errno));
        close(g_fd1); close(g_fd2);
        return 5;
    }

    /* We will open parent queue for sending too (workers need to mq_open O_WRONLY),
       but parent created it O_RDONLY above. To send from parent (rare), open a writer handle. */
    mqd_t parent_q_w = mq_open(g_parent_mq_name, O_WRONLY);
    if (parent_q_w == (mqd_t)-1) {
        /* not fatal; parent may not need to write to its own queue */
    }

    /* allocate arrays */
    g_threads = calloc(num_threads, sizeof(pthread_t));
    g_worker_mq_names = calloc(num_threads, MAX_MQ_NAME_LEN);
    g_blocks_processed = calloc(num_threads, sizeof(int));
    if (!g_threads || !g_worker_mq_names || !g_blocks_processed) {
        fprintf(stderr, "Parent: allocation failure\n");
        mq_close(parent_q); mq_unlink(g_parent_mq_name);
        close(g_fd1); close(g_fd2);
        return 6;
    }
    for (int i = 0; i < num_threads; ++i) g_blocks_processed[i] = -1;

    /* create worker threads */
    for (int i = 0; i < num_threads; ++i) {
        worker_arg_t *a = malloc(sizeof(worker_arg_t));
        if (!a) { fprintf(stderr, "alloc fail\n"); exit(1); }
        a->idx = i;
        if (pthread_create(&g_threads[i], NULL, worker_thread, a) != 0) {
            fprintf(stderr, "Parent: pthread_create failed for worker %d\n", i);
            free(a);
            /* continue; parent may still handle fewer threads */
        }
    }

    /* parent main loop: receive READY/RESULT/DONE on parent queue and dispatch jobs */
    int workers_done = 0;
    while (workers_done < num_threads) {
        /* check for signal */
        if (g_sigint) {
            /* attempt to stop workers by sending stop to known worker MQs */
            pthread_mutex_lock(&g_state_lock);
            for (int i = 0; i < num_threads; ++i) {
                if (g_worker_mq_names[i][0] != '\0') {
                    /* send STOP job */
                    mqd_t wq = mq_open(g_worker_mq_names[i], O_WRONLY);
                    if (wq != (mqd_t)-1) {
                        job_msg_t stop = { .block_idx = -1, .len = 0 };
                        mq_send(wq, (const char*)&stop, sizeof(job_msg_t), 0);
                        mq_close(wq);
                    }
                }
            }
            pthread_mutex_unlock(&g_state_lock);
            /* break out and then wait for DONE messages; or continue to receive them */
            g_sigint = 0; /* handle only once */
        }

        char buf[MQ_MSGSIZE];
        ssize_t r = mq_receive(parent_q, buf, MQ_MSGSIZE, NULL);
        if (r == -1) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Parent: mq_receive failed: %s\n", strerror(errno));
            break;
        }
        if ((size_t)r < sizeof(parent_msg_t)) {
            /* ignore malformed */
            continue;
        }
        parent_msg_t msg;
        memcpy(&msg, buf, sizeof(parent_msg_t));

        if (msg.type == MSG_READY) {
            /* register worker MQ name if not seen */
            pthread_mutex_lock(&g_state_lock);
            if (g_worker_mq_names[msg.worker_idx][0] == '\0') {
                strncpy(g_worker_mq_names[msg.worker_idx], msg.worker_mq_name, MAX_MQ_NAME_LEN - 1);
            }
            /* prepare reply job message */
            job_msg_t job;
            if (g_mismatch_found || g_next_block >= g_num_blocks) {
                job.block_idx = -1; /* send stop */
                job.len = 0;
            } else {
                long long b = g_next_block++;
                job.block_idx = b;
                long long remaining = g_file_size - (b * (long long)g_block_size);
                job.len = (size_t)(remaining < (long long)g_block_size ? remaining : (long long)g_block_size);
            }
            pthread_mutex_unlock(&g_state_lock);

            /* open worker MQ for sending job */
            mqd_t wq = mq_open(msg.worker_mq_name, O_WRONLY);
            if (wq == (mqd_t)-1) {
                /* worker queue not available: count it as done to avoid infinite loop */
                fprintf(stderr, "Parent: unable to open worker MQ '%s' (%s). Treating worker %d as done.\n",
                        msg.worker_mq_name, strerror(errno), msg.worker_idx);
                parent_msg_t fake_done = {0};
                fake_done.type = MSG_DONE;
                fake_done.worker_idx = msg.worker_idx;
                fake_done.blocks_processed = -1;
                /* simulate a done: set later */
                mq_close(wq);
                /* increment workers_done and set -1 processed if not set */
                pthread_mutex_lock(&g_state_lock);
                if (g_blocks_processed[msg.worker_idx] == -1) {
                    g_blocks_processed[msg.worker_idx] = 0;
                }
                workers_done++;
                pthread_mutex_unlock(&g_state_lock);
                continue;
            }
            if (mq_send(wq, (const char*)&job, sizeof(job_msg_t), 0) == -1) {
                fprintf(stderr, "Parent: mq_send job to %s failed: %s\n", msg.worker_mq_name, strerror(errno));
            }
            mq_close(wq);

        } else if (msg.type == MSG_RESULT) {
            /* mismatch reported */
            pthread_mutex_lock(&g_state_lock);
            if (!g_mismatch_found) {
                g_mismatch_found = 1;
                g_mismatch_block = msg.block_idx;
            }
            /* record blocks processed for that worker */
            g_blocks_processed[msg.worker_idx] = msg.blocks_processed;
            pthread_mutex_unlock(&g_state_lock);

            /* We do not immediately force-stop all workers; parent will reply STOP to READY requests.
               But we could proactively send STOP to known workers to speed up termination: */
            pthread_mutex_lock(&g_state_lock);
            for (int i = 0; i < num_threads; ++i) {
                if (g_worker_mq_names[i][0] != '\0') {
                    mqd_t wq = mq_open(g_worker_mq_names[i], O_WRONLY);
                    if (wq != (mqd_t)-1) {
                        job_msg_t stop = { .block_idx = -1, .len = 0 };
                        mq_send(wq, (const char*)&stop, sizeof(job_msg_t), 0);
                        mq_close(wq);
                    }
                }
            }
            pthread_mutex_unlock(&g_state_lock);

            /* treat the reporter as done (it will send a DONE soon too) */
            /* We do not increment workers_done here; we'll wait for MSG_DONE from each */
        } else if (msg.type == MSG_DONE) {
            pthread_mutex_lock(&g_state_lock);
            if (g_blocks_processed[msg.worker_idx] == -1) {
                g_blocks_processed[msg.worker_idx] = msg.blocks_processed;
            }
            workers_done++;
            pthread_mutex_unlock(&g_state_lock);
        } else {
            /* ignore unexpected */
        }
    }

    /* join worker threads */
    for (int i = 0; i < num_threads; ++i) {
        if (g_threads[i]) {
            pthread_join(g_threads[i], NULL);
        }
    }

    /* print final report */
    if (g_mismatch_found) {
        printf("Result: FAILURE (mismatch at block index %" PRId64 ")\n", (int64_t)g_mismatch_block);
    } else {
        printf("Result: SUCCESS (files identical)\n");
    }
    printf("Blocks total: %" PRId64 " (block size %zu)\n", (int64_t)g_num_blocks, g_block_size);
    printf("Per-thread blocks processed:\n");
    for (int i = 0; i < num_threads; ++i) {
        int cnt = g_blocks_processed[i];
        if (cnt < 0) cnt = 0;
        printf("  Thread %d : %d blocks\n", i, cnt);
    }

    /* cleanup: close and unlink parent MQ, unlink any worker MQs not yet removed */
    mq_close(parent_q);
    mq_unlink(g_parent_mq_name);
    if (parent_q_w != (mqd_t)-1) mq_close(parent_q_w);

    pthread_mutex_lock(&g_state_lock);
    for (int i = 0; i < num_threads; ++i) {
        if (g_worker_mq_names[i][0] != '\0') {
            /* attempt unlink in case worker didn't unlink (best-effort) */
            mq_unlink(g_worker_mq_names[i]);
        }
    }
    pthread_mutex_unlock(&g_state_lock);

    free(g_threads);
    free(g_worker_mq_names);
    free(g_blocks_processed);
    close(g_fd1);
    close(g_fd2);

    return g_mismatch_found ? 1 : 0;
}
