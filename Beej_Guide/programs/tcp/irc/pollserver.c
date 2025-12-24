/*
** pollserver.c -- a cheezy multiperson chat server
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

#define PORT "9036"   // Port we're listening on
#define MAXDATASIZE 512

/*
 * Convert socket to IP address string.
 * addr: struct sockaddr_in or struct sockaddr_in6
 */
const char *inet_ntop2(void *addr, char *buf, size_t size)
{
	struct sockaddr_storage *sas = addr;
	struct sockaddr_in *sa4;
	struct sockaddr_in6 *sa6;
	void *src;

	switch (sas->ss_family) {
		case AF_INET:
			sa4 = addr;
			src = &(sa4->sin_addr);
			break;
		case AF_INET6:
			sa6 = addr;
			src = &(sa6->sin6_addr);
			break;
		default:
			return NULL;
	}

	return inet_ntop(sas->ss_family, src, buf, size);
}

/*
 * Return a listening socket.
 */
int get_listener_socket(void)
{
	int listener;	 // Listening socket descriptor
	int yes=1;		// For setsockopt() SO_REUSEADDR, below
	int rv;

	struct addrinfo hints, *ai, *p;

	// Get us a socket and bind it
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET; // set to AF_UNSPEC to allow both IPv4 & IPv6
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if ((rv = getaddrinfo(NULL, PORT, &hints, &ai)) != 0) {
		fprintf(stderr, "pollserver: %s\n", gai_strerror(rv));
		exit(1);
	}
	
	for(p = ai; p != NULL; p = p->ai_next) {
		listener = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol);
		if (listener < 0) { 
			continue;
		}
		
		// Lose the pesky "address already in use" error message
		setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

		if (bind(listener, p->ai_addr, p->ai_addrlen) < 0) {
			close(listener);
			continue;
		}

		break;
	}

	// If we got here, it means we didn't get bound
	if (p == NULL) {
		return -1;
	}

	freeaddrinfo(ai); // All done with this

	// Listen
	if (listen(listener, 10) == -1) {
		return -1;
	}

	return listener;
}

/*
 * Add a new file descriptor to the set.
 */
void add_to_pfds(struct pollfd **pfds, int newfd, int *fd_count, int *fd_size) 
// why the need for **pfds? --> address of array pointer (*pfds) --> call by reference
// if we use *pfds --> array pointer call by value, its reallocated address is not reflected out the function scope
{
	// If we don't have room, add more space in the pfds array
	if (*fd_count == *fd_size) {
		*fd_size *= 2; // Double it
		*pfds = realloc(*pfds, sizeof(**pfds) * (*fd_size));
	}

	(*pfds)[*fd_count].fd = newfd;
	(*pfds)[*fd_count].events = POLLIN; // Check ready-to-read
	(*pfds)[*fd_count].revents = 0; // reset necessary? no, just to avoid stale bits before poll overwrites it.

	(*fd_count)++;
}

/*
 * Remove a file descriptor at a given index from the set.
 */
void del_from_pfds(struct pollfd pfds[], int i, int *fd_count)
{
	// Copy the one from the end over this one
	pfds[i] = pfds[*fd_count-1];

	(*fd_count)--;
	// add vector like downsize realloc when fd_count*2 ==  fd_size
}

/*
 * Handle incoming connections.
 */
void handle_new_connection(int listener, int *fd_count,
		int *fd_size, struct pollfd **pfds)
{
	struct sockaddr_storage remoteaddr; // Client address
	socklen_t addrlen;
	int newfd;  // Newly accept()ed socket descriptor
	char remoteIP[INET6_ADDRSTRLEN];

	addrlen = sizeof remoteaddr;
	newfd = accept(listener, (struct sockaddr *)&remoteaddr,
			&addrlen); // can we give null to *addrlen param?
	// If you pass a non-NULL addr, you must pass a non-NULL addrlen pointing to a socklen_t value that initially contains the size of the buffer pointed to by addr.

	if (newfd == -1) {
		perror("accept");
	} else {
		add_to_pfds(pfds, newfd, fd_count, fd_size); // pass array pointer by reference --> address of array pointer

		printf("pollserver: new connection from %s on socket %d\n",
				inet_ntop2(&remoteaddr, remoteIP, sizeof remoteIP), newfd);
	}
}

/*
 * Handle regular client data or client hangups.
 */
void handle_client_data(int listener, int *fd_count,
		struct pollfd *pfds, int *pfd_i) // _i --> index of the resp client's poll_file_descriptor
{
	char buf[MAXDATASIZE];	// Buffer for client data

	int nbytes = recv(pfds[*pfd_i].fd, buf, sizeof buf, 0);

	int sender_fd = pfds[*pfd_i].fd;

	if (nbytes <= 0) { // Got error or connection closed by client
		if (nbytes == 0) {
			// Connection closed
			printf("pollserver: socket %d hung up\n", sender_fd);
		} else {
			perror("recv");
		}

		close(pfds[*pfd_i].fd); // Bye!

		del_from_pfds(pfds, *pfd_i, fd_count);

		// reexamine the slot we just deleted
		(*pfd_i)--; // why decrement unnecessarily? update process_connections' for loop iterator (i) when elem is deleted within array

	} else { // We got some good data from a client
		printf("pollserver: recv from fd %d: %.*s", sender_fd,
				nbytes, buf);
		// Send to everyone!
		for(int j = 0; j < *fd_count; j++) {
			int dest_fd = pfds[j].fd;

			// Except the listener and ourselves
			if (dest_fd != listener && dest_fd != sender_fd) {
				if (send(dest_fd, buf, nbytes, 0) == -1) {
					perror("send");
				}
			}
		}
	}
}

/*
 * Process all existing connections.
 */
void process_connections(int listener, int *fd_count, int *fd_size,
		struct pollfd **pfds)
{
	for(int i = 0; i < *fd_count; i++) {

		// Check if someone's ready to read
		if ((*pfds)[i].revents & (POLLIN | POLLHUP)) {
			// We got one!!

			if ((*pfds)[i].fd == listener) {
				// If we're the listener, it's a new connection
				handle_new_connection(listener, fd_count, fd_size, pfds); 
				// to handle the posibility of array pointer itself being reallocated, we pass the array pointer by reference
			} else {
				// Otherwise we're just a regular client
				handle_client_data(listener, fd_count, *pfds, &i); // pass iterator (i) by ref, to handle update on elem deletion within array
				// when we handle the elements of array only, we can simply pass the array pointer by value
			}
		}
	}
}

/*
 * Main: create a listener and connection set, loop forever
 * processing connections.
 */
int main(void)
{
	int listener;	 // Listening socket descriptor

	// Start off with room for 5 connections
	// (We'll realloc as necessary)
	int fd_size = 5;
	int fd_count = 0;
	struct pollfd *pfds = malloc(sizeof *pfds * fd_size);

	// Set up and get a listening socket
	listener = get_listener_socket();

	if (listener == -1) {
		fprintf(stderr, "error getting listening socket\n");
		exit(1);
	}

	// Add the listener to set;
	// Report ready to read on incoming connection
	pfds[0].fd = listener;
	pfds[0].events = POLLIN;

	fd_count = 1; // For the listener
	
	printf("pollserver: waiting for connections @[%s]...\n", PORT);

	// Main loop
	for(;;) {
		int poll_count = poll(pfds, fd_count, -1);

		if (poll_count == -1) {
			perror("poll");
			exit(1);
		}
		// shouldn't we handle poll_count == 0 --> timeout, applying continue or exit or something? but here timeout=-1, i.e. wait indefinitly, ig we can ignore this check??
		// Not strictly necessary in your code path with -1 timeout, but checking for 0 and continuing is harmless defensive programming.

		// Run through connections looking for data to read
		process_connections(listener, &fd_count, &fd_size, &pfds);
	}

	free(pfds);
}

