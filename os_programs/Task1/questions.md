# Basic Linux Process Related Exercises

## 1. Write a program, which will compare two files (of sufficiently large size). 

- The program should do comparison of blocks of file in parallel using processes and message queues.
- In cases where the files do not match, try to do the least work, i.e. stop once we find the first mis-match.
- Input would be the two files to compare and the number of parallel process that we would like to use.

The final result should include
- Success / Failure
- Number of blocks processed by each of the process that was running in parallel
 
When the program returns the status, it should ensure that all the resources that was created has been properly released. 



## 2. Redo the above programs, 
- using threads rather than processes, and 
- continue to use message queues as the ways of communicating between the threads
 


## 3. Write a program that will take a file as an input, 
- and then perform the following in sequence on the content of the file 
- (The content is text file with newlines).
- The words are separated by spaces/tabs
- Substitute any occurence of the specified pattern1, by pattern2 for all occurences

In a line with words, if there are multiple separator, replace them by one
- Break lines that are greater than 100 characters to multiple lines of maximum 100 characters 
- write the resultant data back to the output file

The prgram should