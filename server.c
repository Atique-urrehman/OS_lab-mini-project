/* server.c - Phase 1 complete server */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>

#include "client_queue.h"
#include "task_queue.h"

#define DEFAULT_PORT 9000
#define BACKLOG 16
#define CLIENT_QUEUE_CAP 128
#define CLIENT_POOL_SIZE 4
#define TASK_QUEUE_CAP 128
#define WORKER_POOL_SIZE 4
#define USER_QUOTA_BYTES (10 * 1024 * 1024) /* 10 MB */

static int listen_fd = -1;
static client_queue_t client_q;
static pthread_t client_threads[CLIENT_POOL_SIZE];
static volatile int running = 1;

static task_queue_t task_q;
static pthread_t worker_threads[WORKER_POOL_SIZE];

static void handle_sigint(int signo) {
    (void)signo;
    running = 0;
    cq_close(&client_q);
    tq_close(&task_q);
    if (listen_fd != -1) close(listen_fd);
}

/* Ensure storage/<username> exists */
static int ensure_user_dir(const char *username) {
    char path[512];
    snprintf(path, sizeof(path), "storage/%s", username);
    struct stat st;
    if (stat("storage", &st) == -1) {
        if (mkdir("storage", 0755) != 0) return -1;
    }
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) != 0) return -1;
    }
    return 0;
}

/* Compute total bytes used by a user (safe: uses stat on entries) */
static size_t compute_user_usage(const char *username) {
    size_t total = 0;
    char path[512];
    snprintf(path, sizeof(path), "storage/%s", username);
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char fpath[768];
        snprintf(fpath, sizeof(fpath), "%s/%s", path, entry->d_name);
        struct stat st_entry;
        if (stat(fpath, &st_entry) != 0) continue;
        if (!S_ISREG(st_entry.st_mode)) continue;
        total += (size_t)st_entry.st_size;
    }
    closedir(d);
    return total;
}

/* Worker helpers */

static int worker_handle_upload(task_t *t) {
    if (ensure_user_dir(t->username) != 0) return -1;
    size_t used = compute_user_usage(t->username);
    if (used + t->data_len > USER_QUOTA_BYTES) return -2;
    char path[1024];
    snprintf(path, sizeof(path), "storage/%s/%s", t->username, t->filename);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t written = fwrite(t->data, 1, t->data_len, f);
    fclose(f);
    if (written != t->data_len) return -1;
    return 0;
}

static int worker_handle_download(task_t *t, void **out_buf, size_t *out_len) {
    char path[1024];
    snprintf(path, sizeof(path), "storage/%s/%s", t->username, t->filename);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return -1; }
    rewind(f);
    *out_len = (size_t)len;
    *out_buf = malloc(*out_len ? *out_len : 1);
    if (!*out_buf) { fclose(f); return -1; }
    size_t r = fread(*out_buf, 1, *out_len, f);
    fclose(f);
    if (r != *out_len) { free(*out_buf); *out_buf = NULL; *out_len = 0; return -1; }
    return 0;
}

static int worker_handle_delete(task_t *t) {
    char path[1024];
    snprintf(path, sizeof(path), "storage/%s/%s", t->username, t->filename);
    if (unlink(path) != 0) return -1;
    return 0;
}

static int worker_handle_list(task_t *t, char **out_text, size_t *out_len) {
    char path[512];
    snprintf(path, sizeof(path), "storage/%s", t->username);
    DIR *d = opendir(path);
    if (!d) {
        *out_text = strdup("(no files)\n");
        *out_len = strlen(*out_text);
        return 0;
    }
    size_t cap = 1024;
    char *buf = malloc(cap);
    if (!buf) { closedir(d); return -1; }
    buf[0] = '\0';
    size_t len = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char fpath[768];
        snprintf(fpath, sizeof(fpath), "%s/%s", path, entry->d_name);
        struct stat st_entry;
        if (stat(fpath, &st_entry) != 0) continue;
        if (!S_ISREG(st_entry.st_mode)) continue;
        char line[512];
        snprintf(line, sizeof(line), "%s\n", entry->d_name);
        size_t l = strlen(line);
        if (len + l + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); closedir(d); return -1; }
            buf = nb;
        }
        memcpy(buf + len, line, l);
        len += l;
        buf[len] = '\0';
    }
    closedir(d);
    *out_text = buf;
    *out_len = len;
    return 0;
}

/* Worker thread function */
void *worker_fn(void *arg) {
    (void)arg;
    while (running) {
        task_t *t = NULL;
        if (tq_pop(&task_q, &t) != 0) break; /* queue closed */
        if (!t) continue;

        if (t->type == TASK_UPLOAD) {
            int r = worker_handle_upload(t);
            pthread_mutex_lock(&t->resp->lock);
            t->resp->success = (r == 0);
            if (r == 0) t->resp->msg = strdup("UPLOAD OK\n");
            else if (r == -2) t->resp->msg = strdup("UPLOAD FAILED: QUOTA EXCEEDED\n");
            else t->resp->msg = strdup("UPLOAD FAILED\n");
            t->resp->done = 1;
            pthread_cond_signal(&t->resp->cond);
            pthread_mutex_unlock(&t->resp->lock);
        } else if (t->type == TASK_DOWNLOAD) {
            void *buf = NULL; size_t len = 0;
            int r = worker_handle_download(t, &buf, &len);
            pthread_mutex_lock(&t->resp->lock);
            t->resp->success = (r == 0);
            if (r == 0) {
                t->resp->data = buf;
                t->resp->data_len = len;
                t->resp->msg = strdup("DOWNLOAD OK\n");
            } else {
                t->resp->msg = strdup("DOWNLOAD FAILED\n");
            }
            t->resp->done = 1;
            pthread_cond_signal(&t->resp->cond);
            pthread_mutex_unlock(&t->resp->lock);
        } else if (t->type == TASK_DELETE) {
            int r = worker_handle_delete(t);
            pthread_mutex_lock(&t->resp->lock);
            t->resp->success = (r == 0);
            if (r == 0) t->resp->msg = strdup("DELETE OK\n");
            else t->resp->msg = strdup("DELETE FAILED\n");
            t->resp->done = 1;
            pthread_cond_signal(&t->resp->cond);
            pthread_mutex_unlock(&t->resp->lock);
        } else if (t->type == TASK_LIST) {
            char *out = NULL; size_t outlen = 0;
            int r = worker_handle_list(t, &out, &outlen);
            pthread_mutex_lock(&t->resp->lock);
            t->resp->success = (r == 0);
            if (r == 0) {
                t->resp->data = out;
                t->resp->data_len = outlen;
                t->resp->msg = strdup("LIST OK\n");
            } else {
                t->resp->msg = strdup("LIST FAILED\n");
            }
            t->resp->done = 1;
            pthread_cond_signal(&t->resp->cond);
            pthread_mutex_unlock(&t->resp->lock);
        }

        /* cleanup task metadata (response remains for client to read) */
        if (t->username) free(t->username);
        if (t->filename) free(t->filename);
        if (t->data) free(t->data);
        free(t);
    }
    return NULL;
}

/* Socket helpers */
static ssize_t send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static ssize_t recv_all(int fd, void *buf, size_t len) {
    char *p = buf;
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = recv(fd, p + recvd, len - recvd, 0);
        if (n <= 0) {
            if (n == 0) return (ssize_t)recvd;
            if (errno == EINTR) continue;
            return -1;
        }
        recvd += (size_t)n;
    }
    return (ssize_t)recvd;
}

/* simple line reader (returns malloc'd string without newline) */
static char *read_line(int fd) {
    char buf[1024];
    size_t pos = 0;
    while (1) {
        ssize_t n = recv(fd, buf + pos, 1, 0);
        if (n <= 0) return NULL;
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            return strdup(buf);
        }
        pos++;
        if (pos >= sizeof(buf) - 1) return NULL;
    }
}

/* Client thread: authenticate and process commands */
void *client_worker(void *arg) {
    (void)arg;
    while (running) {
        int sockfd;
        if (cq_pop(&client_q, &sockfd) != 0) break;
        if (!running) { close(sockfd); break; }

        const char *welcome = "SIMPLE-DROPBOX-SERVER v1\nSend: HELLO <username>\n";
        send_all(sockfd, welcome, strlen(welcome));

        char *line = read_line(sockfd);
        if (!line) { close(sockfd); continue; }
        char username[256] = {0};
        if (sscanf(line, "HELLO %255s", username) != 1) {
            const char *err = "Expected: HELLO <username>\n";
            send_all(sockfd, err, strlen(err));
            free(line);
            close(sockfd);
            continue;
        }
        free(line);

        send_all(sockfd, "AUTH OK\n", 8);
        ensure_user_dir(username);

        /* session loop */
        while (running) {
            char *cmdline = read_line(sockfd);
            if (!cmdline) break;
            char *p = cmdline;
            while (*p == ' ') p++;

            if (strncmp(p, "UPLOAD ", 7) == 0) {
                char fname[512]; long sz;
                if (sscanf(p+7, "%511s %ld", fname, &sz) != 2 || sz < 0) {
                    send_all(sockfd, "UPLOAD SYNTAX: UPLOAD <filename> <size>\n", 40);
                    free(cmdline);
                    continue;
                }
                void *buf = malloc((size_t)sz ? (size_t)sz : 1);
                if (!buf) { send_all(sockfd, "UPLOAD FAILED: NO MEMORY\n", 25); free(cmdline); continue; }
                ssize_t got = recv_all(sockfd, buf, (size_t)sz);
                if (got != sz) { free(buf); send_all(sockfd, "UPLOAD FAILED\n", 14); free(cmdline); continue; }

                task_t *t = calloc(1, sizeof(task_t));
                t->type = TASK_UPLOAD;
                t->username = strdup(username);
                t->filename = strdup(fname);
                t->data = buf;
                t->data_len = (size_t)sz;
                t->resp = task_response_create();
                if (tq_push(&task_q, t) != 0) {
                    send_all(sockfd, "SERVER BUSY\n", 11);
                    task_response_destroy(t->resp);
                    free(t->username); free(t->filename); free(t->data); free(t);
                    free(cmdline);
                    continue;
                }

                pthread_mutex_lock(&t->resp->lock);
                while (!t->resp->done) pthread_cond_wait(&t->resp->cond, &t->resp->lock);
                if (t->resp->msg) send_all(sockfd, t->resp->msg, strlen(t->resp->msg));
                pthread_mutex_unlock(&t->resp->lock);
                task_response_destroy(t->resp);
                free(cmdline);
                continue;
            }

            else if (strncmp(p, "DOWNLOAD ", 9) == 0) {
                char fname[512];
                if (sscanf(p+9, "%511s", fname) != 1) { send_all(sockfd, "DOWNLOAD SYNTAX\n", 16); free(cmdline); continue; }
                task_t *t = calloc(1, sizeof(task_t));
                t->type = TASK_DOWNLOAD;
                t->username = strdup(username);
                t->filename = strdup(fname);
                t->resp = task_response_create();
                if (tq_push(&task_q, t) != 0) {
                    send_all(sockfd, "SERVER BUSY\n", 11);
                    task_response_destroy(t->resp);
                    free(t->username); free(t->filename); free(t);
                    free(cmdline);
                    continue;
                }
                pthread_mutex_lock(&t->resp->lock);
                while (!t->resp->done) pthread_cond_wait(&t->resp->cond, &t->resp->lock);
                if (t->resp->success && t->resp->data) {
                    char hdr[64];
                    int h = snprintf(hdr, sizeof(hdr), "DOWNLOAD %zu\n", t->resp->data_len);
                    send_all(sockfd, hdr, h);
                    send_all(sockfd, t->resp->data, t->resp->data_len);
                } else {
                    if (t->resp->msg) send_all(sockfd, t->resp->msg, strlen(t->resp->msg));
                }
                pthread_mutex_unlock(&t->resp->lock);
                task_response_destroy(t->resp);
                free(cmdline);
                continue;
            }

            else if (strncmp(p, "DELETE ", 7) == 0) {
                char fname[512];
                if (sscanf(p+7, "%511s", fname) != 1) { send_all(sockfd, "DELETE SYNTAX\n", 14); free(cmdline); continue; }
                task_t *t = calloc(1, sizeof(task_t));
                t->type = TASK_DELETE;
                t->username = strdup(username);
                t->filename = strdup(fname);
                t->resp = task_response_create();
                if (tq_push(&task_q, t) != 0) {
                    send_all(sockfd, "SERVER BUSY\n", 11);
                    task_response_destroy(t->resp);
                    free(t->username); free(t->filename); free(t);
                    free(cmdline);
                    continue;
                }
                pthread_mutex_lock(&t->resp->lock);
                while (!t->resp->done) pthread_cond_wait(&t->resp->cond, &t->resp->lock);
                if (t->resp->msg) send_all(sockfd, t->resp->msg, strlen(t->resp->msg));
                pthread_mutex_unlock(&t->resp->lock);
                task_response_destroy(t->resp);
                free(cmdline);
                continue;
            }

            else if (strncmp(p, "LIST", 4) == 0) {
                task_t *t = calloc(1, sizeof(task_t));
                t->type = TASK_LIST;
                t->username = strdup(username);
                t->resp = task_response_create();
                if (tq_push(&task_q, t) != 0) {
                    send_all(sockfd, "SERVER BUSY\n", 11);
                    task_response_destroy(t->resp);
                    free(t->username); free(t);
                    free(cmdline);
                    continue;
                }
                pthread_mutex_lock(&t->resp->lock);
                while (!t->resp->done) pthread_cond_wait(&t->resp->cond, &t->resp->lock);
                if (t->resp->success) {
                    send_all(sockfd, t->resp->msg, strlen(t->resp->msg));
                    if (t->resp->data_len > 0 && t->resp->data) send_all(sockfd, t->resp->data, t->resp->data_len);
                } else {
                    if (t->resp->msg) send_all(sockfd, t->resp->msg, strlen(t->resp->msg));
                }
                pthread_mutex_unlock(&t->resp->lock);
                task_response_destroy(t->resp);
                free(cmdline);
                continue;
            }

            else if (strncmp(p, "BYE", 3) == 0) {
                free(cmdline);
                break;
            }

            else {
                send_all(sockfd, "Unknown command. Use UPLOAD/DOWNLOAD/DELETE/LIST/BYE\n", 51);
                free(cmdline);
            }
        }

        close(sockfd);
    }
    return NULL;
}

/* Setup listening socket */
int setup_listener(int port) {
    int fd;
    int opt = 1;
    struct sockaddr_in addr;
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc >= 2) port = atoi(argv[1]);

    signal(SIGINT, handle_sigint);

    if (cq_init(&client_q, CLIENT_QUEUE_CAP) != 0) {
        fprintf(stderr, "Failed to init client queue\n");
        return 1;
    }
    if (tq_init(&task_q, TASK_QUEUE_CAP) != 0) {
        fprintf(stderr, "Failed to init task queue\n");
        return 1;
    }

    /* start client threads */
    for (int i = 0; i < CLIENT_POOL_SIZE; ++i) {
        if (pthread_create(&client_threads[i], NULL, client_worker, NULL) != 0) {
            perror("pthread_create client");
        }
    }
    /* start worker threads */
    for (int i = 0; i < WORKER_POOL_SIZE; ++i) {
        if (pthread_create(&worker_threads[i], NULL, worker_fn, NULL) != 0) {
            perror("pthread_create worker");
        }
    }

    listen_fd = setup_listener(port);
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to setup listener\n");
        cq_close(&client_q);
        tq_close(&task_q);
        for (int i = 0; i < CLIENT_POOL_SIZE; ++i) pthread_join(client_threads[i], NULL);
        for (int i = 0; i < WORKER_POOL_SIZE; ++i) pthread_join(worker_threads[i], NULL);
        cq_destroy(&client_q);
        tq_destroy(&task_q);
        return 1;
    }

    printf("Server listening on port %d\n", port);

    while (running) {
        struct sockaddr_in cli_addr;
        socklen_t addrlen = sizeof(cli_addr);
        int clientfd = accept(listen_fd, (struct sockaddr*)&cli_addr, &addrlen);
        if (clientfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr.sin_addr, ipstr, sizeof(ipstr));
        printf("Accepted connection from %s:%d, fd=%d\n", ipstr, ntohs(cli_addr.sin_port), clientfd);
        if (cq_push(&client_q, clientfd) != 0) {
            close(clientfd);
        }
    }

    /* Shutdown */
    running = 0;
    cq_close(&client_q);
    tq_close(&task_q);
    if (listen_fd != -1) close(listen_fd);

    for (int i = 0; i < CLIENT_POOL_SIZE; ++i) pthread_join(client_threads[i], NULL);
    for (int i = 0; i < WORKER_POOL_SIZE; ++i) pthread_join(worker_threads[i], NULL);

    cq_destroy(&client_q);
    tq_destroy(&task_q);

    printf("Server shutdown cleanly\n");
    return 0;
}
