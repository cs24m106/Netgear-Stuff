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

#define BLOCK_SIZE 20

struct msg_buffer {
    long mtype;
    int mismatch;
};

int compareFileContent(char* fname1, char* fname2, int start, int end, int msgid){
    FILE *f1, *f2;   
    struct msg_buffer msg;

    f1 = fopen(fname1,"r");
    f2 = fopen(fname2,"r");
    
    char *buff1 = malloc(sizeof(char)*BLOCK_SIZE);
    char *buff2 = malloc(sizeof(char)*BLOCK_SIZE);
    
    fseek(f1, start, SEEK_SET);
    fseek(f2, start, SEEK_SET);
    msg.mtype = 1;

    int current = start;

    while (current < end){
        int read_size = (end - current < BLOCK_SIZE) ? end-current : BLOCK_SIZE;
        fread(buff1, 1, read_size, f1);
        fread(buff2, 1, read_size, f2);

        if(memcmp(buff1, buff2, read_size) != 0){
            msg.mismatch = 1;
            msgsnd(msgid, &msg, sizeof(int), 0);
            fclose(f1);
            fclose(f2);
            free(buff1); free(buff2);
            exit(1);
        }
        current += read_size;
    }
    fclose(f1);
    fclose(f2);
    free(buff1); free(buff2);
    exit(0);
}


int lenOfFile(FILE *f1){
    int len;
    fseek(f1,0,SEEK_END);
    len = ftell(f1);
    fseek(f1,0,SEEK_SET);
    return len;
}

int main(){
    int num_p,n;
    char* fname1, *fname2;

    fname1 = malloc(sizeof(char)*20);
    fname2 = malloc(sizeof(char)*20);

    printf("Enter the file name: ");
    scanf("%s",fname1);
    printf("Enter the file name: ");
    scanf("%s",fname2);
    printf("Enter the number of Process: ");
    scanf("%d",&num_p);
    
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
        printf("Failure...\nNumber of Process used = 1\n");
        exit(1);
    }

    int msgid = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
    int segement_size = len1/num_p;
    int start,end;


    for(int i=0;i<num_p;i++){
        start = i*segement_size;
        end = (i==num_p-1) ? len1 : (i+1)*segement_size;

        pid_t pid = fork();
        if(pid == 0){
            compareFileContent(fname1,fname2,start,end,msgid);
        }
    }

    int finished_children = 0;
    int parent_mismatch = 0;

    struct msg_buffer recv_msg;


    while(finished_children < num_p){
        if(msgrcv(msgid, &recv_msg, sizeof(int), 1, IPC_NOWAIT) != -1){
            if(recv_msg.mismatch == 1){
                parent_mismatch = 1;
                printf("Mismatch Found...\n");
                exit(1);
                kill(0, SIGTERM);
                break;
            }
        }

        if(waitpid(-1, NULL, WNOHANG) > 0){
            finished_children++;
        }
    }
    
    if(!parent_mismatch){
        printf("SUCCESS...files are same.\nNum of Process: %d\n",finished_children);
    }
    msgctl(msgid, IPC_RMID, NULL);
}




/*

void printBlocks(char ** content, int n){
    for(int i =0; i<n; i++){
        puts(content[i]);
    }
}

char** readFileToBlocks(FILE* f, int len, int n){
    char* block;
    char **content;
    content = (char **) malloc((len/n + 1)*sizeof(char*));

    for(int i=0;i<(len/n + 1);i++){
        content[i] = malloc(sizeof(char)*n);
        fread(content[i], sizeof(char), n,f);
    }
    
    return content;
}*/