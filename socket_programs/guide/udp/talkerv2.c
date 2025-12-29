/*
** talker.c -- a datagram "client" demo
** waits briefly for ICMPv6 error after sendto()
** IPv6 loopback address = ::1
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <linux/errqueue.h>

#define SERVERPORT "4950"   // the port users will be connecting to
#define WAIT_TIMEOUT_MS 3000   // wait 3 seconds for ICMP error

static void read_icmp_error(int sockfd)
{
	char cbuf[512];
	char dummy;
	struct iovec iov;
	struct msghdr msg;
	ssize_t n;

	memset(&iov, 0, sizeof(iov));
	iov.iov_base = &dummy;
	iov.iov_len  = sizeof(dummy);

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);

	n = recvmsg(sockfd, &msg, MSG_ERRQUEUE);
	if (n < 0) {
		perror("recvmsg(MSG_ERRQUEUE)");
		return;
	}

	for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	     cmsg != NULL;
	     cmsg = CMSG_NXTHDR(&msg, cmsg)) {

		if (cmsg->cmsg_level == IPPROTO_IPV6 &&
		    cmsg->cmsg_type == IPV6_RECVERR) {

			struct sock_extended_err *serr =
				(struct sock_extended_err *)CMSG_DATA(cmsg);

			printf("ICMPv6 error received:\n");
			printf("  errno : %u (%s)\n",
			       serr->ee_errno, strerror(serr->ee_errno));
			printf("  origin: %u\n", serr->ee_origin);
			printf("  type  : %u\n", serr->ee_type);
			printf("  code  : %u\n", serr->ee_code);
		}
	}
}

int main(int argc, char *argv[])
{
	int sockfd;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int numbytes;
	int on = 1;

	if (argc != 3) {
		fprintf(stderr,"usage: talker hostname message\n");
		exit(1);
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET6; // set to AF_INET to use IPv4
	hints.ai_socktype = SOCK_DGRAM;

	rv = getaddrinfo(argv[1], SERVERPORT, &hints, &servinfo);
	if (rv != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and make a socket
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				     p->ai_protocol)) == -1) {
			perror("talker: socket");
			continue;
		}
		break;
	}

	if (p == NULL) {
		fprintf(stderr, "talker: failed to create socket\n");
		return 2;
	}

	/* Enable reception of ICMPv6 errors (Linux-specific) */
	if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_RECVERR,
	               &on, sizeof(on)) == -1) {
		perror("setsockopt(IPV6_RECVERR)");
	}

	/* Original sendto() — unchanged */
	if ((numbytes = sendto(sockfd, argv[2], strlen(argv[2]), 0,
	                       p->ai_addr, p->ai_addrlen)) == -1) {
		perror("talker: sendto");
		exit(1);
	}

	printf("talker: sent %d bytes to %s\n", numbytes, argv[1]);

	/* Wait for ICMP error up to timeout */
	struct pollfd pfd;
	pfd.fd = sockfd;
	pfd.events = POLLERR;

	int pret = poll(&pfd, 1, WAIT_TIMEOUT_MS);
	if (pret == 0) {
		printf("No ICMP error received within timeout\n");
	} else if (pret < 0) {
		perror("poll");
	} else {
		if (pfd.revents & POLLERR) {
			read_icmp_error(sockfd);
		}
	}

	freeaddrinfo(servinfo);
	close(sockfd);
	return 0;
}
