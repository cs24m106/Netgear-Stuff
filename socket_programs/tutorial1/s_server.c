//
// Created by vspl007 on 12/19/25.
//
// ./filename <IP_Address> <portNo>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void error(char* msg) {
    perror(msg);
    exit(1);
}

void SocketAddress(struct sockaddr_in *temp,char *ip, int port) {
    temp->sin_family = AF_INET;
    temp->sin_port = htons(port);
    if (ip == NULL) {
        temp->sin_addr.s_addr = INADDR_ANY;
    }else {
        inet_pton(AF_INET, ip, &temp->sin_addr.s_addr);
    }
}

int main() {

    int sockfd;
    char* ip;
    int port;
    printf("Enter the port number: ");
    scanf("%d", &port);
    struct sockaddr_in servaddr, clientaddr;

    SocketAddress(&servaddr, NULL, port);
    //servaddr.sin_family = AF_INET;
    //servaddr.sin_port = htons(port);
    //servaddr.sin_addr.s_addr = INADDR_ANY;
    char buffer[255];

    printf("The IP address: %d\t%s\n",servaddr.sin_addr.s_addr, NULL);
    printf("The port Number: %d\n",port);


    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    printf("Socket FD: %d\n", sockfd);
    if (sockfd < 0) {
        printf("Socket Created Failed....\n");
    }

    int bind_result = bind(sockfd, (struct sockaddr *) &servaddr, sizeof(servaddr));
    if  (bind_result < 0) {
        printf("Bind Failed....\n");
    }
    int listenresult = listen(sockfd, 2);
    printf("Listenresult: %d\n",listenresult);
    if (listenresult < 0) {
        printf("Listening Socket Failed....\n");
    }

    int clientaddresssize = sizeof(struct sockaddr_in);
    printf("Address Size: %d\n",clientaddresssize);
    int clientsockfd = accept(sockfd, (struct sockaddr*) &clientaddr, &clientaddresssize);
    printf("clientscokfd : %d\n", clientsockfd);

    recv(clientsockfd, buffer, 255, 0);
    printf("Client: %s\n",buffer);

    if (strncmp(buffer, "arun",4   ) == 0) {
        strcpy(buffer, "Password Correct");
        send(clientsockfd, buffer, 255, 0);
        while (1) {
            recv(clientsockfd, buffer, 255, 0);
            printf("Client: %s\n",buffer);
            printf("Server: ");
            scanf("%s", buffer);
            send(clientsockfd, buffer, 255, 0);
            if (strcmp(buffer, "exit") == 0) {
                break;
            }
        }
    }
    else {
        strcpy(buffer, "Password Incorrect");
        send(clientsockfd, buffer, strlen(buffer), 0);
    }
    close(sockfd);
    close(clientsockfd);
}

