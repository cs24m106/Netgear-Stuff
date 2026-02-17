#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>


void error(const char *msg){
	perror(msg);
	exit(1);
}

int main(int argc, char *argv[]){

	if(argc < 2){
		fprintf(stderr,"Port Number not provided\n");
		exit(1);
	}

	int sockfd, newsockfd, portno, n;
	int try = 3;
	char buffer[255];

	struct sockaddr_in serv_addr, cli_addr;
	socklen_t clilen;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if(sockfd<0)
		error("error openning Socket.");

	bzero((char *) &serv_addr, sizeof(serv_addr));
	portno = atoi(argv[1]);

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(portno);

	if(bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr))<0)
		error("Binding Failed.");

	listen(sockfd, 5); //max number of client can be connected
	clilen = sizeof(cli_addr);
	
	newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);

	if(newsockfd<0)
		error("Error on accept...");

	//continuous communication
	bzero(buffer, 255);
        n = read(newsockfd, buffer, 255);
        if(n<0)
        	error("Error on reading...");
        printf("Client Password: %s\n", buffer);
        if(strncmp("Logesh",buffer,6)!=0)
	{
	bzero(buffer, 255);
	strcpy(buffer,"Incorrect Password...");
	n= write(newsockfd, buffer, strlen(buffer));
        if(n<0)
        	error("Error on writing...");
	
	}	
	else{
	bzero(buffer,255);
	strcpy(buffer,"Password Correct. Proceed...");
	n = write(newsockfd, buffer, strlen(buffer));
	if(n<0)
		error("Error on writing...");

	while(1){
		bzero(buffer, 255);
		n = read(newsockfd, buffer, 255);
		if(n<0)
			error("Error on reading...");
		printf("Client: %s\n",buffer);
		bzero(buffer, 255);


		printf("Server: ");
		fgets(buffer, 255, stdin);

		n= write(newsockfd, buffer, strlen(buffer));
		if(n<0)
			error("Error on reading...");

		int i = strncmp("exit", buffer, 4);
		if(i==0)
			break;
	}
	}

	close(sockfd);
	close(newsockfd);
	return 0;
	
}

