// client_queue.c
#include "client_queue.h"
#include <stdlib.h>

int cq_init(client_queue_t *q,int c){
    q->items=malloc(sizeof(int)*c);
    q->capacity=c;q->front=q->rear=q->size=0;q->closed=0;
    pthread_mutex_init(&q->lock,NULL);
    pthread_cond_init(&q->not_empty,NULL);
    pthread_cond_init(&q->not_full,NULL);
    return 0;
}
void cq_destroy(client_queue_t *q){
    free(q->items);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}
int cq_push(client_queue_t *q,int s){
    pthread_mutex_lock(&q->lock);
    while(q->size==q->capacity&&!q->closed)
        pthread_cond_wait(&q->not_full,&q->lock);
    if(q->closed){pthread_mutex_unlock(&q->lock);return -1;}
    q->items[q->rear]=s;
    q->rear=(q->rear+1)%q->capacity;q->size++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}
int cq_pop(client_queue_t *q,int *s){
    pthread_mutex_lock(&q->lock);
    while(q->size==0&&!q->closed)
        pthread_cond_wait(&q->not_empty,&q->lock);
    if(q->size==0&&q->closed){pthread_mutex_unlock(&q->lock);return -1;}
    *s=q->items[q->front];
    q->front=(q->front+1)%q->capacity;q->size--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 0;
}
void cq_close(client_queue_t *q){
    pthread_mutex_lock(&q->lock);
    q->closed=1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}
