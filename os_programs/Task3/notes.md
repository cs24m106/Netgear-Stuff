[`pthread_mutex_t`](https://www.geeksforgeeks.org/linux-unix/mutex-lock-for-linux-thread-synchronization/) is the data type for a mutex (mutual exclusion) lock in the POSIX Threads (pthreads) library in C. Some of its Fns:
- `pthread_mutex_init()`: Dynamically initializes a mutex object. A NULL attribute parameter uses default settings.
- `pthread_mutex_destroy()`: Destroys a mutex object, freeing any resources it may hold. It must be unlocked when destroyed.
- `pthread_mutex_lock()`: Locks the mutex. If the mutex is already locked by another thread, the calling thread waits until it becomes available.
- `pthread_mutex_trylock()`: Attempts to lock the mutex, but does not block if the mutex is already locked. Instead, it returns immediately with an error code (EBUSY).
- `pthread_mutex_unlock()`: Unlocks the mutex, making it available for other waiting threads. 

<br>

[`pthread_cond_t`](https://www.geeksforgeeks.org/linux-unix/condition-wait-signal-multi-threading/) is a data type representing a condition variable, used in multithreaded programming for thread synchronization, allowing threads to wait for a specific condition to become true and be notified when it happens. Some of its Fns:
- `pthread_cond_init()`: Initializes the condition variable, taking an optional attributes object (or NULL for defaults).
- `pthread_cond_destroy()`: Destroys the condition variable, releasing resources.
- `pthread_cond_wait()`: Blocks the calling thread until signaled; atomically unlocks the associated mutex and waits.
- `pthread_cond_signal()`: Wakes up one thread waiting on the condition, if any.
- `pthread_cond_broadcast()`: Wakes up all threads waiting on the condition, if any. 

<br>

A thread must have the mutex locked before calling pthread_cond_wait(). WHY?
- The primary purpose of the mutex is to protect the shared data (the "predicate") associated with the condition variable, not the condition variable itself. The requirement to lock the mutex before waiting is essential to prevent a critical concurrency issue known as a **"lost wakeup"** race condition. 
- **Atomic Unlock and Wait**: If the condition is false, the thread needs to block. The pthread_cond_wait() function atomically performs two actions:
    - It unlocks the mutex, allowing a signalling thread to acquire the mutex, modify the shared data, and then signal the condition.
    - It places the calling thread onto the condition variable's waiting queue.
```c
// STANDARD USAGE PATTERN
pthread_mutex_lock(&mutex);
while (!condition_is_met) {
    pthread_cond_wait(&cond, &mutex); // Automatically unlocks and relocks
}
// ... safely access shared data here ...
pthread_mutex_unlock(&mutex);
```