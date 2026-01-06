#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>

typedef struct node{
    int data;
    struct node* next;
    struct node* prev;
} Node;

typedef struct queue{
    Node *front;
    Node *rear;
    pthread_mutex_t lock; // race cond safety
    pthread_cond_t cond; // condition holder to suspend deque until size of queue > 1
    int count;
} Queue;

typedef struct thread_args{
    int tno;
    Queue* q;
    int no_ops;
} TArgs;

Node* createNode(int data){
    Node *n = malloc(sizeof(Node));
    n->data = data;
    n->next = NULL;
    n->prev = NULL;
    return  n;
}

void enqueue(Queue* q, int data){ // push
    Node* temp = createNode(data);
    
    pthread_mutex_lock(&q->lock);
    
    if(q->count == 0){
        q->front = temp;
        q->rear = temp;
        q->count = 1;
    }else{
        q->rear->next = temp;
        temp->prev = q->rear;
        q->rear = temp;
        q->count++;
    }

    pthread_cond_signal(&q->cond); // producer signals consumer (wakeup)
    pthread_mutex_unlock(&q->lock);
}

int dequeue(Queue* q){ // pop
    int n;

    pthread_mutex_lock(&q->lock);
    //wait for non empty queue
    while(q->count == 0){
        pthread_cond_wait(&q->cond, &q->lock); // consumer waiting for producer (sleep)
    }
    Node* temp;
    
    if (q->front){
        temp = q->front;
        n = temp->data;
        if (q->front->next){
            q->front = q->front->next;
            q->front->prev = NULL;
            q->count--;
        }
        else{
            q->front = q->rear = NULL;
            q->count = 0;
        }
        free(temp);
    }
    else{
        printf("Queue Empty... Condition Wait Failed!\n");
        n = -1;
    }
    pthread_mutex_unlock(&q->lock);
    return n;
}

void PrintQueueFront(Queue *q){ // front to rear forward direction
    Node *curr = q->front;
    printf("Queue From Frnt => ");
    for(int i=1; curr != NULL; i++){
        printf("[%d]:%d,",i,curr->data);
        curr = curr->next;
    }
    printf("\n");
}

void PrintQueueRear(Queue *q){ // rear to front backward direction
    Node *curr = q->rear;
    printf("Queue From Rear => ");
    for(int i=1; curr != NULL; i++){
        printf("[%d]:%d,",i,curr->data);
        curr = curr->prev;
    }
    printf("\n");
}

void QueueMenu(Queue *q){
    int i,ch;
    int data,n;
    bool flag = true;
    while(flag){
        printf("\n1)Enqueue\n2)Dequeue\n3)Print-Front\n4)Print-Rear\n5)Exit\nEnter your choice: ");
        scanf("%d",&ch);
        switch(ch){
            case 1:
                printf("Enqueued Data: ");
                scanf("%d",&data);
                enqueue(q,data);
                break;
            case 2:
                n = dequeue(q);
                printf("Dequeued Data: %d\n",n);
                break;
            case 3:
                PrintQueueFront(q);
                continue;;
            case 4:
                PrintQueueRear(q);
                break;
            case 5:
                printf("Exitting Queue Menu...\n");
                flag = false;
                break;
            default:
                printf("Enter Vaild Choice :(\n");
                break;
        }
    }
}

void* enque_worker(void *arg){ // rand enque worker
    TArgs* args = (TArgs *) arg; int n;
    for(int i =0; i < args->no_ops; i++){
        n = rand()%100; enqueue(args->q,n);
        printf("\nEnque-Worker(ID:%d) Push:%d done\n",args->tno, n);
        PrintQueueFront(args->q);
        sleep(1); // sleep in between so we can print queue from main thread
    }
    return NULL;
}

void* dequeue_worker(void *arg){ // group deque worker
    TArgs* args = (TArgs *) arg; int n;
    for(int i =0; i < args->no_ops; i++){
        n = dequeue(args->q);
        printf("\nDeque-Worker(ID:%d) Pop:%d done\n",args->tno, n);
        PrintQueueRear(args->q);
        sleep(1); // sleep in between so we can print queue from main thread
    }
    return NULL;
}

int main(){
    Queue q;
    TArgs args1, args2, args3, args4;
    pthread_t eq1, eq2, deq1, deq2;
    
    // initialize Queue members
    q.count = 0;
    
    // initialize mutex, condition vars
    pthread_mutex_init(&q.lock, NULL);
    pthread_cond_init(&q.cond,NULL);
    
    q.front = NULL; q.rear = NULL;

    args1.tno = 1; args1.q = &q;
    args1.no_ops = 20;
    pthread_create(&eq1, NULL, enque_worker, &args1); // push 20 elems

    args2.tno = 2; args2.q = &q;
    args2.no_ops = 10;
    pthread_create(&deq1, NULL, dequeue_worker, &args2); // pop 10 elems
    
    args3.tno = 3; args3.q = &q;
    args3.no_ops = 20;
    pthread_create(&deq2, NULL, dequeue_worker, &args3); // pop 20 elems

    args4.tno = 4; args4.q = &q;
    args4.no_ops = 10;
    pthread_create(&eq2, NULL, enque_worker, &args4); // push 10 elems
    
    //QueueMenu(&q); // uncomment if u want handle extra from main thread
    
    pthread_join(eq1,NULL);
    pthread_join(eq1,NULL);
    pthread_join(deq1,NULL);
    pthread_join(deq2,NULL);
}