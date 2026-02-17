#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#define DEF_BLOCK_SIZE 1024

typedef struct msg_buffer{ // for better accessibility without having to add prefix struct in std declaration
    const char* fname1; 
    const char* fname2; 
    size_t start;
    size_t end;
    int blocks;
} ThreadsArg; // // use typedef for alias on same struct name itself

/* Shared control state */
static atomic_bool mismatch_flag = ATOMIC_VAR_INIT(false); // r&w
static size_t block_size; // r-only

/* Helper to get/set mismatch flag safely */
static inline void set_mismatch_flag(bool value){
    atomic_store_explicit(&mismatch_flag, value, memory_order_seq_cst);
}

// child/worker fn handler
void* compareFileContent(void *args)
{
    ThreadsArg *targs = (ThreadsArg *) args;
    printf("1. Child worker (TID:%ld) initiated with block[start:%010lu,end:%010lu] successfully!\n", pthread_self(), targs->start, targs->end);

    FILE *f1, *f2;   
    f1 = fopen(targs->fname1,"r");
    f2 = fopen(targs->fname2,"r");
    fseek(f1, targs->start, SEEK_SET);
    fseek(f2, targs->start, SEEK_SET);
    
    targs->blocks = 0;
    char *buf1 = malloc(sizeof(char)*block_size);
    char *buf2 = malloc(sizeof(char)*block_size);
    size_t curr = targs->start;
    bool i_found = false;

    while (curr < targs->end){
        if (mismatch_flag) break; // break if mismatch is already found by other thread

        int read_size = (targs->end - curr < block_size) ? targs->end-curr : block_size;
        fread(buf1, 1, read_size, f1);
        fread(buf2, 1, read_size, f2);
        targs->blocks++;

        if(memcmp(buf1, buf2, read_size) != 0){
            set_mismatch_flag(true);
            i_found = true;
            break;
        }
        curr += read_size;
    }
    
    fclose(f1); fclose(f2);
    free(buf1); free(buf2);
    printf("2. Child Thread (TID:%ld) %s with no.of processed blocks = %d!\n", pthread_self(), 
    (curr==targs->end||i_found)?"completed":"terminated", targs->blocks);
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
    int num_t = atoi(argv[3]);
    if (num_t <= 0) num_t = 1;

    block_size = DEF_BLOCK_SIZE;
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
    
    pthread_t *threads = malloc(num_t* sizeof(pthread_t));
    ThreadsArg *targs = malloc(num_t * sizeof(ThreadsArg));

    size_t segement_size = len1/num_t;
    size_t start,end;

    printf("Files sizes are same, checking incontents...\n"
        "...block by block (block_size=%lu) per process (num_processes=%d).\n"
        "Each process read equally split sections (size:%ld) of the file.\n\n", block_size, num_t, segement_size);
    
    for(int i=0;i<num_t;i++){
        targs[i].fname1 = file1;
        targs[i].fname2 = file2;
        targs[i].start = i*segement_size;
        targs[i].end = (i==num_t-1) ? len1 : (i+1)*segement_size; // handle last process excess data
        pthread_create(&threads[i], NULL, compareFileContent, &targs[i]);
    }

    //wait for threads to complete
    for(int i=0;i<num_t;i++){
        pthread_join(threads[i],NULL); 
        // no need for signal/pthread_cancel as once mismatch flag is set automatically all threads stop processing
    }

    int total_blocks = 0; // total no.of blocks actually processed

    for(int i=0;i<num_t;i++){
        total_blocks += targs[i].blocks;
    }

    free(threads); free(targs);
    //free(fname1); free(fname2); // clean up buffers used to store filenames
    
    if(!mismatch_flag){
        printf("\nSUCCESS...No Mismatch Found! ");
    }
    else{
        printf("\nFAILURE...Mismatch Found! ");
    }
    printf("Num of Blocks Processed: %d, using %d no.of processes.\n", total_blocks, num_t);
}