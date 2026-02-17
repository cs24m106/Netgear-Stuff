#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_SIZE 3

typedef struct Node{
    char* key;
    int data;
    struct Node* next;
} Node;


unsigned int hash(const char* key)
{
    unsigned int h = 0;
    while (*key) 
        h = (h * 5) + *key++; // prev*5 + next_char
    return h % MAX_SIZE;
}

Node* insertNode(Node* head, int val)
{
    Node* newNode = (Node*)malloc(sizeof(Node));
    newNode->data = val;
    newNode->next = head;
    return newNode;
}

Node* insertNodeKey(Node* head, const char* key, int val)
{
    Node* newNode = (Node*)malloc(sizeof(Node));
    newNode->key = strdup(key);
    newNode->data = val;
    newNode->next = head;
    return newNode;
}

void printList(Node* head)
{
    Node* curr = head;
    while(curr != NULL)
    {
        printf("%p:%d -> ", curr, curr->data);
        curr = curr->next;
    }
    printf("NULL\n");
}

typedef struct {
    Node* top;
    int cur_size;
    int max_size;
} Stack;

void pushNode(Stack* s, int val)
{
    if (s->cur_size >= s->max_size){
        printf("Stack Overflow!\n");
        return;
    }
    s->top = insertNode(s->top, val);
    s->cur_size++;
}

int popNode(Stack* s)
{
    if (s->cur_size <= 0){
        printf("Stack Underflow!\n");
        return -1;
    }
    Node* tofree = s->top;
    s->top = s->top->next;
    s->cur_size--;
    int tosave = tofree->data;
    free(tofree);
    return tosave;
}



typedef struct {
    int items[MAX_SIZE];
    int front;
    int rear;
    int count;
} Queue;

void enqueue(Queue* q, int value) {
    if (q->count == MAX_SIZE) {
        printf("Queue overflow!\n");
        return;
    }
    q->items[q->rear] = value;
    q->rear = (q->rear + 1) % MAX_SIZE;
    q->count++;
}

int dequeue(Queue* q) {
    if (q->count <= 0) {
        printf("Queue underflow!\n");
        return -1;
    }
    int value = q->items[q->front];
    q->front = (q->front + 1) % MAX_SIZE;
    q->count--;
    return value;
}

// string(key-index) -> int (data)
typedef struct {
    Node* buckets[MAX_SIZE];
} HashMap;

void putKeyVal(HashMap* map, const char* key, int val)
{
    int idx = hash(key);
    printf("put() hash idx for %s is %d\n", key, idx);
    Node* curr = map->buckets[idx];
    while(curr!=NULL)
    {
        if(strcmp(curr->key, key) == 0)
        {
            curr->data = val;
            return;
        }
        curr = curr->next;
    }
    map->buckets[idx] = insertNodeKey(map->buckets[idx], key, val);
}

void removeKey(HashMap* map, const char* key)
{
    int idx = hash(key);
    printf("remove() hash idx for %s is %d\n", key, idx);
    Node* prev = map->buckets[idx];
    Node* curr = prev->next;
    if(prev!=NULL && strcmp(prev->key, key) == 0)
    {
        map->buckets[idx] = prev->next;
        free(prev);
        return;
    }
    
    while(curr!=NULL)
    {
        if(strcmp(curr->key, key) == 0)
        {
            prev->next = curr->next;
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

int get(HashMap* map, const char* key) 
{
    int idx = hash(key);
    printf("get() hash idx for %s is %d\n", key, idx);
    Node* curr = map->buckets[idx];
    while(curr!=NULL)
    {
        if(strcmp(curr->key, key) == 0)
        {
            return curr->data;
        }
        curr = curr->next;
    }
    return -1;
}


void main()
{
    HashMap h;
    for(int i=0; i<MAX_SIZE; i++)
        h.buckets[i] = NULL;
    
    putKeyVal(&h, "apple", 10);
    putKeyVal(&h, "banana", 20);
    putKeyVal(&h, "mango", 30);

    printf("apple: %d\n", get(&h, "apple"));   // 10
    printf("banana: %d\n", get(&h, "banana")); // 20
    printf("mango: %d\n", get(&h, "mango")); // 30
    removeKey(&h, "mango");

    printf("mango: %d\n", get(&h, "mango")); // 30
    printf("grape: %d\n", get(&h, "grape")); // not found
}

    // int arr[5] = {1,2,3,4,5};

    // for(int i=0; i<5; i++)
    //     printf("%p:%d, ", arr+i, arr[i]);
    // printf("\n");

    // int *p = (int*)malloc(5 * sizeof(int));
    // for(int i=0; i<5; i++)
    //     printf("%p:%d, ", p+i, p[i]);
    // printf("\n");



    // Node* list = NULL;
    // list = insertNode(list, 1);
    // list = insertNode(list, 2);
    // list = insertNode(list, 3);    

    // printList(list);
    // Node* tofree = list->next;
    // list->next = list->next->next;
    // free(tofree);
    // printList(list);


    // Stack s;
    // s.cur_size = 0;
    // s.max_size = 3;
    // s.top = NULL;

    // pushNode(&s, 1);
    // pushNode(&s, 2);
    // pushNode(&s, 3);

    // printList(s.top);

    // printf("pop: %d\n", popNode(&s));
    // printList(s.top);

    // Queue q;
    // q.front = 0;
    // q.rear = 0;
    // q.count = 0;

    
    // enqueue(&q, 100);
    // enqueue(&q, 200);
    // enqueue(&q, 300);
    
    // for(int i=0; i<q.count; i++)
    //     printf("%d - ", q.items[(q.front + i) % MAX_SIZE]);
    // printf("\n");

    // printf("Dequeued: %d\n", dequeue(&q));

    // for(int i=0; i<q.count; i++)
    //     printf("%d - ", q.items[(q.front + i) % MAX_SIZE]);
    // printf("\n");
