#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


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
    int clientsockfd;
    struct sockaddr_in servAddr;
    int portno;
    char *ip;
    char *buffer;

    ip = malloc(sizeof(char)*15);
    printf("Enter the port No: ");
    scanf("%d", &portno);
    
    printf("Enter the IP address: ");
    scanf("%s",ip);

    //server address creation
    sockAddress(&servAddr, portno, ip);
    printf("The IP address: %d\t%s\n",servAddr.sin_addr.s_addr, NULL);
    printf("The port Number: %d\n",portno);

    //socket creation
    clientsockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(clientsockfd < 0)
        printf("Error creating socket...\n");

    buffer = malloc(sizeof(char)*255);

    int seq = 1;
    while(1){
        sprintf(buffer, "Msg:%d", seq);
        puts(buffer);

        int s_len = sendto(clientsockfd, buffer, 255, 0, (struct sockaddr *) &servAddr, sizeof(struct sockaddr));
        if(s_len < 0){
            printf("Error Sending .... \n");
        }
        
        if(strncmp(buffer, "Msg:200",7) == 0){
            break;
        }
        seq++;
    }

    close(clientsockfd);

}