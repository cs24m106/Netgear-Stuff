## c perror vs fprintf stderr

In C programming, both  and  are used to output error messages to the standard error stream (), but they serve different purposes and have distinct behaviors.

| Feature | `perror("...")` | `fprintf(stderr, ...)` |
| --- | --- | --- |
| Purpose | Prints a system error message based on the current value of the global variable . | Prints a custom, formatted string to the standard error stream, providing complete control over the output.  |
| Output Content | Outputs a user-supplied string, followed by a colon, a space, a language-dependent system error description (from ), and a newline character. | Outputs exactly the formatted string and arguments you provide.  |
| integration | Automatically accesses and interprets the  value. | Requires manual retrieval of  and formatting it with  if you want to include the system error message.  |
| Flexibility | Less flexible; designed specifically for C library and system call errors. | Highly flexible; can print any kind of message (error, warning, debug info, etc.) with full formatting capabilities.  |
| Example | could output: . | outputs the custom message.  |

When to use each: 
- Use  immediately after a C standard library function or system call that sets the  variable to automatically get the relevant system error description. This is the standard, convenient way to report system-related errors. 
- Use  when you need to report application-specific errors, provide custom error details (like line numbers or specific variable values), or when the error condition does not involve a function that sets.

In essence,  is a specialized wrapper for , making it a more concise and standardized way to handle system errors.

Ref:
[1](https://stackoverflow.com/questions/40677589/whats-difference-between-perror-and-fprintf-to-stderr)
[2](https://www.youtube.com/watch?v=bJmsqBDAuXw)
[3](https://stackoverflow.com/questions/12102332/when-should-i-use-perror-and-fprintfstderr)
[4](https://stackoverflow.com/questions/12102332/when-should-i-use-perror-and-fprintfstderr)
[5](https://code-reference.com/c/stdio.h/perror)
[6](https://stackoverflow.com/questions/58161315/error-difference-between-perror-and-fprintf)
[7](https://www.geeksforgeeks.org/c/perror-in-c/)
[8](https://www.qnx.com/developers/docs/6.5.0SP1/neutrino/lib_ref/p/perror.html)
[9](https://www.ibm.com/docs/en/zos/3.1.0?topic=functions-fprintf-printf-sprintf-format-write-data)
[10](https://stackoverflow.com/questions/12102332/when-should-i-use-perror-and-fprintfstderr)
[11](https://stackoverflow.com/questions/14484214/is-there-a-reason-why-perror-isnt-widely-seen-in-code-for-error-handling)
[12](https://users.cs.cf.ac.uk/Dave.Marshall/C/node18.html)

## Error Handling
Why sendto() succeeds even when destination is unreachable: UDP is connectionless; sendto() merely hands data to the kernel to transmit. If the network path fails (e.g., router unreachable) or the remote host is down, the kernel may receive asynchronous ICMP messages later — sendto() itself does not wait for delivery. This is why sendto() returning success does not mean delivery occurred. 

IP_RECVERR / IPV6_RECVERR + MSG_ERRQUEUE: On Linux you can set the socket option to ask the kernel to queue extended errors related to your outgoing packets. You then call recvmsg(..., MSG_ERRQUEUE) to read those errors; ancillary data contains struct sock_extended_err with type/code and errno and may include the offending address. This is how user-space can learn about ICMP port/host unreachable and more. This is the mechanism used in the example above. See recvmsg(2) and ip(7) / ipv6(7) man pages for details. 

connect() with a UDP socket: If you connect() a UDP socket, the kernel may map asynchronous ICMP "port unreachable" into an immediate error on subsequent send() calls (typically ECONNREFUSED) on Linux. That can be useful, but it is not guaranteed to cover all cases and timing matters. Using connect() also simplifies calling send() instead of sendto().