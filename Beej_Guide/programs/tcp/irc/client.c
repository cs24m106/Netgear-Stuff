/*
 * client.c - simple chat client (select-based)
 *
 * Build:
 *   gcc -Wall -O2 -o client client.c
 *
 * Run:
 *   ./client <server-hostname-or-ip>
 *
 * Type lines and press Enter to send; messages from other clients are printed by the client.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define PORT "3490"
#define MAXDATASIZE 512

ssize_t send_all(int sockfd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sockfd, buf + total, len - total, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue; // call cancelled by interrupts, are allow to send again
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s hostname\n", argv[0]);
        exit(1);
    }

    int sockfd, rv;
    struct addrinfo hints, *servinfo, *p;
    char s[INET6_ADDRSTRLEN];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(argv[1], PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    /* connect to first suitable result */
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) {
            perror("socket");
            continue;
        }

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) < 0) {
            close(sockfd);
            perror("connect");
            continue;
        }

        break;
    }

    if (p == NULL) {
        fprintf(stderr, "client: failed to connect\n");
        freeaddrinfo(servinfo);
        return 2;
    }

    inet_ntop(p->ai_family, get_in_addr((struct sockaddr*)p->ai_addr), s, sizeof s);
    printf("client: connected to %s\n", s);
    freeaddrinfo(servinfo);

    fd_set master, read_fds;
    FD_ZERO(&master);
    FD_SET(STDIN_FILENO, &master);
    FD_SET(sockfd, &master);
    int fdmax = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

    char buf[MAXDATASIZE];

    while (1) {
        read_fds = master;
        if (select(fdmax + 1, &read_fds, NULL, NULL, NULL) == -1) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* stdin? -> read a line and send to server */
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            if (fgets(buf, sizeof buf, stdin) == NULL) {
                /* EOF on stdin (Ctrl-D) -> exit */
                printf("client: exiting\n");
                break;
            }

            size_t len = strlen(buf);
            /* ensure message ends with newline */
            if (len == 0) continue;
            if (buf[len-1] != '\n') {
                /* not full line; send what we have (rare) */
                buf[len] = '\n';
                len++;
                buf[len] = '\0';
            }

            if (send_all(sockfd, buf, len) == -1) {
                perror("send");
                break;
            }
        }

        /* socket? -> receive and print */
        if (FD_ISSET(sockfd, &read_fds)) {
            ssize_t nbytes = recv(sockfd, buf, sizeof buf - 1, 0);
            if (nbytes <= 0) {
                if (nbytes == 0) {
                    printf("client: server closed connection\n");
                } else {
                    perror("recv");
                }
                break;
            }
            buf[nbytes] = '\0';
            fputs(buf, stdout);
            fflush(stdout);
        }
    }

    close(sockfd);
    return 0;
}
