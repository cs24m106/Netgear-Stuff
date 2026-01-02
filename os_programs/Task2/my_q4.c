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
    pid_t pid; // fork distinguisher, for parent = 0, for all childs = getpid()
    bool mismatch;
    int blocks;
} msg_buffer; // // use typedef for alias on same struct name itself

msg_buffer msg;// global msg_buf for interupt handler access
int msgid; // global msg_que_id for interupt handler access

static void send_msg(int msqid, msg_buffer *m) {
    if (msgsnd(msqid, m, sizeof(msg_buffer) - sizeof(long), 0) == -1) {
        /* size of "data" part of the msg_buffer => total struct size - "type" size (long)
        If parent removed queue, children might get EINVAL; just exit gracefully */
        perror("msgsnd");
        exit(EXIT_FAILURE);
    }
}

static void term_handler(int signo) { // termination handler for childrens (when parent calls kill)
    (void)signo; // ignore unused variable warning
    msg.mismatch = true; // update mismatch as termination reqestion bool when parent calls kill
}


// child/worker fn handler
void compareFileContent(const char* fname1, const char* fname2, size_t start, size_t end, size_t blocksize)
{
    msg.pid = getpid();
    printf("1. Child worker (PID:%d) initiated with block[start:%010lu,end:%010lu] successfully!\n", msg.pid, start, end);

    FILE *f1, *f2;   
    f1 = fopen(fname1,"r");
    f2 = fopen(fname2,"r");
    fseek(f1, start, SEEK_SET);
    fseek(f2, start, SEEK_SET);
    
    char *buf1 = malloc(sizeof(char)*blocksize);
    char *buf2 = malloc(sizeof(char)*blocksize);
    size_t curr = start;

    while (curr < end){
        if (msg.mismatch) break; // break if mismatch is already found by other processes
        // i.e. parent term req means evident tat mismatch found already

        int read_size = (end - curr < blocksize) ? end-curr : blocksize;
        fread(buf1, 1, read_size, f1);
        fread(buf2, 1, read_size, f2);
        msg.blocks++;

        if(memcmp(buf1, buf2, read_size) != 0){
            msg.mismatch = true;
            break;
        }
        curr += read_size;
    }
    
    send_msg(msgid, &msg); // send proccessed data before exiting
    fclose(f1); fclose(f2);
    free(buf1); free(buf2);
    printf("2. Child Process (PID:%d) %s with no.of processed blocks = %d!\n", msg.pid, (curr!=end)?"terminated":"completed", msg.blocks);
    exit(0); // exit on child
}


size_t FileLen(const char *file){
    size_t len = 0;
    FILE *f = fopen(file, "r");
    if(f == NULL)
        printf("Error opening file: %s.\n", file);
    fseek(f,0,SEEK_END);
    len = ftell(f);
    fseek(f,0,SEEK_SET);
    fclose(f);
    return len;
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: %s <file1> <file2> <num_workers> [block_size]\n", argv[0]);
        return 1;
    }

    const char *file1 = argv[1];
    const char *file2 = argv[2];
    int max_p = atoi(argv[3]);
    if (max_p <= 0) max_p = 1;

    size_t block_size = DEF_BLOCK_SIZE;
    if (argc == 5) {
        long tmp = atol(argv[4]);
        if (tmp > 0) block_size = (size_t)tmp;
    }

    size_t len1 = FileLen(file1);
    size_t len2 = FileLen(file2);    
    printf("Bytes read in F1: %ld\n",len1);
    printf("Bytes read in F2: %ld\n",len2);
    
    if(len1 != len2){
        printf("Files differ in size.\n");
        printf("Failure...\nNumber of Process used = 1\n");
        exit(1);
    }

    msgid = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
    size_t segement_size = len1/max_p;
    size_t start,end;

    printf("Files sizes are same, checking incontents...\n"
        "...block by block (block_size=%lu) per process (num_processes=%d).\n"
        "Each process read equally split sections (size:%ld) of the file.\n\n", block_size, max_p, segement_size);
    
    // termination signal handler mainly for childrens
    struct sigaction sa;
    sa.sa_handler = term_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    //sigaction(SIGINT, &sa, NULL);

    // use global msg struct for term hander to send blocks processed before exiting
    msg.mtype = 1;
    msg.pid = 0;
    msg.mismatch = false;
    msg.blocks = 0;
    setbuf(stdout, NULL); // Disable buffering on stdout. why? to view child's debug printfs before it gets killed

    int num_p = 0;
    for(;num_p<max_p;){
        /*if (msg.mismatch) {
            printf("Process Forking interupted inbetween as mismatch flag already received\n");
            break;
        }*/
        start = num_p*segement_size;
        end = (num_p==max_p-1) ? len1 : (num_p+1)*segement_size; // handle last process excess data

        int fpid  = fork();
        if (fpid   < 0) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        }
        else num_p++;
        if(fpid   == 0){
            compareFileContent(file1, file2, start, end, block_size); // child fn must have exit at end of fn
        }
        //printf("%d",num_p);
    }
    //printf("\nAll child workers have been initialized successfully\n\n");

    int fin_childs = 0; // no. of childs finished sending back msgs of processed 
    bool p_mismatch = false;
    int t_blocks = 0; // total no.of processed blocks

    while(fin_childs < num_p){
        if(msgrcv(msgid, &msg, sizeof(msg_buffer) - sizeof(long), 1, IPC_NOWAIT) != -1){
            if(msg.mismatch && !p_mismatch){ // update and send kill signal only once
                p_mismatch = true;
                kill(0, SIGTERM); // kills all processes along with parent (but only parent is set to ignore killsignal)
            }
            printf("3. Parent ipc.msg recv from (pid:%d) with no.of processed blocks:%d\n",msg.pid, msg.blocks);
            t_blocks += msg.blocks;
            fin_childs++; // every child must send processed data until then.
        }

        waitpid(-1, NULL, WNOHANG); // reap child zombies from process table
    }
    msgctl(msgid, IPC_RMID, NULL); // clean up msg queue
    //free(fname1); free(fname2); // clean up buffers used to store filenames
    
    if(!p_mismatch){
        printf("\nSUCCESS...No Mismatch Found! ");
    }
    else{
        printf("\nFAILURE...Mismatch Found! ");
    }
    printf("Num of Blocks Processed: %d, using %d no.of processes.\n", t_blocks, num_p);
    exit(0);
}