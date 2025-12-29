# Description:
Classical FTP servers do work in exactly that manner, and FTP is one of the canonical examples that motivated the fork-per-client design in Unix network programming.

High-level FTP server architecture
- One long-lived master (listener) process
- One child process per client session, created using fork()
This matches your original server code’s structure almost exactly.

Why FTP fits the fork() model so well:
- Stateful (login state, current directory, transfer mode, permissions)
- Independent (one user’s session does not need to share state with others)
- Long-lived but isolated
- Security-sensitive (authentication, filesystem access)
Each of these properties aligns naturally with a process-per-session model.

The fork() per-connection pattern in your original demo is a classic concurrency model on Unix. Below I explain when that model is practically useful, what its strengths and weaknesses are, common patterns built on it, and concrete recommendations for when to keep it vs. switch to an alternative.

## Practical uses — when fork() per connection is a good choice
- **Short-lived, independent requests:** <br>
Services where a single request/transaction is handled and then the connection closes (e.g., classic inetd-style services, one-shot protocols). Starting a child for each request is simple and reliable.

- **Isolation and fault tolerance:** <br>
If a handler can crash, leak memory, or otherwise misbehave, a process crash affects only that connection. This is valuable for running third-party code or untrusted extensions.

- **Privilege separation and sandboxing** <br>
The parent can accept a connection, fork(), and the child drops privileges (setuid, chroot) and handles the client. Useful in daemons that must accept connections as root but then execute unprivileged work (e.g., ftpd, ssh session handling historically).

- **Compatibility with blocking, non-thread-safe libraries** <br>
If your connection handling uses libraries that are not thread-safe or APIs that block indefinitely, using a separate process avoids having to rewrite for async or add locking.

- **When you want to exec() a program per connection** <br>
Very common pattern: accept -> fork() -> dup2() to socket -> exec() into a program (CGI, remote shell, telnetd, rshd style behavior). inetd did exactly this.

- **Simple multi-core concurrency without explicit threading** <br>
Processes run concurrently on different CPU cores under the kernel scheduler without need for user-level thread management.

- **Mature, well-understood for low- to moderate-scale services** <br>
For services with a small number of simultaneous clients (tens to a few hundreds depending on resources), fork-per-connection is often "good enough" and easy to reason about.

## Common real-world patterns that use forking
- Process-per-connection (your original model): parent accepts, fork() child handles the client, child exits on disconnect.
- Prefork (process pool): parent creates N worker processes ahead of time; each worker accepts connections (or receives them from parent). Used by Apache prefork MPM, some database server designs.
- Accept+fork+exec: parent spawns per-connection processes that exec() specialized programs (inetd / CGI).
- Hybrid: master process manages socket, children (or threads) do heavy lifting; children may be short-lived or pooled.


# misc FAQ:
- `gai_strerror()` converts a numeric error code returned by `getaddrinfo()` into a human-readable error message. header: `netdb.h`. Typical error codes and meanings:

|Error code|Meaning|
|---|---|
|`EAI_NONAME`|Hostname or service not known|
|`EAI_AGAIN`|Temporary failure in name resolution|
|`EAI_FAIL`|Non-recoverable DNS failure|
|`EAI_MEMORY`|Out of memory|
|`EAI_FAMILY`|Unsupported address family|
|`EAI_SOCKTYPE`|Unsupported socket type|
|`EAI_SERVICE`|Service not supported for socket type|


---
---

# sigchld_handler — why it exists and how it works
Code
```c
void sigchld_handler(int s)
{
    (void)s; // quiet unused variable warning

    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;

    while (waitpid(-1, NULL, WNOHANG) > 0);

    errno = saved_errno;
}
...
struct sigaction sa;
int yes=1;
char s[INET6_ADDRSTRLEN];
int rv;
...
sa.sa_handler = sigchld_handler; // reap all dead processes
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART;
if (sigaction(SIGCHLD, &sa, NULL) == -1) {
perror("sigaction");
exit(1);
}
```

**Function signature:** `void sigchld_handler(int s)`. Why int s?<br>
Signal handlers must have this signature. `s` contains the signal number (here: `SIGCHLD`) and Required by POSIX


## What problem this solves
Every fork()ed child that exits becomes a zombie process until the parent calls wait() or waitpid().

If you do not reap zombies:
- Process table fills up
- Server eventually cannot fork
- System resources leak

## What would go wrong without the loop?

Imagine this scenario, Timeline example:
- Parent forks 3 children
- All 3 children exit very quickly
-  Kernel generates SIGCHLD, but only one signal is delivered
- Signal handler runs once
Now suppose the handler did this: `waitpid(-1, NULL, WNOHANG);`

What happens?
- waitpid() reaps only one child
- The other two zombies remain
- No more SIGCHLD signals arrive
- Zombies remain forever → resource leak
This is exactly the bug the loop prevents.

## Why the while (waitpid(...)) loop works
Let’s look at the actual line: `while (waitpid(-1, NULL, WNOHANG) > 0);`

Meaning of each part
- `pid = -1` → reap any child
- `NULL` → we don’t care about exit status
- `WNOHANG` → do not block

Return values of waitpid()
|Return value	|Meaning    |
|---|---|
|> 0	|One child was reaped   |
|0	|No exited children right now   |
|-1	|Error (or no children exist)   |

Loop behavior
- First iteration: reap 1st exited child
- Second iteration: reap 2nd exited child
- Third iteration: reap 3rd exited child
- Fourth iteration: returns 0 → stop looping

So the loop guarantees all exited children are reaped, even if only one SIGCHLD was delivered.

## Why `WNOHANG` is mandatory inside a signal handler

Signals interrupt the program at arbitrary points. If you used: `waitpid(-1, NULL, 0);`, Then:
- If no child had exited yet
- waitpid() would block
- Blocking inside a signal handler is dangerous and can deadlock the process

`WNOHANG` ensures:
- The handler never blocks
- It only reaps what is already available
- It returns immediately otherwise

This is a hard requirement for safe signal handling.

---
---

## What is a signal (very basic)
A signal is a small asynchronous notification sent by the Linux kernel to a process to tell it that something happened. Examples:
- A child process exited
- User pressed Ctrl+C
- Program accessed invalid memory
Signals are defined by integer constants.

### `SIGCHLD`
What it is? <br>
SIGCHLD is a signal sent by the kernel to a parent process when:
- A child process exits
- A child process stops or resumes

Defined in
- Header: <signal.h>
- Manual page: man 7 signal

Important property
- SIGCHLD is not queued
- Multiple child exits may generate only one signal

### Why this handler exists
Your server uses fork(). <br>
When a child exits:
- Kernel keeps its exit status
- Child becomes a zombie
- Parent must call wait() or waitpid()

If the parent does not:
- Zombies accumulate
- Eventually fork() fails

### Why SIGCHLD delivery behavior exists (design reason)
Historically:
- UNIX signals were designed to be simple notifications
- They were never intended to be counted or queued
- Kernel designers optimized for low overhead

So the contract is:
- “SIGCHLD means something changed.
- You must check what changed.”

That’s why POSIX explicitly documents that:
- SIGCHLD may be coalesced
- Handlers must loop with waitpid()


## Why SIGCHLD is used
- Kernel sends SIGCHLD to a parent when a child exits
- Installing a handler allows the parent to clean up asynchronously
- No polling, no blocking
Core: SIGCHLD is not reliably queued, which means multiple child exits can result in only one signal delivery, so the handler must loop and reap all exited children, not just one.

### What actually happens when a child process exits?
When a child process terminates:
- The kernel marks the child as exited
- The kernel stores the child’s exit status internally
- The child becomes a zombie (its PID and exit status remain)
- The kernel notifies the parent by delivering SIGCHLD

**Important:**
The kernel does not immediately delete the child’s process entry.
It waits for the parent to acknowledge it using wait() or waitpid().

### What does “SIGCHLD is not queued” mean?
Most UNIX signals are level-triggered, not edge-triggered, and not queued.
That means:
- If one child exits, SIGCHLD is generated
- If another child exits before the parent handles the first SIGCHLD, the kernel does not necessarily send another SIGCHLD
- The kernel just notes: “parent has a pending SIGCHLD”

So, Multiple child exits can collapse into a single SIGCHLD delivery

This is why we say SIGCHLD is not reliably queued
(Unless you use POSIX real-time signals, which is not the case here.)

---
---

## Line-by-line explanation
`(void)s;`

- Required because signal handlers must accept an int
- Prevents compiler warnings
- Signal number is not needed

`int saved_errno = errno;`

- Signal handlers interrupt system calls.
- If this handler runs:
    - During accept()
    - During read()
    - During write()
- Then calling waitpid() may overwrite errno, corrupting the caller’s error state.
- Saving and restoring errno is mandatory for correctness.

`while (waitpid(-1, NULL, WNOHANG) > 0);`
This is the heart of the handler.

- `waitpid(-1, ...)`
    - Reaps any child process
    - Not a specific PID
- `WNOHANG`
    - Do not block
    - Return immediately if no child has exited
- Loop required because:
    - Multiple children may exit before one signal arrives
    - SIGCHLD is not queued
- `errno = saved_errno;`
    - Restores the interrupted syscall’s error state.

## What breaks if this handler is removed
- Zombies accumulate
- Server eventually fails under load
- ps shows <defunct> processes

## These variable declarations (why each exists)
```c
struct sigaction sa;
int yes = 1;
char s[INET6_ADDRSTRLEN];
int rv;
```

`struct sigaction sa;`
- Used to install a reliable signal handler.
- sigaction tells the kernel: “When signal X occurs, do Y.” It replaces the older signal() API.
- Why not signal()?
    - signal() is legacy and unreliable
    - Behavior differs across UNIX variants
    - sigaction() is POSIX-correct
Use sigaction() unless you've got very compelling reasons not to do so.

The signal() interface has antiquity (and hence availability) in its favour, and it is defined in the C standard. Nevertheless, it has a number of undesirable characteristics that sigaction() avoids - unless you use the flags explicitly added to sigaction() to allow it to faithfully simulate the old signal() behaviour.

The signal() function does not (necessarily) block other signals from arriving while the current handler is executing; sigaction() can block other signals until the current handler returns.

`int yes = 1;`
- Used for: `setsockopt(... SO_REUSEADDR ...)`
- This flag enables port reuse.
- Without it:
    - Restarting the server immediately after exit may fail
    - Port remains in TIME_WAIT

`char s[INET6_ADDRSTRLEN];`
- Buffer to store printable IP address.
- Why INET6?
    - Works for both IPv4 and IPv6
    - IPv4 fits inside IPv6 buffer

`int rv;`
- Stores return values from library calls.
- Used to:
    - Capture getaddrinfo() errors
    - Print human-readable messages via gai_strerror()

## sigaction Structure definition (simplified)
```c
struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
};
```

### Fields explained
`sa.sa_handler = sigchld_handler;`
- Pointer to your handler function
- Called by the kernel
- Runs asynchronously

sigemptyset(&sa.sa_mask);
- Meaning: No signals are blocked while the handler runs

**What is `sa_mask`?**<br>
`sa_mask` specifies a set of signals that shall be blocked while the signal handler is executing.

**What does “blocked while the handler runs” mean?**<br>
When a signal handler starts executing:
- The kernel temporarily blocks:
    - The signal currently being handled
    - Plus all signals listed in `sa_mask`
- The handler runs
- When the handler returns, the previous signal mask is restored

This prevents signal re-entry and race conditions.

**default value of `sa_mask`?**<br> 
Important rule (POSIX / Linux): There is NO default value. <br>
If you do not initialize sa_mask, it contains garbage memory. <br>
This is not optional behavior — it is undefined behavior.

`sa.sa_flags = SA_RESTART;`
- Macro from: <signal.h>
- Meaning: If a system call (e.g., accept()) is interrupted by SIGCHLD: Kernel automatically restarts it, Instead of returning EINTR.
- Without this: accept() could randomly fail

**Installing the handler** `sigaction(SIGCHLD, &sa, NULL);`

|Parameter	|Meaning|
|---|---|
|SIGCHLD    |Which signal|
|&sa	|New behavior|
|NULL	|Ignore old behavior|

Return value:
- `0` on success
- `-1` on failure

---
---

# Why `setsockopt()` exists at all
Code:
```c
if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
        sizeof(int)) == -1) {
    perror("setsockopt");
    exit(1);
}
```

A socket is not just a file descriptor. It has many tunable behaviors:
- Can the address be reused?
- Should packets be kept alive?
- Should data be sent immediately or buffered?
- How large should buffers be?
- Should packets be broadcast?
- Should errors be reported asynchronously?

These behaviors cannot be expressed via: socket(), bind(), connect().

So UNIX provides a generic configuration interface:
*“Set an option on this socket, at this protocol level, with this value.”*
That interface is setsockopt().

## The function declaration
```c
int setsockopt(
    int sockfd,
    int level,
    int optname,
    const void *optval,
    socklen_t optlen
);
```

Return value:
- `0` → success
- `-1` → failure (errno set)

Declared in: `<sys/socket.h>`


**`int sockfd`**<br>
This is the socket file descriptor returned by: socket(), accept().
setsockopt() modifies this socket only, not all sockets.

**`int level`** <br>
This answers the question: “Which layer of the networking stack does this option belong to?”

Sockets are layered:
```
Application
↓
Socket layer (generic)
↓
Transport layer (TCP / UDP)
↓
Network layer (IP)
```
level selects which layer understands optname.

### level-options examples:
1. `SOL_SOCKET` — the socket layer <br>
Meaning: “This option applies to the generic socket itself, regardless of protocol.” <br>
Header: `<sys/socket.h>`

Common SOL_SOCKET options:
|Option name|Meaning|
|---|---|
|SO_REUSEADDR|Reuse local addresses|
|SO_KEEPALIVE|Enable TCP keepalive|
|SO_SNDTIMEO|Send timeout|
|SO_RCVTIMEO|Receive timeout|
|SO_SNDBUF|Send buffer size|
|SO_RCVBUF|Receive buffer size|
|SO_BROADCAST|Allow broadcast|
|SO_ERROR|Get pending socket error|
|SO_LINGER|Control close() behavior|

These options exist even before a connection is made.

2. IPPROTO_TCP (TCP-specific)
`level = IPPROTO_TCP` - Options affect TCP behavior only.<br>
Header: `<netinet/tcp.h>`<br>

Examples:
|Option|Purpose|
|TCP_NODELAY|Disable Nagle’s algorithm|
|TCP_KEEPIDLE|Idle time before keepalive|
|TCP_KEEPINTVL|Interval between probes|
|TCP_KEEPCNT|Probe retry count|

3. IPPROTO_IP (IPv4 options)
`level = IPPROTO_IP`, Header: `<netinet/in.h>`

Examples:
|Option|Meaning|
|IP_TTL|Time-to-live|
|IP_TOS|Type of service|
|IP_MULTICAST_TTL|Multicast TTL|

4. IPPROTO_IPV6 (IPv6 options)
`level = IPPROTO_IPV6`, Examples:
|Option|Meaning|
|IPV6_V6ONLY|IPv6-only socket|
|IPV6_MULTICAST_HOPS|Multicast hop limit|

**`int optname`**<br>
This is the specific option being set. Its meaning depends entirely on level. Example:
```
SOL_SOCKET + SO_REUSEADDR
IPPROTO_TCP + TCP_NODELAY
```
The kernel interprets (level, optname) as a pair.

**`const void *optval`**<br>
This is the value you want to assign to the option.

Important:
- optval is untyped
- Kernel interprets it based on optname
This is why optlen is required.

Why const void * specifically?
- Kernel only reads from this memory
- Does not modify it (for setsockopt)
For getsockopt(), the pointer is non-const, because the kernel writes into it.

**`socklen_t optlen`**<br>
This tells the kernel: “How many bytes should I read from optval?”. The kernel does not know the size otherwise.

### optval in our code:
The yes variable in your code
```c
int yes = 1;
setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
```
What this means?
- `SO_REUSEADDR` is a boolean option
- Convention:
    - `0` → disabled
    - `non-zero` → enabled

So yes = 1 simply means: “Turn this option ON”

This variable:
- Must exist in memory
- Must be passed by address
- Must have correct size

**Why optval is a pointer (and not a value)?**<br>
Because not all options are integers. Different options require different data types. <br>
Common errno.s & Meaning: `EBADF` - Invalid socket, `ENOPROTOOPT` - Invalid option for level

## Common optval patterns (VERY IMPORTANT)

#### Case 1: Boolean / integer options
```c
int enable = 1;
setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
```
Used for:
- SO_REUSEADDR
- SO_KEEPALIVE
- TCP_NODELAY

#### Case 2: Buffer sizes
```c
int size = 65536;
setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
```
Kernel may modify the value internally.

#### Case 3: Timeouts (struct timeval)
```c
struct timeval tv;
tv.tv_sec = 5;
tv.tv_usec = 0;
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
```
Used for:
- send timeout
- receive timeout

#### Case 4: Linger options (struct linger)
```c
struct linger l;
l.l_onoff = 1;
l.l_linger = 10;
setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
```
Controls behavior of close().

#### Case 5: Multicast configuration
```c
struct ip_mreq mreq;
setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
```

# References:
- [Daemon Processes](https://www.geeksforgeeks.org/operating-systems/daemon-processes/)
- [perror syntax](https://www.geeksforgeeks.org/c/perror-in-c/)
- [signal handling in C](https://www.geeksforgeeks.org/c/signals-c-language/)
- [sigaction man page](https://man7.org/linux/man-pages/man2/sigaction.2.html)
- [waitpid & its different options](https://www.educative.io/answers/what-is-the-waitpid-system-call)
- [wait vs waitpid](https://www.educative.io/answers/wait-vs-waitpid-in-c)
- [signal vs sigaction](https://stackoverflow.com/questions/231912/what-is-the-difference-between-sigaction-and-signal)
- [tcp nagle & clark's algo](https://www.geeksforgeeks.org/computer-networks/silly-window-syndrome/)
- [TOS in IPv4](https://manuals.gfi.com/en/exinda/help/content/exos/tos-diffserv/tos-field.html)
- [IPv4 multicast](https://www.geeksforgeeks.org/computer-networks/what-is-an-ip-multicast/)