/* client.c - Simple Dropbox Client (Phase 1) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>

#define BUF_SIZE 1024

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

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <username>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    const char *username = argv[3];

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    printf("Connected to server %s:%d\n", server_ip, port);

    char buf[BUF_SIZE];
    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }

    char hello_msg[256];
    snprintf(hello_msg, sizeof(hello_msg), "HELLO %s\n", username);
    send_all(sock, hello_msg, strlen(hello_msg));

    n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }

    printf("Type commands (UPLOAD, DOWNLOAD, DELETE, LIST, BYE)\n");

    fd_set readfds;
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sock, &readfds);
        int maxfd = (sock > STDIN_FILENO ? sock : STDIN_FILENO) + 1;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms
        int sel = select(maxfd, &readfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* Check if input from user */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char cmd[BUF_SIZE];
            if (!fgets(cmd, sizeof(cmd), stdin)) break;

            if (strncmp(cmd, "UPLOAD ", 7) == 0) {
                char fname[512];
                if (sscanf(cmd + 7, "%511s", fname) != 1) {
                    printf("Syntax: UPLOAD <filename>\n");
                    continue;
                }
                FILE *f = fopen(fname, "rb");
                if (!f) {
                    perror("fopen");
                    continue;
                }
                fseek(f, 0, SEEK_END);
                long len = ftell(f);
                rewind(f);
                void *data = malloc(len);
                if (!data) {
                    fclose(f);
                    printf("No memory\n");
                    continue;
                }
                fread(data, 1, len, f);
                fclose(f);

                char header[600];
                int h = snprintf(header, sizeof(header), "UPLOAD %s %ld\n", fname, len);
                send_all(sock, header, (size_t)h);
                send_all(sock, data, (size_t)len);
                free(data);
            } 
            else if (strncmp(cmd, "DOWNLOAD ", 9) == 0) {
                send_all(sock, cmd, strlen(cmd));
            } 
            else if (strncmp(cmd, "DELETE ", 7) == 0) {
                send_all(sock, cmd, strlen(cmd));
            } 
            else if (strncmp(cmd, "LIST", 4) == 0) {
                send_all(sock, "LIST\n", 5);
            } 
            else if (strncmp(cmd, "BYE", 3) == 0) {
                send_all(sock, "BYE\n", 4);
                break;
            } 
            else {
                printf("Unknown command.\n");
            }
        }

        /* Check if message from server */
        if (FD_ISSET(sock, &readfds)) {
            char buf[1024];
            ssize_t r = recv(sock, buf, sizeof(buf) - 1, 0);
            if (r <= 0) {
                printf("Server closed connection.\n");
                break;
            }
            buf[r] = '\0';

            /* Print safely without truncation warning */
            char tmp[1025];
            size_t len = strlen(buf);
            if (len > sizeof(tmp) - 2) len = sizeof(tmp) - 2;
            memcpy(tmp, buf, len);
            tmp[len] = '\n';
            tmp[len + 1] = '\0';
            printf("%s", tmp);
        }
    }

    close(sock);
    printf("Disconnected.\n");
    return 0;
}
