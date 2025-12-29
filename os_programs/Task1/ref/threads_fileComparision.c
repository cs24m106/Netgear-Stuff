#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>

#define BLOCK_SIZE 20

typedef struct{
    char* fname1;
    char* fname2;
    int start;
    int end;
    int *mismatch;
}ThreadsArg;

void *compareFileContent(void *arg){
    ThreadsArg *data = (ThreadsArg *) arg;

    FILE *f1, *f2;   

    f1 = fopen(data->fname1,"r");
    f2 = fopen(data->fname2,"r");
    
    char *buff1 = malloc(sizeof(char)*BLOCK_SIZE);
    char *buff2 = malloc(sizeof(char)*BLOCK_SIZE);
    
    fseek(f1, data->start, SEEK_SET);
    fseek(f2, data->start, SEEK_SET);
    
    int current = data->start;


    while (current < data->end){

        if(*(data->mismatch)) break;

        int read_size = (data->end - current < BLOCK_SIZE) ? data->end-current : BLOCK_SIZE;
        fread(buff1, 1, read_size, f1);
        fread(buff2, 1, read_size, f2);

        if(memcmp(buff1, buff2, read_size) != 0){
            *(data->mismatch) = 1;
            break;
        }
        current += read_size;
    }
    fclose(f1);
    fclose(f2);
    free(buff1); free(buff2);
    return NULL;
}


int lenOfFile(FILE *f1){
    int len;
    fseek(f1,0,SEEK_END);
    len = ftell(f1);
    fseek(f1,0,SEEK_SET);
    fclose(f1);
    return len;
}

int main(){
    int num_t,n;
    char* fname1, *fname2;

    fname1 = malloc(sizeof(char)*20);
    fname2 = malloc(sizeof(char)*20);

    printf("Enter the file name: ");
    scanf("%s",fname1);
    printf("Enter the file name: ");
    scanf("%s",fname2);
    printf("Enter the number of Threads: ");
    scanf("%d",&num_t);
    
    FILE* f1;
    FILE* f2;
    
    f1 = fopen(fname1, "r");
    if(f1 == NULL)
        printf("Error opening file 1.\n");
    f2 = fopen(fname2, "r");
    if(f1 == NULL)
        printf("Error opening file 1.\n");

    int len1 = lenOfFile(f1);
    int len2 = lenOfFile(f2);
    int b_size;
    
    printf("Bytes read in F1: %d\n",len1);
    printf("Bytes read in F2: %d\n",len2);
    
    if(len1 != len2){
        printf("Files differ in size.\n");
        printf("Failure...\nNumber of threads used = 1\n");
        exit(1);
    }

    pthread_t threads[num_t];
    ThreadsArg args[num_t];

    int main_mismatch = 0;
    int block_size = len1/num_t;


    for(int i=0;i<num_t;i++){
        args[i].fname1 = fname1;
        args[i].fname2 = fname2;
        args[i].mismatch = &main_mismatch;
        args[i].start = i*block_size;
        args[i].end = (i==num_t-1) ? len1 : (i+1)*block_size;

        pthread_create(&threads[i], NULL, compareFileContent, &args[i]);
    }

    //wait for threads to complete
    for(int i=0;i<num_t;i++){
        pthread_join(threads[i],NULL);
    }
    
    if(!main_mismatch){
        printf("SUCCESS...files are same.\nNum of Threads: %d\n",num_t);
    }
    else{
        printf("Failure...files are different.\nNum of Threads: %d\n",num_t);
    }

}


