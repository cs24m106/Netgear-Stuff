#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>


void sockAddress(struct sockaddr_in *temp, int portno, char *ip){
    temp->sin_family = AF_INET;
    temp->sin_port = htons(portno);
    if(ip != NULL){
        inet_pton(AF_INET, ip, &temp->sin_addr.s_addr);
    }
    else{
        temp->sin_addr.s_addr = INADDR_ANY;
    }
}

int main(){
    int servSockfd;
    struct sockaddr_in servAddr;
    int portno;
    char *ip;
    char *buffer;

    ip = malloc(sizeof(char)*15);
    printf("Enter the port No: ");
    scanf("%d", &portno);
    
    sockAddress(&servAddr, portno, NULL);

    servSockfd  = socket(AF_INET, SOCK_DGRAM, 0);

    int rcvbuf = 512; // 1KB buffer
    setsockopt(servSockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    if(bind(servSockfd, (struct sockaddr *) &servAddr, sizeof(struct sockaddr)) < 0){
        printf("Error Binding.....\n");
    }

    buffer = (char *) malloc(sizeof(char)*255);

    while(1){
        printf("receiving....\n");
        socklen_t addrlen = sizeof(struct sockaddr);
        recvfrom(servSockfd, buffer, 255, 0, (struct sockaddr *) &servAddr, &addrlen);
        printf("Client: ");
        puts(buffer);
        usleep(5000);
        if(strncmp(buffer, "bye",3) == 0){
            break;
        }
    }
        

    close(servSockfd);

}
