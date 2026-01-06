#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>

#define DEF_BLOCK_SIZE 1024
#define DEF_LINE_LENGTH 100
#define LINE_SPLITTER "-\n"

typedef struct msg_buffer{ // for better accessibility without having to add prefix struct in std declaration
    const char* fname;
    const char* word1;
    const char* word2;
    unsigned int tno; // thread number
    size_t base;
    unsigned int n_changes; // no.of words replaced
} ThreadsArg; // // use typedef for alias on same struct name itself

/* Shared control state (all read only) */
static size_t block_size;
static size_t file_size;
static unsigned int line_len;
static char** shared_content; // r&w

unsigned int CountOccurences(const char* line, const char* word){
    unsigned int n=0, len=strlen(word);
    const char *p = line;
    while ((p = strstr(p, word)) != NULL) {
        n++;
        p += len;
    }
    return n;
}

// inplace replace text for all occurances (pass by ref)
void ReplaceText(char** str, const char* text, const unsigned int count, const char* rtxt){
    char *line = *str;
    int slen = strlen(line); // string length
    int wlen = strlen(text); // word length
    if (count == 0) return;
    
    int rlen = strlen(rtxt); // replacing word length
    int nlen = slen + count * (rlen - wlen) + 1; // replacing string length
    // add 1 for '\0' so that strlen fn can work properly
    char* rline = malloc(sizeof(char) * nlen);
    
    int i = 0;
    while (*line){
        const char* index = strstr(line,text);
        if(index == line){
            strcpy(&rline[i],rtxt);
            i += rlen;
            line += wlen;
        }else if(index)
        {
            size_t shift = index - line;
            strncpy(&rline[i], line, shift);
            i+= shift;
            line = (char*)index;
        }
        else{
            size_t leftover = strlen(line);
            strncpy(&rline[i], line, leftover);
            i += leftover;
            line += leftover;
        }
    }
    rline[i] = '\0';
    free(*str); // free up prev buffer
    *str = rline;
    // realloc might not guarentee same pointer location all the time
}

void CompressSeparators(char *str) {
    char *read_ptr = str;
    char *write_ptr = str;

    while (*read_ptr) {
        if (*read_ptr == '\n'){
            if (read_ptr > str && *(read_ptr-1) == '-'){ // hiphen as last char of a line => word continued onto next, 
                write_ptr--;// ignore both '-' and '\n'
                read_ptr++;
                continue;
            }
            *write_ptr++ = '\n'; // compress multiple newlines
            while(isspace((unsigned char)*++read_ptr));
        }
        else if (isspace((unsigned char)*read_ptr)) {
            *write_ptr++ = ' '; // compress white spaces
            while(isspace((unsigned char)*++read_ptr));
        } else {
            *write_ptr++ = *read_ptr++; // copy non-white space characters as it is
        }
    }
    *write_ptr = '\0'; // Null-terminate the resulting string
}

char* FormatLineBreaks(char *str, int max_len){
    int max_breaks = ((strlen(str) + max_len -1) / (max_len));
    char *fstr = malloc(sizeof(char) * (strlen(str) + (max_breaks*strlen(LINE_SPLITTER)) + 1));
    char *rstream = str; 
    char *wstream = fstr;

    while(*rstream != '\0'){
        const char *nxtl = strchr(rstream, '\n'); // next new line
        if (nxtl == NULL) nxtl = rstream + strlen(rstream); // if not found
        
        int len; // nbreaks;
        while(rstream != nxtl){
            len = (int)(nxtl - rstream);
            
            if (len==1){ // skip multiple readlines if any
                rstream++; break;
            }
            else if (len > max_len){
                char *peek = rstream + max_len;
                // peek till next whitespace to add line break
                while(peek>rstream && !isspace((unsigned char)*peek)) peek--; 
                
                if (peek == rstream) {
                    // Special Case: Word is longer than max_len;
                    len = max_len - strlen(LINE_SPLITTER);
                    strncpy(wstream, rstream, len);
                    rstream += len; wstream += len;
                    strcpy(wstream, LINE_SPLITTER); 
                    wstream += strlen(LINE_SPLITTER);    
                }
                else {
                    len = (int)(peek - rstream);
                    strncpy(wstream, rstream, len);
                    rstream = peek+1; wstream += len;
                    *wstream++ = '\n';
                }
            }
            else {
                strncpy(wstream, rstream, len);
                rstream += len; wstream += len;
            }
        }
        if (*rstream == '\n') *wstream++ = *rstream++;
    }
    *wstream = '\0'; // Null-terminate the resulting string
    return fstr;
}

// child/worker fn handler
void* replaceFileContent(void *args)
{
    ThreadsArg *targs = (ThreadsArg *) args;    
    size_t bytes_read, start;    
    
    start = targs->base + targs->tno * block_size;
    if (start > file_size) return NULL; // break if seek point is beyond eof

    FILE *f = fopen(targs->fname,"r");
    fseek(f, start, SEEK_SET);
    printf("1. Child(TID:%d) initiated with block[start:%010lu,end:%010lu] successfully!\n", 
        targs->tno, start, start+block_size);
    
    // 1. READ FILE INPUT
    targs->n_changes = 0;
    char* buf = malloc(sizeof(char) * (block_size+1)); // read onto shared buffer and replace directly on it
    bytes_read = fread(buf, sizeof(char), block_size, f);
    fclose(f); buf[bytes_read] = '\0'; // to handle EOF
    //printf(">>> (TID:%d) inital addr: %p\n", targs->tno, buf);

    // 2. Compress Separators
    CompressSeparators(buf);
    printf("2. Child(TID:%d): CompressSeparators done!\n", targs->tno);

    // 3. Replace Word
    targs->n_changes += CountOccurences(buf, targs->word1);
    ReplaceText(&buf, targs->word1, targs->n_changes, targs->word2);
    printf("3. Child(TID:%d): ReplaceText done!\n", targs->tno);

    // 4. Line Breaks
    char* fbuf = FormatLineBreaks(buf, line_len);
    free(buf);
    printf("4. Child(TID:%d): FormatLineBreaks done!\n", targs->tno);
    
    //printf(">>> (TID:%d) final addr: %p\n", targs->tno, buf);
    shared_content[targs->tno] = fbuf; // save ref onto share buffer
    printf("5. Child(TID:%d) fully completed with no.of replaced words = %d!\n", 
        targs->tno, targs->n_changes);
    return NULL;
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
    if (argc < 5 || argc > 7) {
        fprintf(stderr, "Usage: %s <file> <word1> <word2> <num_workers> [block_size] [line_len]\n", argv[0]);
        return 1;
    }

    const char *fname = argv[1];
    const char *word1 = argv[2];
    const char *word2 = argv[3];
    int n_threads = atoi(argv[4]);
    if (n_threads <= 0) n_threads = 1;

    block_size = DEF_BLOCK_SIZE;
    if (argc >= 6) {
        long tmp = atol(argv[5]);
        if (tmp > 0) block_size = (size_t)tmp;
    }
    line_len = DEF_LINE_LENGTH;
    if (argc == 7) {
        int tmp = atoi(argv[6]);
        if (tmp > 0) line_len = tmp;
    }
    file_size = FileLen(fname);
    printf("Bytes read in File: %ld\n",file_size);

    unsigned int n_blocks = (file_size + block_size - 1)/block_size; // round up / ceil
    unsigned int n_loop = (n_blocks + n_threads - 1)/n_threads;
    shared_content = (char**) malloc(sizeof(char*) * n_threads);
    
    pthread_t *threads = malloc(n_threads* sizeof(pthread_t));
    ThreadsArg *targs = malloc(n_threads * sizeof(ThreadsArg));

    printf("Given Config: block_size=%lu, num_processes=%d.\n"
            "No. of blocks:%u, No. of loops:%u.\n\n", block_size, n_threads, n_blocks, n_loop);
    
    size_t base=0, multiple = n_threads*block_size;
    char new_fname[20], *block_content;
    int fnlen = strlen(fname);
    strncpy(new_fname, fname, fnlen); new_fname[fnlen] = '\0';
    strcat((char*)new_fname,"_rnew.txt");
    FILE *rnew_file = fopen(new_fname, "w");

    int total_changes = 0; // total no.of replaced texts
    for(unsigned int k=0; k<n_loop; k++){
        memset(shared_content, 0, sizeof(char*) * n_threads); // reset to keep track of processed data

        for(int i=0;i<n_threads;i++){            
            targs[i].fname = fname;
            targs[i].word1 = word1;
            targs[i].word2 = word2;
            targs[i].tno = i;
            targs[i].base = base;
            pthread_create(&threads[i], NULL, replaceFileContent, &targs[i]);
        }
        
        //wait for threads to complete
        for(int i=0;i<n_threads;i++){
            pthread_join(threads[i],NULL);
            total_changes += targs[i].n_changes;
        }        
        base += multiple;

        // write-back all processed thread-block content onto new file from main thread only
        for(int i=0; i<n_threads;i++){
            if (shared_content[i] != NULL) {

                block_content = shared_content[i];
                //printf(">>> tid:%d, new addr: %p\n", i, block_content);
                if(strlen(block_content) > 0)
                    fputs(block_content,rnew_file);
                free(block_content);
            }
        }
    }
    fclose(rnew_file);
    free(threads); free(targs);
    printf("\nNum of changes applied: %d, using %d no.of threads.\n", total_changes, n_threads);
}