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

int main(int argc, char *argv[]){
    int servSockfd;
    struct sockaddr_in servAddr;
    int portno;
    char *ip;
    char *buffer;

    FILE* fptr = fopen("ServerReport.txt","w");
    
    ip = malloc(sizeof(char)*15);
    //printf("Enter the port No: ");
    //scanf("%d", &portno);
    portno = atoi(argv[1]);
    sockAddress(&servAddr, portno, NULL);

    servSockfd  = socket(AF_INET, SOCK_DGRAM, 0);

    int rcvbuf = 512; // 1KB buffer
    setsockopt(servSockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    if(bind(servSockfd, (struct sockaddr *) &servAddr, sizeof(struct sockaddr)) < 0){
        printf("Error Binding.....\n");
        fprintf(fptr,"Error Binding.....\n");
        fclose(fptr);
    }

    buffer = (char *) malloc(sizeof(char)*255);
    char text[300] = "Client: ";
    while(1){
        printf("receiving....\n");
        fprintf(fptr,"\nreceiving....\n");
        bzero(buffer,sizeof(buffer));
        bzero(text, sizeof(text));
        socklen_t addrlen = sizeof(struct sockaddr);
        recvfrom(servSockfd, buffer, 255, 0, (struct sockaddr *) &servAddr, &addrlen);
        printf("Client: ");
        puts(buffer);
        strcat(text, buffer);
        fwrite(text,sizeof(char), sizeof(text), fptr);
        
        if(strncmp(buffer, "Msg:180",7) == 0){
            break;
        }
    }
        
    fclose(fptr);
    close(servSockfd);

}
