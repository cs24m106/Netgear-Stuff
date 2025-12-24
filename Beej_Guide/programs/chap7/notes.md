## 7.1. Blocking
- default behaviour of socket calls is set to "blocking" type by kernel
- non-blocking behaviour can be enabled by:`fcntl(sockfd, F_SETFL, O_NONBLOCK);` where:
    - `fcntl()` system call in C programming
    - `F_SETFL` is a command used with the fcntl() function to modify the status flags associated with an open file descriptor (FD). 
- Example Flags That Can Be Set:
    - `O_NONBLOCK`: Makes file operations non-blocking.
    - `O_APPEND`: Forces all writes to append to the end of the file.
    - `O_ASYNC`: Enables the generation of a signal (SIGIO) when input or output is possible.

## 7.3 `select()`
The first parameter (nfds) means: 
- One greater than the highest-numbered file descriptor you want select() to check.

It does NOT mean: 
- number of file descriptors in the set
- size or length of fd_set
- index count

It is strictly: `nfds = max_fd_value + 1`

**Why file descriptor numbers matter?**<br>
In Unix-like systems:
- Every open file/socket/device is identified by a small integer file descriptor. 
- Examples:
    - stdin → fd 0
    - stdout → fd 1
    - stderr → fd 2
    - sockets typically start at 3 and increase

`fd_set` is conceptually a bitset indexed by file descriptor numbers, not a dynamically-sized container. <br>
Internally (simplified): `fd_set ≈ bitmask[0 ... FD_SETSIZE-1]` <br>
Each bit corresponds to one specific file descriptor number.

*"Some Unices update the time in your struct timeval to reflect the amount of time still remaining before a
timeout. But others do not. Don’t rely on that occurring if you want to be portable. (Use gettimeofday()
if you need to track time elapsed. It’s a bummer, I know, but that’s the way it is.)<br>
What happens if a socket in the read set closes the connection? Well, in that case, select() returns with
that socket descriptor set as “ready to read”. When you actually do recv() from it, recv() will return 0.
That’s how you know the client has closed the connection."*

- for both new client conn --> read from listener socker, i.e. add to readfds
- and for old client disconn, --> read from resp client gives nbytes=0 on recv, implies client disconnected
- conclusion in common, having to add both situation type socket_fds into readfds fd_set