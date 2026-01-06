#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sched.h>

#define DEF_BLOCK_SIZE 1024
#define MAX_BLOCK_SIZE block_size*2
#define DEF_LINE_LENGTH 100
#define LINE_SPLITTER "-\n"

/* global control state (all read only) */
static int pno; // local process number (parent id:-1, child ids: 0,1,2...)
static const char* fname;
static const char* word1;
static const char* word2;
static ssize_t block_size;
static ssize_t file_size;
static unsigned int line_len;
static volatile char* shared_memory; // ipc r&w with each process max shm[block_size*2]
static volatile ssize_t* base_addr; // (signed) points to shared-memory first part alone

int CountOccurences(const char* line, const char* word){
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

int ReplaceFileContent(ssize_t base, char* shared_content)
{  
    ssize_t bytes_read, start;    
    
    start = base + pno * block_size;
    if (start > file_size) return 0; // break if seek point is beyond eof

    FILE *f = fopen(fname,"r");
    fseek(f, start, SEEK_SET);
    printf("1. Child(PID:%d) initiated with block[start:%010lu,end:%010lu] successfully!\n", 
        pno, start, start+block_size);
    
    // 1. READ FILE INPUT
    char* buf = malloc(sizeof(char) * (block_size+1)); // read onto shared buffer and replace directly on it
    bytes_read = fread(buf, sizeof(char), block_size, f);
    fclose(f); buf[bytes_read] = '\0'; // to handle EOF
    //printf(">>> (PID:%d) inital addr: %p\n", pno, buf);

    // 2. Compress Separators
    CompressSeparators(buf);
    printf("2. Child(PID:%d): CompressSeparators done!\n", pno);

    // 3. Replace Word
    int n_changes = CountOccurences(buf, word1);
    ReplaceText(&buf, word1, n_changes, word2);
    printf("3. Child(PID:%d): ReplaceText done!\n", pno);

    // 4. Line Breaks
    char* fbuf = FormatLineBreaks(buf, line_len);
    free(buf);
    printf("4. Child(PID:%d): FormatLineBreaks done!\n", pno);
    
    //printf(">>> (PID:%d) final addr: %p\n", pno, buf);
    strcpy(shared_content, fbuf);
    free(fbuf);
    printf("5. Child(PID:%d) fully completed with no.of replaced words = %d!\n", pno, n_changes);
    return n_changes;
}

// child/worker fn handler
void EndlessWorker()
{
    printf(">> Child(PID:%d) EndlessWorker Started!\n", pno);
    ssize_t m_base = -1; // not init
    volatile char *my_shmem = shared_memory + sizeof(ssize_t) + pno * MAX_BLOCK_SIZE;
    int *n_changes = (int*)my_shmem;
    while(true)
    {
        if (*base_addr > file_size) break; // exit cond
        while (m_base == *base_addr) sched_yield(); // blocking call
        m_base = *base_addr;
        *n_changes = ReplaceFileContent(m_base, (char*)(my_shmem + sizeof(int)));
    }
    if (*n_changes < 0) *n_changes = 0; // safety flag to tell parent that child has finished to avoid endless wait
    printf(">> Child(PID:%d) EndlessWorker Ended!\n", pno);
    exit(1);
}

ssize_t FileLen(const char *file){
    ssize_t len = 0;
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

    fname = argv[1];
    word1 = argv[2];
    word2 = argv[3];
    int n_processes = atoi(argv[4]);
    if (n_processes <= 0) n_processes = 1;

    block_size = DEF_BLOCK_SIZE;
    if (argc >= 6) {
        long tmp = atol(argv[5]);
        if (tmp > 0) block_size = (ssize_t)tmp;
    }
    line_len = DEF_LINE_LENGTH;
    if (argc == 7) {
        int tmp = atoi(argv[6]);
        if (tmp > 0) line_len = tmp;
    }
    file_size = FileLen(fname);
    printf("Bytes read in File: %ld\n",file_size);

    unsigned int n_blocks = (file_size + block_size - 1)/block_size; // round up / ceil
    //unsigned int n_loop = (n_blocks + n_processes - 1)/n_processes;
    shared_memory = mmap(NULL, sizeof(ssize_t) + n_processes*MAX_BLOCK_SIZE, 
                          PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0); // zero-filled by default
    base_addr = (ssize_t*)shared_memory;
    printf("Given Config: block_size=%lu, num_processes=%d.\n"
            "No. of blocks:%u\n\n", block_size, n_processes, n_blocks);
    
    
    char new_fname[20];
    int fnlen = strlen(fname);
    strncpy(new_fname, fname, fnlen); new_fname[fnlen] = '\0';
    strcat((char*)new_fname,"_rnew.txt");
    FILE *rnew_file = fopen(new_fname, "w");

    setbuf(stdout, NULL); // Disable buffering on stdout. why? to view child's debug printfs before it gets killed

    // reset shared memory so child waits for parent to set args
    *base_addr = -1;
    // ResetUpdates(n_processes);
    volatile char *mem_iter = shared_memory + sizeof(ssize_t);
    for(int i=0;i<n_processes;i++){ 
        *(int*)mem_iter = -1; // set n_changes = -1 init to detect change if processing completed
        mem_iter += MAX_BLOCK_SIZE;
    }

    for(int i=0;i<n_processes;i++){ 
        pno = i; // pno of child
        pid_t pid = fork();
        if (pid < 0){
            perror("fork failed");
            return 1;
        }
        if (pid==0)
            EndlessWorker();
    }
    pno = -1; // reset pno for parent

    ssize_t multiple = n_processes*block_size;
    *base_addr = 0; // start point
    int total_changes = 0; // total no.of replaced texts
    
    while(*base_addr<file_size){
        char *block_content; 
        mem_iter = shared_memory + sizeof(ssize_t);

        for(int i=0;i<n_processes;i++){
            int *n_changes = (int*)mem_iter;
            while(*n_changes < 0) sched_yield(); // wait for child one by one to complete, blocking call for parent
            //printf("--> Worker(PID:%d) done, processing info...\n", i);
            total_changes += *n_changes;
            *n_changes = -1; // reset
            block_content = (char*)(mem_iter + sizeof(int));
            if(strlen(block_content) > 0)
                fputs(block_content,rnew_file); // write back processed block content from parent process only
            mem_iter += MAX_BLOCK_SIZE;
        }     
        
        *base_addr += multiple; // update point
    }
    fclose(rnew_file);
    // Wait for all child processes to finish
    for (int i = 0; i < n_processes; i++) {
        wait(NULL);
    }
    printf("\nNum of changes applied: %d, using %d no.of processes.\n", total_changes, n_processes);
}