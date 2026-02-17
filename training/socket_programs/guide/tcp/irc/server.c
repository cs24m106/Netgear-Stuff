/*
 * server.c - simple chat lobby server (select-based)
 *
 * Build:
 *   gcc -Wall -O2 -o server server.c
 *
 * Run:
 *   ./server
 *
 * Clients connect to PORT (default 3490). Server broadcasts each client's
 * newline-terminated message to every other connected client.
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
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT "9034"
#define MAXDATASIZE 512

/* send_all: ensure whole buffer sent */
ssize_t send_all(int sockfd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sockfd, buf + total, len - total, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* get printable addr string */
void addr_to_str(struct sockaddr *sa, char *out, size_t outlen) {
    if (sa->sa_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in*)sa;
        inet_ntop(AF_INET, &sin->sin_addr, out, outlen);
    } else {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)sa;
        inet_ntop(AF_INET6, &sin6->sin6_addr, out, outlen);
    }
}

int main(void) {
    int listener = -1, rv;
    struct addrinfo hints, *ai, *p;
    int yes = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;      /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;      /* use my IP */

    if ((rv = getaddrinfo(NULL, PORT, &hints, &ai)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    /* bind to first usable address */
    for (p = ai; p != NULL; p = p->ai_next) {
        listener = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listener < 0) continue;

        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

        if (bind(listener, p->ai_addr, p->ai_addrlen) < 0) {
            close(listener);
            continue;
        }

        break;
    }

    if (p == NULL) {
        fprintf(stderr, "server: failed to bind\n");
        freeaddrinfo(ai);
        return 2;
    }

    freeaddrinfo(ai);

    if (listen(listener, 10) < 0) {
        perror("listen");
        close(listener);
        return 3;
    }

    printf("server: listening on port %s\n", PORT);

    fd_set master, read_fds;
    FD_ZERO(&master);
    FD_ZERO(&read_fds);
    FD_SET(listener, &master);
    int fdmax = listener;

    /* main loop */
    while (1) {
        read_fds = master;
        if (select(fdmax + 1, &read_fds, NULL, NULL, NULL) == -1) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        for (int fd = 0; fd <= fdmax; ++fd) {
            if (!FD_ISSET(fd, &read_fds)) continue;

            if (fd == listener) {
                /* new connection */
                struct sockaddr_storage remoteaddr;
                socklen_t addrlen = sizeof remoteaddr;
                int newfd = accept(listener, (struct sockaddr*)&remoteaddr, &addrlen);
                if (newfd == -1) {
                    perror("accept");
                    continue;
                }

                /* add to master set */
                FD_SET(newfd, &master);
                if (newfd > fdmax) fdmax = newfd;

                char addrstr[INET6_ADDRSTRLEN];
                addr_to_str((struct sockaddr*)&remoteaddr, addrstr, sizeof addrstr);

                printf("server: new connection from %s (fd=%d)\n", addrstr, newfd);

                /* announce to other clients */
                char joinmsg[256];
                int n = snprintf(joinmsg, sizeof joinmsg, "server: client %d (%s) joined\n", newfd, addrstr);
                for (int j = 0; j <= fdmax; ++j) {
                    if (j == listener || j == newfd) continue;
                    if (FD_ISSET(j, &master)) {
                        send_all(j, joinmsg, (size_t)n);
                    }
                }
            } else {
                /* data from connected client */
                char buf[MAXDATASIZE];
                ssize_t nbytes = recv(fd, buf, sizeof buf - 1, 0);
                if (nbytes <= 0) {
                    /* connection closed or error */
                    if (nbytes == 0) {
                        printf("server: socket %d hung up\n", fd);
                    } else {
                        perror("recv");
                    }

                    /* announce departure */
                    char leavemsg[128];
                    int m = snprintf(leavemsg, sizeof leavemsg, "server: client %d left\n", fd);
                    for (int j = 0; j <= fdmax; ++j) {
                        if (j == listener || j == fd) continue;
                        if (FD_ISSET(j, &master)) {
                            send_all(j, leavemsg, (size_t)m);
                        }
                    }

                    close(fd);
                    FD_CLR(fd, &master);
                } else {
                    /* broadcast to everyone except the sender and listener */
                    buf[nbytes] = '\0';

                    /* remove trailing newline if present to avoid double newlines */
                    if (nbytes > 0 && buf[nbytes-1] == '\n') buf[nbytes-1] = '\0';

                    char out[MAXDATASIZE + 64];
                    int outlen = snprintf(out, sizeof out, "client %d: %s\n", fd, buf);

                    for (int j = 0; j <= fdmax; ++j) {
                        if (j == listener || j == fd) continue;
                        if (FD_ISSET(j, &master)) {
                            if (send_all(j, out, (size_t)outlen) == -1) {
                                /* if send fails, close that connection */
                                perror("send_all");
                                close(j);
                                FD_CLR(j, &master);
                            }
                        }
                    }
                }
            }
        } /* for fd loop */
    } /* while */

    /* cleanup */
    for (int i = 0; i <= fdmax; ++i) {
        if (FD_ISSET(i, &master)) close(i);
    }

    return 0;
}
