// client_queue.h
#ifndef CLIENT_QUEUE_H
#define CLIENT_QUEUE_H
#include <pthread.h>

typedef struct {
    int *items;
    int capacity, front, rear, size;
    int closed;
    pthread_mutex_t lock;
    pthread_cond_t not_empty, not_full;
} client_queue_t;

int cq_init(client_queue_t *q,int cap);
void cq_destroy(client_queue_t *q);
int cq_push(client_queue_t *q,int sock);
int cq_pop(client_queue_t *q,int *sock);
void cq_close(client_queue_t *q);
#endif
