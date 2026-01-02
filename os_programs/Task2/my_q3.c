#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
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
static size_t chunk_size;
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

// inplace replace text for all occurances
char* ReplaceText(char* line, const char* text, const unsigned int count, const char* rtxt){
    char* line = *line;
    int slen = strlen(line);
    int wlen = strlen(text);
    if (count == 0) return;
    
    int rlen = strlen(rtxt);
    int nlen = slen + count * (rlen - wlen);
    char* rline = malloc(sizeof(char) * (nlen+1)); // add 1 for '\0' so that strlen fn can work properly
    
    int i = 0;
    while (*line){
        if(strstr(line,text) == line){
            strcpy(&rline[i],rtxt);
            i += rlen;
            line += wlen;
        }else{
            rline[i++] = *line++;
        }
    }
    return rline;
}

void CompressSeparators(char *str) {
    char *read_ptr = str;
    char *write_ptr = str;

    while (*read_ptr) {
        if (*read_ptr == '\n'){
            if (*(read_ptr-1) == '-'){ // hiphen as last char of a line => word continued onto next, 
                write_ptr--;// ignore both '-' and '\n'
                read_ptr++;
                continue;
            }
            *write_ptr++ = '\n'; // compress multiple newlines
            while(isspace((unsigned char)*read_ptr++));
        }
        else if (isspace((unsigned char)*read_ptr++)) {
            *write_ptr++ = ' '; // compress white spaces
            while(isspace((unsigned char)*read_ptr++));
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
            //nbreaks = (len + max_len -1) * (max_len);

            if (len > max_len){
                char *peek = rstream + max_len;
                // peek till next whitespace to add line break
                while(peek>rstream && !isspace((unsigned char)*peek)) peek--; 
                
                if (peek == rstream) {
                    // Special Case: Word is longer than max_len;
                    len = max_len - strlen(LINE_SPLITTER);
                    memcpy(wstream, rstream, sizeof(char)*len);
                    rstream += len; wstream += len;
                    strcpy(wstream, LINE_SPLITTER); 
                    wstream += strlen(LINE_SPLITTER);    
                }
                else {
                    len = (int)(peek - rstream);
                    memcpy(wstream, rstream, sizeof(char)*len);
                    rstream = peek+1; wstream += len;
                    *wstream++ = '\n';
                }
            }
            else {
                memcpy(wstream, rstream, sizeof(char)*len);
                rstream += len; wstream += len;
            }
        }
        if (*rstream == '\n') *wstream++ = *rstream++;
    }
    *wstream = '\0'; // Null-terminate the resulting string
    return fstr;
}

// child/worker fn handler
void* compareFileContent(void *args)
{
    ThreadsArg *targs = (ThreadsArg *) args;    
    FILE *f = fopen(targs->fname,"r");
    size_t bytes_read, start;    
    
    start = targs->base + targs->tno * block_size;
    fseek(f, start, SEEK_SET);
    printf("1. Child worker (TID:%d) initiated with block[start:%010lu,end:%010lu] successfully!\n", 
        targs->tno, start, start+block_size);
    
    // READ FILE INPUT
    targs->n_changes = 0;
    char* buf = malloc(sizeof(char) * (block_size+1)); // read onto shared buffer and replace directly on it
    bytes_read = fread(buf, sizeof(char), block_size, f);
    fclose(f); buf[bytes_read] = '\0'; // to handle EOF
    
    // 1. Compress Separators
    CompressSeparators(buf);

    // 2. Replace Word
    targs->n_changes += CountOccurences(buf, targs->word1);
    char* rbuf = ReplaceText(&buf, targs->word1, targs->n_changes, targs->word2);
    free(buf);
    
    // 3. Line Breaks
    char* fbuf = FormatLineBreaks(rbuf, line_len);
    free(rbuf);
    shared_content[targs->tno] = fbuf;
    
    printf("2. Child Thread (TID:%d) completed with no.of replaced words = %d!\n", 
        pthread_self(), targs->n_changes);
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

    const char *file = argv[1];
    const char *word1 = argv[2];
    const char *word2 = argv[3];
    int num_t = atoi(argv[4]);
    if (num_t <= 0) num_t = 1;

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
    size_t len = FileLen(file);
    printf("Bytes read in File: %ld\n",len);

    chunk_size = num_t * block_size;
    unsigned int n_chunks = (len + chunk_size - 1)/chunk_size; // round up / ceil
    shared_content = (char**) malloc(sizeof(char*) * num_t);
    
    pthread_t *threads = malloc(num_t* sizeof(pthread_t));
    ThreadsArg *targs = malloc(num_t * sizeof(ThreadsArg));

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