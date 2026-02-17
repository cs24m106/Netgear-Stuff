#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

int numberOfOccurence(char* line, char* word){
    int n=0;
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

void readFileToBlocks(FILE* f, int len, int n, char** content){
    char* block;

    for(int i=0;i<(len/n + 1);i++){
        content[i] = (char*) malloc(sizeof(char)*n);
        fread(content[i], sizeof(char), n,f);
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

typedef struct ThreadArgs{
    char **content;
    char *word1,*word2;
    int start, end;
}ARGS;

void* replaceTextlines(void *args){
    ARGS *arg = (ARGS *) args;

    char* line, *newline;

    for(int i=arg->start; i<arg->end; i++){
        line = arg->content[i];
        newline = replaceText(line, arg->word1,arg->word2);
        arg->content[i] = newline;
    }
}

int main(){
    char fname[20];

    char word1[20];
    char word2[20];
    char** content;
    char *line, *newline;
    int num_t;  

    printf("Enter the file name: ");
    scanf("%s",fname);

    FILE* f, *new_f;

    f = fopen(fname, "r");
    int len = lenOfFile(f);
    int numOfLines = len/100 + 1;

    printf("Number of Threads: ");
    scanf("%d",&num_t);

    printf("File Size: %d\nNumber of lines: %d\n",len, numOfLines);
    

    printf("Enter the old word: ");
    scanf("%s",word1);
    printf("Enter the new word: ");
    scanf("%s",word2);

    pthread_t pid[num_t];
    ARGS arg[num_t];

    content = (char **) malloc(sizeof(char *) * numOfLines);
    int segment_size = numOfLines/num_t;
    int start, end;
   
    readFileToBlocks(f,len, 100, content);
    
    for(int i=0;i<num_t;i++){
        arg[i].start = i*segment_size;
        arg[i].end = (i==num_t-1) ? numOfLines : (i+1)*segment_size;
        arg[i].word1 = word1;
        arg[i].word2 = word2;
        arg[i].content = content;

        pthread_create(&pid[i], NULL, replaceTextlines, &arg[i]);
    }
    
    for(int i=0; i<num_t;i++){
        //wait
        pthread_join(pid[i], NULL);
    }
    
    new_f = fopen("new.txt", "w");
    for(int i=0; i<numOfLines;i++){
        line = content[i];
        if(strlen(line) > 0)
            fputs(line,new_f);
            //puts(line);
    }
    fclose(new_f);
}

