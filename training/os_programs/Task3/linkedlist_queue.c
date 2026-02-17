#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

struct Node{
    struct Node* next;
    int data;
};
typedef struct Node NODE;

typedef struct queue{
    NODE *head;
    NODE *tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int count;
}QUEUE;

typedef struct threadArgs{
    QUEUE* q;
    int c;
}ARGS;

NODE* createNode(int data){
    NODE *node = malloc(sizeof(NODE));
    node->data = data;
    node->next = NULL;
    return  node;
}

void enqueue(QUEUE* q, int data){
    NODE* temp = createNode(data);
    
    pthread_mutex_lock(&q->lock);
    
    if(q->count == 0){
        q->head = temp;
        q->tail = temp;
        q->count = 1;
    }else{
        temp->next = q->head;
        q->head = temp;
        q->count++;
    }

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

int dequeue(QUEUE* q){
    int n;
    pthread_mutex_lock(&q->lock);

    //wait for non empty queue
    while(q->count == 0){
        pthread_cond_wait(&q->cond, &q->lock);
    }
    NODE* temp = q->head;
    if(q->count > 1){
        for(int i=0;temp->next->next != NULL;i++){
            temp = temp->next;
        }
        n = temp->next->data;
        temp->next = NULL;
        q->count--;
        free(temp->next);
    }else if(q->count == 1){
        n = temp->data;
        q->head = q->tail = NULL;
        q->count = 0;
        free(temp);
    }else{
        printf("Queue Empty...\n");
        return -1;
    }
    pthread_mutex_unlock(&q->lock);
    return n;
}

void PrintQueue(QUEUE q){
    NODE *temp = q.head;
    for(int i=0; temp != NULL; i++){
        printf("Index: %d\tValue: %d\n",i+1,temp->data);
        temp = temp->next;
    }
}

void mainProgram(QUEUE q){
    int i,ch;
    int data,n;
    while(1){
        printf("1)Enqueue\n2)Dequeue\n3)Print\n4)Exit\nEnter your choice: ");
        scanf("%d",&ch);
        switch(ch){
            case 1:
                printf("Enter the Data: ");
                scanf("%d",&data);
                enqueue(&q,data);
                break;
            case 2:
                printf("Dequeue:");
                n = dequeue(&q);
                printf(" %d\n",n);
                break;
            case 3:
                PrintQueue(q);
                break;
            case 4:
                printf("Exit...\n");
                break;
            default:
                printf("Enter Vaild Choice.\n");
                break;
        }
    }
}

void* client(void *arg){
    ARGS* args = (ARGS *) arg;
    for(int i =0; i<args->c; i++){
        enqueue(args->q,i);
    }
    return NULL;
}

void* server(void *arg){
    int n;
    ARGS* args = (ARGS *) arg;
    for(int i =0; i<args->c; i++){
        n = dequeue(args->q);
        printf("Val: %d\n",n);
    }

    return NULL;
}

int main(){
    QUEUE q;
    ARGS args1,args2;
    pthread_t ser1,cli1,ser2,cli2;
    
    pthread_mutex_init(&q.lock, NULL);
    pthread_cond_init(&q.cond,NULL);
    q.count = 0;
    q.head = NULL; q.tail = NULL;

    args1.q = &q;
    args1.c = 20;

    args2.q = &q;
    args2.c = 40;

    pthread_create(&cli1,NULL, client, &args1);
    pthread_create(&ser1,NULL, server, &args1);
    
    pthread_create(&cli2,NULL, client, &args2);
    pthread_create(&ser2,NULL, server, &args2);
    
    
    pthread_join(cli1,NULL);
    pthread_join(ser1,NULL);
    pthread_join(cli2,NULL);
    pthread_join(ser2,NULL);
}