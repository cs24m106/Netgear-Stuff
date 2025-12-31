#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

int numberOfOccurence(char* line, char* word){
    int n;
    int len = strlen(word);
    for(int i=0; line[i] != '\0'; i++){
        if(strstr(&line[i], word) == &line[i]){
            n++;
            i += len -1;
        }
    }
    return n;
}

char* replaceText(char *line, char* word1, char* word2){
    int i, wlen1, wlen2;
    wlen1 = strlen(word1);
    wlen2 = strlen(word2);
    int linelen = strlen(line);
    int n = numberOfOccurence(line, word1);

    char *result = malloc(sizeof(char)*(linelen+n*(abs(wlen1 - wlen2)) + 1));

    i =0;
    while (*line){
        if(strstr(line,word1) == line){
            strcpy(&result[i],word2);
            i += wlen2;
            line += wlen1;
        }else{
            result[i++] = *line++;
        }
    }

    result[i] = '\0';
    return result;
}

void readFileToBlocks(FILE* f, int len, int n, char* shared_content){
    char* block;
    
    for(int i=0;i<(len/n + 1);i++){
        fread(shared_content+(i*n), sizeof(char), n,f);
    }
    fclose(f);
}

void printBlocks(char ** content, int n){
    for(int i =0; i<n; i++){
        puts(content[i]);
    }
}

int lenOfFile(FILE *f1){
    int len;
    fseek(f1,0,SEEK_END);
    len = ftell(f1);
    fseek(f1,0,SEEK_SET);
    return len;
}

void replaceTextlines(char *content, char *word1, char* word2, int start, int end){
    char* line, *newline;

    for(int i=start; i<end; i++){
        line = content+(i*100);
        newline = replaceText(line, word1,word2);
        strncpy(content + i*100, newline,99);
        content[(i*100+99)] = '\0';
        //printf("%s", (content+ i*100));
        //printf("Process %d replaced line %d: %s\n", getpid(), i, content + (i * 100));
    }
}

int main(){

    char fname[20];
    //char line[100];
    char word1[20];
    char word2[20];
    char *shared_content;

    char *line, *newline;
    int num_p;  
    pid_t pid;  

    printf("Enter the file name: ");
    scanf("%s",fname);

    //printf("Enter the line: ");
    //gets(line);

    FILE* f, *new_f;

    f = fopen(fname, "r");
    int len = lenOfFile(f);
    int numOfLines = len/100 + 1;

    printf("Number of Process: ");
    scanf("%d",&num_p);

    printf("File Size: %d\nNumber of lines: %d\n",len, numOfLines);
    

    printf("Enter the old word: ");
    scanf("%s",word1);
    printf("Enter the new word: ");
    scanf("%s",word2);

    int segment_size = numOfLines/num_p;
    int start, end;
    int shm_size = 100*numOfLines;
    
    //Shared Memory Concept::
    shared_content = mmap(NULL,shm_size,PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);


    readFileToBlocks(f,len, 100, shared_content);
    
    for(int i=0;i<num_p;i++){
        start = i*segment_size;
        end = (i==num_p-1) ? numOfLines : (i+1)*segment_size;

        pid = fork();
        if(pid == 0){
            //child process
            replaceTextlines(shared_content,word1,word2,start, end);
            exit(0);
        }
    }
    
    for(int i=0; i<num_p;i++){
        wait(NULL);
    }
    
    new_f = fopen("new.txt", "w");
    for(int i=0; i<numOfLines;i++){
        line = shared_content + i*100;
        if(strlen(line) > 0)
            fputs(line,new_f);
            //puts(line);
    }
    fclose(new_f);
}


/*
    for(int i=0; i<numOfLines; i++){
        line = content[i];
        newline = replaceText(line, word1,word2);
        content[i] = newline;
        printf("%s", content[i]);
    }*/