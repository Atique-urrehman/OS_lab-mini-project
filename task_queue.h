#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include <pthread.h>
#include <stdint.h>

typedef enum {
    TASK_LIST,
    TASK_UPLOAD,
    TASK_DOWNLOAD,
    TASK_DELETE
} task_type_t;

typedef struct task_response {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int done;         // 0 = pending, 1 = done
    int success;      // 0 = fail, 1 = success
    char *msg;        // textual message (allocated by worker)
    void *data;       // for download/list results (allocated by worker)
    size_t data_len;
} task_response_t;

typedef struct task {
    task_type_t type;
    char *username;   // owner
    char *filename;   // may be NULL for LIST
    void *data;       // for upload: file bytes (allocated by client thread)
    size_t data_len;  // data length for upload
    task_response_t *resp; // response pointer (client waits on this)
} task_t;

typedef struct {
    task_t **items;
    int capacity;
    int front;
    int rear;
    int size;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int closed;
} task_queue_t;

int tq_init(task_queue_t *q, int capacity);
void tq_destroy(task_queue_t *q);
int tq_push(task_queue_t *q, task_t *t);
int tq_pop(task_queue_t *q, task_t **t);
void tq_close(task_queue_t *q);

task_response_t *task_response_create();
void task_response_destroy(task_response_t *r);

#endif // TASK_QUEUE_H
