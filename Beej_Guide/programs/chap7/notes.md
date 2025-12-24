## 7.1. Blocking
- default behaviour of socket calls is set to "blocking" type by kernel
- non-blocking behaviour can be enabled by:`fcntl(sockfd, F_SETFL, O_NONBLOCK);` where:
    - `fcntl()` system call in C programming
    - `F_SETFL` is a command used with the fcntl() function to modify the status flags associated with an open file descriptor (FD). 
- Example Flags That Can Be Set:
    - `O_NONBLOCK`: Makes file operations non-blocking.
    - `O_APPEND`: Forces all writes to append to the end of the file.
    - `O_ASYNC`: Enables the generation of a signal (SIGIO) when input or output is possible.
