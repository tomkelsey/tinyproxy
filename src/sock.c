/* $Id: sock.c,v 1.8 2001-09-16 20:11:07 rjkaes Exp $
 *
 * Sockets are created and destroyed here. When a new connection comes in from
 * a client, we need to copy the socket and the create a second socket to the
 * remote server the client is trying to connect to. Also, the listening
 * socket is created and destroyed here. Sounds more impressive than it
 * actually is.
 *
 * Copyright (C) 1998  Steven Young
 * Copyright (C) 1999  Robert James Kaes (rjkaes@flarenet.com)
 * Copyright (C) 2000  Chris Lightfoot (chris@ex-parrot.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include "tinyproxy.h"

#include "dnscache.h"
#include "log.h"
#include "sock.h"
#include "utils.h"

/*
 * The mutex is used for locking around any calls which access global
 * variables.
 *	- rjkaes
 */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

#define LOCK()   pthread_mutex_lock(&mutex);
#define UNLOCK() pthread_mutex_unlock(&mutex);

/* This routine is so old I can't even remember writing it.  But I do
 * remember that it was an .h file because I didn't know putting code in a
 * header was bad magic yet.  anyway, this routine opens a connection to a
 * system and returns the fd.
 *	- steve
 *
 * Cleaned up some of the code to use memory routines which are now the
 * default. Also, the routine first checks to see if the address is in
 * dotted-decimal form before it does a name lookup.
 *      - rjkaes
 */
int opensock(char *ip_addr, uint16_t port)
{
	int sock_fd;
	struct sockaddr_in port_info;
	int ret;

	assert(ip_addr != NULL);
	assert(port > 0);

	memset((struct sockaddr*)&port_info, 0, sizeof(port_info));

	port_info.sin_family = AF_INET;

	/* Lookup and return the address if possible */
	ret = dnscache(&port_info.sin_addr, ip_addr);

	if (ret < 0) {
		log_message(LOG_ERR, "Could not lookup address [%s].", ip_addr);
		return -1;
	}

	port_info.sin_port = htons(port);

	if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		log_message(LOG_ERR, "Could not create socket because of '%s'.", strerror(errno));
		return -1;
	}

	if (connect(sock_fd, (struct sockaddr*)&port_info, sizeof(port_info)) < 0) {
		log_message(LOG_ERR, "Could not connect socket because of '%s'", strerror(errno));
		return -1;
	}

	return sock_fd;
}

/*
 * Set the socket to non blocking -rjkaes
 */
int socket_nonblocking(int sock)
{
	int flags;

	assert(sock >= 0);

	flags = fcntl(sock, F_GETFL, 0);
	return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Set the socket to blocking -rjkaes
 */
int socket_blocking(int sock)
{
	int flags;

	assert(sock >= 0);

	flags = fcntl(sock, F_GETFL, 0);
	return fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
}

/*
 * Start listening to a socket. Create a socket with the selected port.
 * The size of the socket address will be returned to the caller through
 * the pointer, while the socket is returned as a default return.
 *	- rjkaes
 */
int listen_sock(uint16_t port, socklen_t *addrlen)
{
	int listenfd;
	const int on = 1;
	struct sockaddr_in addr;

	assert(port > 0);
	assert(addrlen != NULL);

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	if (config.ipAddr) {
		addr.sin_addr.s_addr = inet_addr(config.ipAddr);
	} else {
		addr.sin_addr.s_addr = inet_addr("0.0.0.0");
	}
	
	bind(listenfd, (struct sockaddr *)&addr, sizeof(addr));

	listen(listenfd, MAXLISTEN);

	*addrlen = sizeof(addr);

	return listenfd;
}

/*
 * Takes a socket descriptor and returns the string contain the peer's
 * IP address.
 */
char *getpeer_ip(int fd, char *ipaddr)
{
	struct sockaddr_in name;
	size_t namelen = sizeof(name);

	assert(fd >= 0);
	assert(ipaddr != NULL);

	if (getpeername(fd, (struct sockaddr*)&name, &namelen) != 0) {
		log_message(LOG_ERR, "Connect: 'could not get peer name'");
	} else {
		strlcpy(ipaddr,
			inet_ntoa(*(struct in_addr*)&name.sin_addr.s_addr),
			PEER_IP_LENGTH);
	}

	return ipaddr;
}

/*
 * Takes a socket descriptor and returns the string containing the peer's
 * address.
 */
char *getpeer_string(int fd, char *string)
{
	struct sockaddr_in name;
	size_t namelen = sizeof(name);
	struct hostent *peername;

	assert(fd >= 0);
	assert(string != NULL);

	if (getpeername(fd, (struct sockaddr *)&name, &namelen) != 0) {
		log_message(LOG_ERR, "Connect: 'could not get peer name'");
	} else {
		LOCK();
		peername = gethostbyaddr((char *)&name.sin_addr.s_addr,
					 sizeof(name.sin_addr.s_addr),
					 AF_INET);
		if (peername)
			strlcpy(string, peername->h_name, PEER_STRING_LENGTH);
		UNLOCK();
	}

	return string;
}

/*
 * Write the buffer to the socket. If an EINTR occurs, pick up and try
 * again.
 */
ssize_t safe_write(int fd, const void *buffer, size_t count)
{
	ssize_t len;

	do {
		len = write(fd, buffer, count);
	} while (len < 0 && errno == EINTR);

	return len;
}

/*
 * Matched pair for safe_write(). If an EINTR occurs, pick up and try
 * again.
 */
ssize_t safe_read(int fd, void *buffer, size_t count)
{
	ssize_t len;

	do {
		len = read(fd, buffer, count);
	} while (len < 0 && errno == EINTR);

	return len;
}

/*
 * Reads in a line of text one character at a time. Finishes when either a
 * newline is detected, or maxlen characters have been read. The function
 * will actually copy one less than maxlen into the buffer. In other words,
 * the returned string will _always_ be '\0' terminated.
 */
ssize_t readline(int fd, char *ptr, size_t maxlen)
{
	size_t n;
	ssize_t rc;
	char c;

	assert(fd >= 0);
	assert(ptr != NULL);

	for (n = 1; n < maxlen; n++) {
	again:
		if ((rc = read(fd, &c, 1)) == 1) {
			*ptr++ = c;
			if (c == '\n')
				break;
		} else if (rc == 0) {
			if (n == 1)
				return 0;
			else
				break;
		} else {
			if (errno == EINTR)
				goto again;
			return -1;
		}
	}

	/* Tack a NIL to the end to make is a standard "C" string */
	*ptr = '\0';
	return (ssize_t)n;
}
