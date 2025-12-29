/*
 * parallel_cmp.c
 *
 * Compare two files block-by-block using N parallel processes and a System V message queue.
 *
 * Build:
 *   gcc -std=c11 -O2 -Wall -Wextra -o parallel_cmp parallel_cmp.c
 *
 * Usage:
 *   ./parallel_cmp <file1> <file2> <num_workers> [block_size]
 *
 * Example:
 *   ./parallel_cmp bigA.bin bigB.bin 8 65536
 *
 * Notes:
 * - If files differ in size, program reports mismatch immediately.
 * - Default block_size = 4096 bytes if not supplied.
 */

#define _XOPEN_SOURCE 700
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>

#define DEFAULT_BLOCK_SIZE 4096
#define READY_TYPE 1L       /* workers -> parent: ready request (mtype) */
#define RESULT_TYPE 2L      /* workers -> parent: result/done (mtype) */

/* Message layout used on the single message queue.
 * Note: when calling msgsnd/msgrcv the "size" argument = sizeof(msg) - sizeof(long)
 */
typedef struct {
    long mtype;                /* message type (see constants above and worker PID for job replies) */
    pid_t pid;                 /* sender PID (workers set this) */
    long long block_idx;       /* block index for job (parent->worker) OR mismatched block index (worker->parent) */
    size_t len;                /* length of the block (parent->worker) */
    int mismatch;              /* worker->parent: 1 if mismatch found, 0 otherwise */
    int blocks_processed;      /* worker->parent: total blocks processed by this worker when exiting */
} msg_t;

/* Global state for cleanup handlers */
static int g_msqid = -1;
static pid_t *g_children = NULL;
static int g_child_count = 0;

/* Remove msg queue and optionally kill children */
static void cleanup_resources(void) {
    if (g_msqid != -1) {
        /* remove message queue */
        if (msgctl(g_msqid, IPC_RMID, NULL) == -1 && errno != EINVAL) {
            perror("msgctl(IPC_RMID)");
        }
        g_msqid = -1;
    }
    if (g_children) {
        for (int i = 0; i < g_child_count; ++i) {
            if (g_children[i] > 0) {
                kill(g_children[i], SIGTERM);
            }
        }
    }
}

/* SIGINT / SIGTERM handler -> best-effort cleanup and exit */
static void term_handler(int signo) {
    (void)signo;
    cleanup_resources();
    _exit(2);
}

/* Helper to send a message (wrap msgsnd with error handling) */
static void send_msg(int msqid, msg_t *m) {
    if (msgsnd(msqid, m, sizeof(msg_t) - sizeof(long), 0) == -1) {
        /* If parent removed queue, children might get EINVAL; just exit gracefully */
        perror("msgsnd");
        _exit(3);
    }
}

/* Worker process function */
static void worker_loop(const char *file1, const char *file2, int msqid, size_t block_size) {
    pid_t me = getpid();
    int fd1 = open(file1, O_RDONLY);
    int fd2 = open(file2, O_RDONLY);
    if (fd1 == -1 || fd2 == -1) {
        perror("open in worker");
        _exit(4);
    }

    char *buf1 = malloc(block_size);
    char *buf2 = malloc(block_size);
    if (!buf1 || !buf2) {
        perror("malloc");
        close(fd1); close(fd2);
        _exit(5);
    }

    int blocks_processed = 0;

    for (;;) {
        /* send ready message to parent */
        msg_t ready = {0};
        ready.mtype = READY_TYPE;
        ready.pid = me;
        send_msg(msqid, &ready);

        /* wait for job from parent: parent will send with mtype == worker_pid */
        msg_t job = {0};
        ssize_t r = msgrcv(msqid, &job, sizeof(msg_t) - sizeof(long), (long)me, 0);
        if (r == -1) { perror("msgrcv job"); break; }

        if (job.block_idx < 0) {
            /* -1 means stop/exit */
            break;
        }

        off_t offset = (off_t)job.block_idx * (off_t)block_size;
        size_t to_read = job.len;

        ssize_t n1 = pread(fd1, buf1, to_read, offset);
        ssize_t n2 = pread(fd2, buf2, to_read, offset);
        if (n1 < 0 || n2 < 0) {
            perror("pread");
            /* send done with processed count and exit */
            msg_t done = {0};
            done.mtype = RESULT_TYPE;
            done.pid = me;
            done.block_idx = -1;
            done.blocks_processed = blocks_processed;
            done.mismatch = 0;
            send_msg(msqid, &done);
            break;
        }

        blocks_processed++;

        if (n1 != n2 || (n1 > 0 && memcmp(buf1, buf2, (size_t)n1) != 0)) {
            /* mismatch found: report mismatch and exit */
            msg_t res = {0};
            res.mtype = RESULT_TYPE;
            res.pid = me;
            res.block_idx = job.block_idx;
            res.len = (size_t)n1;
            res.mismatch = 1;
            res.blocks_processed = blocks_processed;
            send_msg(msqid, &res);

            /* Also send a final done message (to be explicit) -- parent treats any RESULT as final for that worker */
            /* (We embed blocks_processed in the same message, so no extra 'done' needed; but keep consistent) */
            close(fd1); close(fd2);
            free(buf1); free(buf2);
            _exit(0); /* exit now */
        }

        /* otherwise match: continue loop and request next job */
    }

    /* send done message with blocks_processed */
    msg_t done = {0};
    done.mtype = RESULT_TYPE;
    done.pid = me;
    done.block_idx = -1;
    done.blocks_processed = blocks_processed;
    done.mismatch = 0;
    send_msg(msqid, &done);

    close(fd1);
    close(fd2);
    free(buf1); free(buf2);
    _exit(0);
}

/* parent: orchestrates workers and collates results */
int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: %s <file1> <file2> <num_workers> [block_size]\n", argv[0]);
        return 2;
    }

    const char *file1 = argv[1];
    const char *file2 = argv[2];
    int num_workers = atoi(argv[3]);
    if (num_workers <= 0) num_workers = 1;

    size_t block_size = DEFAULT_BLOCK_SIZE;
    if (argc == 5) {
        long tmp = atol(argv[4]);
        if (tmp > 0) block_size = (size_t)tmp;
    }

    /* prepare signal handlers for cleanup */
    struct sigaction sa = {0};
    sa.sa_handler = term_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* stat files - to store detailed information and attributes about a file (for size comparision)*/
    struct stat st1, st2;
    if (stat(file1, &st1) == -1) { perror("stat file1"); return 3; }
    if (stat(file2, &st2) == -1) { perror("stat file2"); return 3; }

    if (st1.st_size != st2.st_size) {
        printf("Result: FAILURE (file sizes differ: %lld vs %lld bytes)\n",
               (long long)st1.st_size, (long long)st2.st_size);
        printf("No worker processes were launched.\n");
        return 0;
    }

    long long file_size = (long long)st1.st_size;
    long long num_blocks = (file_size + (long long)block_size - 1) / (long long)block_size;

    if (num_blocks == 0) {
        printf("Files are empty: Success.\n");
        return 0;
    }

    /* create a single System V message queue */
    int msqid = msgget(IPC_PRIVATE, IPC_CREAT | IPC_EXCL | 0600); 
    // create new msgque file_id exclusively that is not in use by other processes with ownership (read & write)
    if (msqid == -1) { perror("msgget"); return 4; }
    g_msqid = msqid;

    /* record children */
    g_children = calloc(num_workers, sizeof(pid_t));
    if (!g_children) { perror("calloc"); cleanup_resources(); return 5; }
    g_child_count = num_workers;

    /* spawn workers */
    for (int i = 0; i < num_workers; ++i) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            /* continue creating remaining? For simplicity, abort and cleanup */
            cleanup_resources();
            return 6;
        } else if (pid == 0) {
            /* child (worker) */
            worker_loop(file1, file2, msqid, block_size);
            /* worker_loop does _exit; not reached */
        } else {
            g_children[i] = pid;
        }
    }

    /* parent orchestrator: respond to worker ready requests and collect results */
    long long next_block = 0;
    int workers_done = 0;
    int mismatch_found = 0;
    long long mismatch_block = -1;
    pid_t *worker_pids_seen = calloc(num_workers, sizeof(pid_t));
    int worker_pids_seen_count = 0;

    /* map for reporting per-worker blocks processed: pid -> count (simple linear search, worker count small) */
    typedef struct { pid_t pid; int count; } wpair;
    wpair *wc = calloc(num_workers, sizeof(wpair));

    while (workers_done < num_workers) {
        msg_t m = {0};
        ssize_t r = msgrcv(msqid, &m, sizeof(msg_t) - sizeof(long), 0, 0);
        if (r == -1) {
            if (errno == EINTR) continue;
            perror("msgrcv parent");
            break;
        }

        if (m.mtype == READY_TYPE) {
            pid_t worker_pid = m.pid;
            /* record known worker pid */
            int known = 0;
            for (int j = 0; j < worker_pids_seen_count; ++j) {
                if (worker_pids_seen[j] == worker_pid) { known = 1; break; }
            }
            if (!known && worker_pids_seen_count < num_workers) {
                worker_pids_seen[worker_pids_seen_count++] = worker_pid;
            }

            /* If we already found mismatch, tell worker to stop */
            msg_t reply = {0};
            reply.mtype = (long)worker_pid;

            if (mismatch_found || next_block >= num_blocks) {
                reply.block_idx = -1; /* stop */
                send_msg(msqid, &reply);
            } else {
                reply.block_idx = next_block++;
                long long remaining_bytes = file_size - (reply.block_idx * (long long)block_size);
                reply.len = (size_t)(remaining_bytes < (long long)block_size ? remaining_bytes : (long long)block_size);
                send_msg(msqid, &reply);
            }
        } else if (m.mtype == RESULT_TYPE) {
            pid_t worker_pid = m.pid;

            /* record this worker's processed count as final */
            int slot = -1;
            for (int j = 0; j < num_workers; ++j) {
                if (wc[j].pid == worker_pid) { slot = j; break;}
                if (wc[j].pid == 0) { slot = j; break; }
            }
            if (slot >= 0) {
                wc[slot].pid = worker_pid;
                wc[slot].count = m.blocks_processed;
            }

            if (m.mismatch) {
                if (!mismatch_found) {
                    mismatch_found = 1;
                    mismatch_block = m.block_idx;
                    /* we will send stop messages to workers when they next request jobs
                     * (they will send READY and parent will reply block_idx=-1).
                     * No immediate broadcast is necessary because we employ request-response.
                     */
                }
                /* treat this worker as done (it will exit shortly) */
                workers_done++;
            } else {
                /* m.block_idx == -1 indicates worker says done */
                if (m.block_idx == -1) {
                    workers_done++;
                } else {
                    /* Shouldn't happen: parent doesn't expect non-mismatch results aside from done */
                }
            }
        } else {
            /* Unexpected message type: ignore */
        }
    }

    /* wait for all children to exit to avoid zombies */
    for (int i = 0; i < num_workers; ++i) {
        if (g_children[i] > 0) {
            int status = 0;
            waitpid(g_children[i], &status, 0);
        }
    }

    /* cleanup message queue and allocations */
    cleanup_resources();

    /* Print final report */
    if (mismatch_found) {
        printf("Result: FAILURE (mismatch at block index %lld)\n", mismatch_block);
    } else {
        printf("Result: SUCCESS (files identical)\n");
    }
    printf("Blocks total: %lld (block size %zu)\n", num_blocks, block_size);
    printf("Per-process blocks processed:\n");
    for (int i = 0; i < num_workers; ++i) {
        if (wc[i].pid != 0) {
            printf("  PID %d : %d blocks\n", wc[i].pid, wc[i].count);
        }
    }

    free(wc);
    free(worker_pids_seen);
    free(g_children);
    return mismatch_found ? 1 : 0;
}
