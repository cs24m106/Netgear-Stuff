#define _GNU_SOURCE // issue with sigaction not recognized error https://github.com/Microsoft/vscode/issues/71012 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdbool.h>

#define DEF_BLOCK_SIZE 1024

typedef struct msg_buffer{ // for better accessibility without having to add prefix struct in std declaration
    long mtype; // long "type" --> must required, any memeber below is "data" part
    bool mismatch;
    int blocks;
} msg_buffer; // // use typedef for alias on same struct name itself

msg_buffer msg;// global msg_buf for interupt handler access
int msgid; // global msg_que_id for interupt handler access
pid_t gpid; // fork distinguisher, for parent != 0, for all childs == 0

static void send_msg(int msqid, msg_buffer *m) {
    if (msgsnd(msqid, m, sizeof(msg_buffer) - sizeof(long), 0) == -1) {
        /* size of "data" part of the msg_buffer => total struct size - "type" size (long)
        If parent removed queue, children might get EINVAL; just exit gracefully */
        perror("msgsnd");
        _exit(3);
    }
}

static void term_handler(int signo) { // termination handler for childrens (when parent calls kill)
    (void)signo; // ignore unused variable warning
    printf("Process %d terminated with no.of processed blocks = %d!\n", getpid(), msg.blocks);
    send_msg(msgid, &msg); // send proccessed data before exiting
    exit(1);
}


// child/worker fn handler
int compareFileContent(const char* fname1, const char* fname2, size_t start, size_t end, size_t blocksize)
{
    printf("Child worker (PID:%d) initiated with block[start:%010lu,end:%010lu] successfully!\n", getpid(), start, end);
    // termination signal handler for childrens
    struct sigaction sa;
    sa.sa_handler = term_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // use global msg struct for term hander to send blocks processed before exiting
    msg.mtype = 1;
    msg.mismatch = false;
    msg.blocks = 0;
    
    FILE *f1, *f2;   
    f1 = fopen(fname1,"r");
    f2 = fopen(fname2,"r");
    fseek(f1, start, SEEK_SET);
    fseek(f2, start, SEEK_SET);
    
    char *buf1 = malloc(sizeof(char)*blocksize);
    char *buf2 = malloc(sizeof(char)*blocksize);
    size_t current = start;

    while (current < end){
        int read_size = (end - current < blocksize) ? end-current : blocksize;
        fread(buf1, 1, read_size, f1);
        fread(buf2, 1, read_size, f2);
        msg.blocks++;

        if(memcmp(buf1, buf2, read_size) != 0){
            msg.mismatch = true;
            send_msg(msgid, &msg); 
            fclose(f1);
            fclose(f2);
            free(buf1); free(buf2);
            exit(1);
        }
        current += read_size;
    }
    fclose(f1); fclose(f2);
    free(buf1); free(buf2);
    printf("Child Process %d completed fully with no.of processed blocks = %d!\n", getpid(), msg.blocks);
    exit(0); // exit on child
}


size_t lenOfFile(FILE *f1){
    size_t len;
    fseek(f1,0,SEEK_END);
    len = ftell(f1);
    fseek(f1,0,SEEK_SET);
    return len;
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: %s <file1> <file2> <num_workers> [block_size]\n", argv[0]);
        return 1;
    }

    const char *file1 = argv[1];
    const char *file2 = argv[2];
    int num_p = atoi(argv[3]);
    if (num_p <= 0) num_p = 1;

    size_t block_size = DEF_BLOCK_SIZE;
    if (argc == 5) {
        long tmp = atol(argv[4]);
        if (tmp > 0) block_size = (size_t)tmp;
    }

    int blocks_processed=0;    
    FILE *f1, *f2;
    
    f1 = fopen(file1, "r");
    if(f1 == NULL)
        printf("Error opening file 1.\n");
    f2 = fopen(file2, "r");
    if(f2 == NULL)
        printf("Error opening file 2.\n");

    size_t len1 = lenOfFile(f1);
    size_t len2 = lenOfFile(f2);
    fclose(f1); fclose(f2);
    
    printf("Bytes read in F1: %ld\n",len1);
    printf("Bytes read in F2: %ld\n",len2);
    
    if(len1 != len2){
        printf("Files differ in size.\n");
        printf("Failure...\nNumber of Process used = 1\n");
        exit(1);
    }

    msgid = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
    size_t segement_size = len1/num_p;
    size_t start,end;

    printf("Files sizes are same, checking incontents...\n"
        "...block by block (block_size=%lu) per process (num_processes=%d).\n"
        "Each process read equally split sections (size:%ld) of the file.\n\n", block_size, num_p, segement_size);

    for(int i=0;i<num_p;i++){
        start = i*segement_size;
        end = (i==num_p-1) ? len1 : (i+1)*segement_size; // handle last process excess data

        gpid = fork();
        if (gpid < 0) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        }
        if(gpid == 0){
            compareFileContent(file1, file2, start, end, block_size); // child fn must have exit at end of fn
        }
    }
    if (gpid>0)
        signal(SIGTERM, SIG_IGN); // Ignore the signal in the parent Only

    int finished_children = 0;
    bool p_mismatch = false;
    int t_blocks = 0;

    while(finished_children < num_p){
        if(msgrcv(msgid, &msg, sizeof(msg_buffer) - sizeof(long), 1, IPC_NOWAIT) != -1){
            if(msg.mismatch == true){
                p_mismatch = true;
                t_blocks += msg.blocks;
                kill(0, SIGTERM); // kills all processes along with parent (but only parent is set to ignore killsignal)
            }
        }

        if(waitpid(-1, NULL, WNOHANG) > 0){ // reap child zombies from process table
            finished_children++;
        }
    }
    msgctl(msgid, IPC_RMID, NULL); // clean up msg queue
    //free(fname1); free(fname2); // clean up buffers used to store filenames
    
    if(!p_mismatch){
        printf("\nSUCCESS...No Mismatch Found!");
    }
    else{
        printf("\nFAILURE...Mismatch Found!");
    }
    printf("Num of Blocks Processed: %d\n", t_blocks);
    exit(0);
}