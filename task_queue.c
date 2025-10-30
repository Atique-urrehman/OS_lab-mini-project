#define _POSIX_C_SOURCE 200809L
#include "task_queue.h"
#include <stdlib.h>
#include <string.h>

int tq_init(task_queue_t *q, int capacity) {
    if (capacity <= 0) return -1;
    q->items = calloc(capacity, sizeof(task_t*));
    if (!q->items) return -1;
    q->capacity = capacity;
    q->front = q->rear = q->size = 0;
    q->closed = 0;
    if (pthread_mutex_init(&q->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&q->not_empty, NULL) != 0) return -1;
    if (pthread_cond_init(&q->not_full, NULL) != 0) return -1;
    return 0;
}

void tq_destroy(task_queue_t *q) {
    if (!q) return;
    free(q->items);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

int tq_push(task_queue_t *q, task_t *t) {
    int ret = 0;
    pthread_mutex_lock(&q->lock);
    while (q->size == q->capacity && !q->closed) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }
    if (q->closed) {
        ret = -1;
    } else {
        q->items[q->rear] = t;
        q->rear = (q->rear + 1) % q->capacity;
        q->size++;
        pthread_cond_signal(&q->not_empty);
        ret = 0;
    }
    pthread_mutex_unlock(&q->lock);
    return ret;
}

int tq_pop(task_queue_t *q, task_t **t) {
    int ret = 0;
    pthread_mutex_lock(&q->lock);
    while (q->size == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    if (q->size == 0 && q->closed) {
        ret = -1;
    } else {
        *t = q->items[q->front];
        q->front = (q->front + 1) % q->capacity;
        q->size--;
        pthread_cond_signal(&q->not_full);
        ret = 0;
    }
    pthread_mutex_unlock(&q->lock);
    return ret;
}

void tq_close(task_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

task_response_t *task_response_create() {
    task_response_t *r = calloc(1, sizeof(task_response_t));
    if (!r) return NULL;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);
    r->done = 0;
    r->success = 0;
    r->msg = NULL;
    r->data = NULL;
    r->data_len = 0;
    return r;
}

void task_response_destroy(task_response_t *r) {
    if (!r) return;
    if (r->msg) free(r->msg);
    if (r->data) free(r->data);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cond);
    free(r);
}
