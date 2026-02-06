//
// Created by vspl007 on 12/19/25.
//
// ./filename <Server ip address> <portnumber>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

void error(char* msg) {
    perror(msg);
    exit(1);
}

int main() {

    int sockfd,port;
    char *buffer;
    char ip[50];
    struct sockaddr_in serv_addr;

    printf("Enter the port number: ");
    scanf("%d",&port);
    printf("Enter the ip address: ");
    scanf("%s",ip);
    buffer = (char*) malloc(255*sizeof(char));

    serv_addr.sin_port = htons(port);
    serv_addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &serv_addr.sin_addr.s_addr);

    printf("The Server Address: %d\t%s \n",serv_addr.sin_addr.s_addr, ip);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd<0) {
        error("Socket not created");
    }

    int result = connect(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr));
    if (result<0) {
        error("Connection Failed...");
    }

    printf("Enter the Password:: ");
    scanf("%s",buffer);

    send(sockfd, buffer, 255,0);

    recv(sockfd, buffer, 255, 0);
    printf("Server: %s\n",buffer);
    if (strcmp(buffer, "Password Correct") == 0) {
        while (1) {
            printf("Client: ");
            scanf("%s", buffer);
            send(sockfd, buffer, 255, 0);
            recv(sockfd, buffer, 255, 0);
            printf("Server: %s\n",buffer);

            if (strcmp(buffer, "exit") == 0) {
                break;
            }
        }
    }

    close(sockfd);
    return 0;
}