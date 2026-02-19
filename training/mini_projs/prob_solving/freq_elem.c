/*
Question:
Frequencies in a Limited Array
Difficulty: Easy, Accuracy: 27.64%, Submissions: 390K+Points: 2, Average Time: 10m
You are given an array arr[] containing positive integers. 
The elements in the array arr[] range from  1 to n (where n is the size of the array), 
and some numbers may be repeated or absent. Your have to count the frequency of all numbers 
in the range 1 to n and return an array of size n such that result[i] represents 
the frequency of the number i (1-based indexing)
*/

#include <stdio.h>
#include <stdlib.h>

/**
 * Function to count frequencies in-place
 * @param n: size of the array
 * @param arr: input array of size n
 * @return: pointer to the modified array (or a new one depending on preference)
 */
void frequencyCount(int n, int arr[]) {
    // Step 1: Adjust values to 0-based indexing (0 to n-1)
    // and handle values > n if they exist (though the prompt says 1 to n)
    for (int i = 0; i < n; i++) {
        arr[i] -= 1;
    }

    // Step 2: Use the array indices to store frequencies
    // We add 'n' to the index corresponding to the value found
    for (int i = 0; i < n; i++) {
        // We use % n because the value at arr[i] might have 
        // already been incremented by n previously
        int originalValue = arr[i] % n;
        arr[originalValue] += n;
    }

    // Step 3: Result extraction
    // The frequency of (i+1) is now stored as (arr[i] / n)
    for (int i = 0; i < n; i++) {
        arr[i] = arr[i] / n;
    }
}

int main() {
    int arr[] = {2, 3, 2, 3, 5};
    int n = sizeof(arr) / sizeof(arr[0]);

    frequencyCount(n, arr);

    printf("Frequencies: ");
    for (int i = 0; i < n; i++) {
        printf("%d ", arr[i]);
    }
    // Output should be: 0 2 2 0 1 
    // (1 appears 0 times, 2 appears 2 times, 3 appears 2 times, etc.)

    return 0;
}