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

**how to handle new fd_max when client disconnects? not necessarily needed but if i were to implement it for efficient for-loop search**<br>
It matters when:
- You have many short-lived connections
-File descriptor numbers grow large
- You care about reducing O(fdmax) scanning cost
This is exactly one of the historical reasons select() does not scale well.
```c
/*  Refer the question comment in selectserver.c
    Recompute fdmax ONLY if needed */
        if (s == *fdmax) {
            for (int i = *fdmax - 1; i >= 0; i--) {
                if (FD_ISSET(i, master)) {
                    *fdmax = i;
                    break;
                }
            }
        }
```

## 7.5 Serialization()
To change byte order of int types, we use unsigned char buffer. **Why "unsigned" char buffer?**<br>
This is a data type that typically occupies one byte (8 bits) of memory. 
Unlike a regular char (which can be signed or unsigned depending on the system and 
store values from -128 to 127), an unsigned char is guaranteed to store only non-negative values, 
ranging from 0 to 255. This range corresponds exactly to all possible 8-bit patterns, 
making it the closest representation of the hardware concept of a "byte" in C/C++.

notice: unpack fns in `pack2.c` file convert to a larger byte type, to ensure portability encorporating sign-bit extension manually without architechtural dependency for resp data type sent and received of same size. <br>
Example `unpacki16()`:
- inp: 16bit = 2B big-endian network addressable unsigned char buffer
- out: lets assume signed integer = 32bit = 4B (depending on arch)
- unsigned int changed byte order => i2
- sign-extension:
    - if `(i2 <= 0x7fffu)` --> 1st bit MSB == 0, i.e. sign extended towards significant bits is also 0's
    - else sign-bit = 1, sign extension w.r.t. 2's comp sys, significant bits are to be filled with 1's
    - `-1 - (0xffffu - i2)` --> `0xffffffff - (0xffff - i2)` 
    - assuming int here is 32bit=4B => 4pairs of hexdec => -1 = `0x ff ff ff ff`
    - which is nothing but 0xffff(----) where (----) 16bits are replaced with i2's bit values resp (in a vague sense)
- in other sense: `signed_value = unsigned_value − 2^16` is being computed manually regardless of arch, assumptions like *int is 32-bit*, *machine uses two’s complement*. The language semantics still operate on values, not bit layouts. 
- The expression: `-1 - (0xFFFF - i2)` is evaluated as pure arithmetic: `-1 - 65535 + i2` = `i2 - 65536` = `i2 - 2^16` here
<br>
<br>

### Variadic Functions in C
In C, variadic functions are functions that can take a variable number of arguments. This feature is useful when the number of arguments for a function is unknown.It takes one fixed argument and then any number of arguments can be passed.

[Syntax](https://www.geeksforgeeks.org/c/variadic-functions-in-c/)
```c
return_type name(fixed_arg, ...);
```

- `va_start` - Initializes the va_list variable to point to the first variable argument. It requires the va_list variable and the name of the last fixed argument.
- `va_start(ap, last)` — initialize `ap` so it points to the first unnamed argument; `last` is the last named parameter of the function.
- **Rule:** the second argument to va_start must be the identifier of the last named parameter in the function definition. It is not a special keyword `arg-count`  — it’s simply whatever parameter you declared last.

**How does traversal actually work? (conceptual memory picture)** <br>

Conceptually (simple model): Caller pushes arguments, left-to-right (conceptual):
```
[ fixed param1 ][ fixed param2 ][ var1 ][ var2 ][ var3 ] ...
```

Inside the callee:
- `va_start(ap, last_fixed)` figures out the address immediately after last_fixed and sets ap to it.
- Each `va_arg(ap, T)` reads T from ap and advances ap by sizeof(T) (subject to ABI alignment rules and promotions).

For variadic functions only, the C standard enforces a rule called: **Default Argument Promotions**<br>
This happens at the call site, before the function even begins execution. 
The rules are: (This is mandatory, not optional)
| Original type                 | Actually passed      |
| ----------------------------- | -------------------- |
| `char`                        | `int`                |
| `signed char`                 | `int`                |
| `unsigned char`               | `int`                |
| `short`                       | `int`                |
| `unsigned short`              | `int`                |
| `float`                       | `double`             |
| `double`                      | `double` (unchanged) |
| `long`, `long long`, pointers | unchanged            |

**Why does C do this?**<br>
Because in a variadic function:
- The callee has no type information
- The stack/register layout must be uniform
- Small integer types are expensive to track individually

So the compiler normalizes everything. <br>
**NOTE:** Thus, If you wrote: `va_arg(ap, char)   // ❌ Undefined behavior` the program would break.

What goes wrong?
- The actual argument is an int (4 bytes)
- You told va_arg to read 1 byte
- It reads only the first byte of the int
- The cursor advances incorrectly
- The next argument is now read from the wrong address

*Thus its `vg_arg` call is supposed to be made w.r.t. crt promoted types and explicitly casted back to the required type for certain cases as mentioned above.*