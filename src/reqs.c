/* $Id: reqs.c,v 1.54 2002-04-09 20:06:24 rjkaes Exp $
 *
 * This is where all the work in tinyproxy is actually done. Incoming
 * connections have a new thread created for them. The thread then
 * processes the headers from the client, the response from the server,
 * and then relays the bytes between the two.
 * If TUNNEL_SUPPORT is enabled, then tinyproxy will actually work
 * as a simple buffering TCP tunnel. Very cool! (Robert actually uses
 * this feature for a buffering NNTP tunnel.)
 *
 * Copyright (C) 1998	    Steven Young
 * Copyright (C) 1999-2001  Robert James Kaes (rjkaes@flarenet.com)
 * Copyright (C) 2000       Chris Lightfoot (chris@ex-parrot.com)
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

#include "acl.h"
#include "anonymous.h"
#include "buffer.h"
#include "conns.h"
#include "filter.h"
#include "hashmap.h"
#include "log.h"
#include "regexp.h"
#include "reqs.h"
#include "sock.h"
#include "stats.h"
#include "utils.h"

#define HTTP400ERROR "Unrecognizable request. Only HTTP is allowed."
#define HTTP500ERROR "Unable to connect to remote server."
#define HTTP503ERROR "Internal server error."

/*
 * Maximum length of a HTTP line
 */
#define HTTP_LINE_LENGTH (MAXBUFFSIZE / 6)

/*
 * Macro to help test if the Upstream proxy supported is compiled in and
 * enabled.
 */
#ifdef UPSTREAM_SUPPORT
#  define UPSTREAM_CONFIGURED() (config.upstream_name && config.upstream_port != -1)
#else
#  define UPSTREAM_CONFIGURED() (0)
#endif

/*
 * Macro to help test if tunnel support is compiled in, and is enabled.
 */
#ifdef TUNNEL_SUPPORT
#  define TUNNEL_CONFIGURED() (config.tunnel_name && config.tunnel_port != -1)
#else
#  define TUNNEL_CONFIGURED() (0)
#endif

/*
 * Codify the test for the carriage return and new line characters.
 */
#define CHECK_CRLF(header, len) ((len == 1 && header[0] == '\n') || (len == 2 && header[0] == '\r' && header[1] == '\n'))

/*
 * Read in the first line from the client (the request line for HTTP
 * connections. The request line is allocated from the heap, but it must
 * be freed in another function.
 */
static int
read_request_line(struct conn_s *connptr)
{
	size_t len;

      retry:
	len = readline(connptr->client_fd, &connptr->request_line);
	if (len <= 0) {
		log_message(LOG_ERR,
			    "read_request_line: Client (file descriptor: %d) closed socket before read.",
			    connptr->client_fd);
		safefree(connptr->request_line);
		return -1;
	}

	/*
	 * Strip the new line and character return from the string.
	 */
	if (chomp(connptr->request_line, len) == len) {
		/*
		 * If the number of characters removed is the same as the
		 * length then it was a blank line. Free the buffer and
		 * try again (since we're looking for a request line.)
		 */
		safefree(connptr->request_line);
		goto retry;
	}

	log_message(LOG_CONN, "Request (file descriptor %d): %s",
		    connptr->client_fd, connptr->request_line);

	return 0;
}

/*
 * This structure holds the information pulled from a URL request.
 */
struct request_s {
	char *method;
	char *protocol;

	char *host;
	uint16_t port;

	char *path;
};

static void
free_request_struct(struct request_s *request)
{
	if (!request)
		return;

	safefree(request->method);
	safefree(request->protocol);

	safefree(request->host);
	safefree(request->path);

	safefree(request);
}

/*
 * Pull the information out of the URL line.
 */
static int
extract_http_url(const char *url, struct request_s *request)
{
	request->host = safemalloc(strlen(url) + 1);
	request->path = safemalloc(strlen(url) + 1);

	if (!request->host || !request->path) {
		safefree(request->host);
		safefree(request->path);

		return -1;
	}

	if (sscanf
	    (url, "http://%[^:/]:%hu%s", request->host, &request->port,
	     request->path) == 3) ;
	else if (sscanf(url, "http://%[^/]%s", request->host, request->path) == 2)
		request->port = 80;
	else if (sscanf(url, "http://%[^:/]:%hu", request->host, &request->port)
		 == 2)
		strcpy(request->path, "/");
	else if (sscanf(url, "http://%[^/]", request->host) == 1) {
		request->port = 80;
		strcpy(request->path, "/");
	} else {
		log_message(LOG_ERR, "extract_http_url: Can't parse URL.");

		safefree(request->host);
		safefree(request->path);

		return -1;
	}

	return 0;
}

/*
 * Extract the URL from a SSL connection.
 */
static int
extract_ssl_url(const char *url, struct request_s *request)
{
	request->host = safemalloc(strlen(url) + 1);
	if (!request->host)
		return -1;

	if (sscanf(url, "%[^:]:%hu", request->host, &request->port) == 2) ;
	else if (sscanf(url, "%s", request->host) == 1)
		request->port = 443;
	else {
		log_message(LOG_ERR, "extract_ssl_url: Can't parse URL.");

		safefree(request->host);
		return -1;
	}

	return 0;
}

/*
 * Create a connection for HTTP connections.
 */
static int
establish_http_connection(struct conn_s *connptr, struct request_s *request)
{
	if (write_message(connptr->server_fd,
			  "%s %s HTTP/1.0\r\n",
			  request->method, request->path) < 0)
		return -1;
	
	if (write_message(connptr->server_fd, "Host: %s\r\n", request->host) < 0)
		return -1;

	/*
	 * Send the Connection header since we don't support persistant
	 * connections.
	 */
	if (safe_write(connptr->server_fd, "Connection: close\r\n", 19) < 0)
		return -1;

	return 0;
}

/*
 * These two defines are for the SSL tunnelling.
 */
#define SSL_CONNECTION_RESPONSE "HTTP/1.0 200 Connection established\r\n"
#define PROXY_AGENT "Proxy-agent: " PACKAGE "/" VERSION "\r\n"

/*
 * Send the appropriate response to the client to establish a SSL
 * connection.
 */
static inline int
send_ssl_response(struct conn_s *connptr)
{
	if (safe_write
	    (connptr->client_fd, SSL_CONNECTION_RESPONSE,
	     strlen(SSL_CONNECTION_RESPONSE)) < 0)
		return -1;

	if (safe_write(connptr->client_fd, PROXY_AGENT, strlen(PROXY_AGENT)) < 0)
		return -1;

	if (safe_write(connptr->client_fd, "\r\n", 2) < 0)
		return -1;

	return 0;
}

/*
 * Break the request line apart and figure out where to connect and
 * build a new request line. Finally connect to the remote server.
 */
static struct request_s *
process_request(struct conn_s *connptr)
{
	char *url;
	struct request_s *request;

	int ret;

	size_t request_len;

	/* NULL out all the fields so free's don't cause segfaults. */
	request = safecalloc(1, sizeof(struct request_s));
	if (!request)
		return NULL;

	request_len = strlen(connptr->request_line) + 1;

	request->method = safemalloc(request_len);
	url = safemalloc(request_len);
	request->protocol = safemalloc(request_len);

	if (!request->method || !url || !request->protocol) {
		safefree(url);
		free_request_struct(request);

		return NULL;
	}

	ret =
	    sscanf(connptr->request_line, "%[^ ] %[^ ] %[^ ]",
		   request->method, url, request->protocol);
	if (ret < 2) {
		log_message(LOG_ERR,
			    "process_request: Bad Request on file descriptor %d",
			    connptr->client_fd);
		httperr(connptr, 400, "Bad Request. No request found.");

		safefree(url);
		free_request_struct(request);

		return NULL;
	}
	/* 
	 * NOTE: We need to add code for the simple HTTP/0.9 style GET
	 * request.
	 */

	if (!url) {
		log_message(LOG_ERR,
			    "process_request: Null URL on file descriptor %d",
			    connptr->client_fd);
		httperr(connptr, 400, "Bad Request. Null URL.");

		safefree(url);
		free_request_struct(request);

		return NULL;
	}

	if (strncasecmp(url, "http://", 7) == 0) {
		/* Make sure the first four characters are lowercase */
		memcpy(url, "http", 4);

		if (extract_http_url(url, request) < 0) {
			httperr(connptr, 400,
				"Bad Request. Could not parse URL.");

			safefree(url);
			free_request_struct(request);

			return NULL;
		}
	} else if (strcmp(request->method, "CONNECT") == 0) {
		if (extract_ssl_url(url, request) < 0) {
			httperr(connptr, 400,
				"Bad Request. Could not parse URL.");

			safefree(url);
			free_request_struct(request);

			return NULL;
		}
		
		connptr->connect_method = TRUE;
	} else {
		log_message(LOG_ERR,
			    "process_request: Unknown URL type on file descriptor %d",
			    connptr->client_fd);
		httperr(connptr, 400, "Bad Request. Unknown URL type.");

		safefree(url);
		free_request_struct(request);

		return NULL;
	}

	safefree(url);

#ifdef FILTER_ENABLE
	/*
	 * Filter restricted domains
	 */
	if (config.filter) {
		if (filter_url(request->host)) {
			update_stats(STAT_DENIED);

			log_message(LOG_NOTICE,
				    "Proxying refused on filtered domain \"%s\"",
				    request->host);
			httperr(connptr, 404,
				"Connection to filtered domain is now allowed.");

			free_request_struct(request);

			return NULL;
		}
	}
#endif

	/*
	 * Check to see if they're requesting the stat host
	 */
	if (config.stathost && strcmp(config.stathost, request->host) == 0) {
		log_message(LOG_NOTICE, "Request for the stathost.");

		free_request_struct(request);

		showstats(connptr);
		return NULL;
	}

	/*
	 * Break apart the protocol and update the connection structure.
	 */
	if (strncasecmp(request->protocol, "http", 4) == 0) {
		memcpy(request->protocol, "HTTP", 4);
		sscanf(request->protocol, "HTTP/%u.%u",
		       &connptr->protocol.major, &connptr->protocol.minor);
	}

	return request;
}

/*
 * pull_client_data is used to pull across any client data (like in a
 * POST) which needs to be handled before an error can be reported, or
 * server headers can be processed.
 *	- rjkaes
 */
static int
pull_client_data(struct conn_s *connptr, unsigned long int length)
{
	char *buffer;
	ssize_t len;

	buffer = safemalloc(MAXBUFFSIZE);
	if (!buffer)
		return -1;

	do {
		len =
		    safe_read(connptr->client_fd, buffer,
			      min(MAXBUFFSIZE, length));

		if (len <= 0) {
			safefree(buffer);
			return -1;
		}

		if (!connptr->response_message_sent) {
			if (safe_write(connptr->server_fd, buffer, len) < 0) {
				safefree(buffer);
				return -1;
			}
		}

		length -= len;
	} while (length > 0);

	safefree(buffer);
	return 0;
}

#ifdef XTINYPROXY_ENABLE
/*
 * Add the X-Tinyproxy header to the collection of headers being sent to
 * the server.
 *	-rjkaes
 */
static int
add_xtinyproxy_header(struct conn_s *connptr)
{
	char ipaddr[PEER_IP_LENGTH];

	/*
	 * Don't try to send if we have an invalid server handle.
	 */
	if (connptr->server_fd == -1)
		return 0;

	return write_message(connptr->server_fd,
			     "X-Tinyproxy: %s\r\n",
			     getpeer_ip(connptr->client_fd, ipaddr));
}
#endif				/* XTINYPROXY */

/*
 * Take a complete header line and break it apart (into a key and the data.)
 * Now insert this information into the hashmap for the connection so it
 * can be retrieved and manipulated later.
 */
static inline int
add_header_to_connection(hashmap_t hashofheaders, char *header, size_t len)
{
	char *sep;
	size_t data_len;

	/* Get rid of the new line and return at the end */
	chomp(header, len);

	sep = strchr(header, ':');
	if (!sep)
		return -1;

	/* Blank out colons, spaces, and tabs. */
	while (*sep == ':' || *sep == ' ' || *sep == '\t')
		*sep++ = '\0';
	
	data_len = strlen(sep) + 1; /* need to add the null to the length */
	return hashmap_insert(hashofheaders, header, sep, data_len);
}

/*
 * Read all the headers from the stream
 */
static int
get_all_headers(int fd, hashmap_t hashofheaders)
{
	char *header;
	ssize_t len;

	for (;;) {
		if ((len = readline(fd, &header)) <= 0) {
			return -1;
		}

		/*
		 * If we received just a CR LF on a line, the headers are
		 * finished.
		 */
		if (CHECK_CRLF(header, len))
			break;

		if (add_header_to_connection(hashofheaders, header, len) < 0)
			return -1;
	}

	return 0;
}

/*
 * Extract the headers to remove.  These headers were listed in the Connection
 * header sent via the client (which is stored in data right now.)
 */
static int
remove_connection_headers(hashmap_t hashofheaders, char* data, ssize_t len)
{
	char* ptr;

	/*
	 * Go through the data line and replace any special characters with
	 * a NULL.
	 */
	ptr = data;
	while ((ptr = strpbrk(ptr, "()<>@,;:\\\"/[]?={} \t"))) {
		*ptr++ = '\0';
	}

	/*
	 * All the tokens are separated by NULLs.  Now go through the tokens
	 * and remove them from the hashofheaders.
	 */
	ptr = data;
	while (ptr < data + len) {
		DEBUG2("Removing header [%s]", ptr);
		hashmap_remove(hashofheaders, ptr);

		/* Advance ptr to the next token */
		ptr += strlen(ptr) + 1;
		while (*ptr == '\0')
			ptr++;
	}

	return 0;
}

/*
 * Number of buckets to use internally in the hashmap.
 */
#define HEADER_BUCKETS 32

/*
 * Here we loop through all the headers the client is sending. If we
 * are running in anonymous mode, we will _only_ send the headers listed
 * (plus a few which are required for various methods).
 *	- rjkaes
 */
static int
process_client_headers(struct conn_s *connptr)
{
	static char *skipheaders[] = {
		"host",
		"connection",
		"keep-alive",
		"proxy-authenticate",
		"proxy-authorization",
		"te",
		"trailers",
		"transfer-encoding",
		"upgrade"
	};
	int i;
	hashmap_t hashofheaders;
	vector_t listofheaders;
	long content_length = -1;

	char *data, *header;
	size_t len;

	hashofheaders = hashmap_create(HEADER_BUCKETS);
	if (!hashofheaders)
		return -1;

	/*
	 * Get all the headers from the client in a big hash.
	 */
	if (get_all_headers(connptr->client_fd, hashofheaders) < 0) {
		log_message(LOG_WARNING, "Could not retrieve all the headers from the client");

		hashmap_delete(hashofheaders);
		return -1;
	}
	
	/*
	 * Don't send headers if there's already an error, or if this was
	 * a CONNECT method (unless upstream proxy is in use.)
	 */
	if (connptr->server_fd == -1
	    || (connptr->connect_method && !UPSTREAM_CONFIGURED())) {
		log_message(LOG_INFO, "Not sending client headers to remote machine");
		hashmap_delete(hashofheaders);
		return 0;
	}

	/*
	 * See if there is a "Connection" header.  If so, we need to do a bit
	 * of processing. :)
	 */
	len = hashmap_search(hashofheaders, "connection", (void **)&data);
	if (len > 0) {
		/*
		 * Go through the tokens in the connection header and
		 * remove the headers from the hash.
		 */
		remove_connection_headers(hashofheaders, data, len);
		hashmap_remove(hashofheaders, "connection");
	}

	/*
	 * See if there is a "Content-Length" header.  If so, again we need
	 * to do a bit of processing.
	 */
	len = hashmap_search(hashofheaders, "content-length", (void **)&data);
	if (len > 0) {
		content_length = atol(data);
	}

	/*
	 * See if there is a "Via" header.  If so, again we need to do a bit
	 * of processing.
	 */
	len = hashmap_search(hashofheaders, "via", (void **)&data);
	if (len > 0) {
		/* Take on our information */
		char hostname[128];
		gethostname(hostname, sizeof(hostname));

		write_message(connptr->server_fd,
			      "Via: %s, %hu.%hu %s (%s/%s)\r\n",
			      data,
			      connptr->protocol.major, connptr->protocol.minor,
			      hostname, PACKAGE, VERSION);

		hashmap_remove(hashofheaders, "via");
	} else {
		/* There is no header, so we need to create it. */
		char hostname[128];
		gethostname(hostname, sizeof(hostname));

		write_message(connptr->server_fd,
			      "Via: %hu.%hu %s (%s/%s)\r\n",
			      connptr->protocol.major, connptr->protocol.minor,
			      hostname, PACKAGE, VERSION);
	}

	/*
	 * Delete the headers listed in the skipheaders list
	 */
	for (i = 0; i < (sizeof(skipheaders) / sizeof(char *)); i++) {
		hashmap_remove(hashofheaders, skipheaders[i]);
	}

	/*
	 * Output all the remaining headers to the remote machine.
	 */
	listofheaders = hashmap_keys(hashofheaders);
	for (i = 0; i < vector_length(listofheaders); i++) {
		len = vector_getentry(listofheaders, i, (void **)&data);

		hashmap_search(hashofheaders, data, (void **)&header);

		if (!is_anonymous_enabled() || anonymous_search(data) == 0) {
			write_message(connptr->server_fd,
				      "%s: %s\r\n",
				      data, header);
		}
	}
	vector_delete(listofheaders);

	/* Free the hashofheaders since it's no longer needed */
	hashmap_delete(hashofheaders);

#if defined(XTINYPROXY_ENABLE)
	if (config.my_domain)
		add_xtinyproxy_header(connptr);
#endif
	
	/* Write the final "blank" line to signify the end of the headers */
	safe_write(connptr->server_fd, "\r\n", 2);

	/*
	 * Spin here pulling the data from the client.
	 */
	if (content_length >= 0)
		return pull_client_data(connptr,
					(unsigned long int) content_length);
	else
		return 0;
}

/*
 * Loop through all the headers (including the response code) from the
 * server.
 */
static int
process_server_headers(struct conn_s *connptr)
{
	char *header;
	ssize_t len;

	while (1) {
		if ((len = readline(connptr->server_fd, &header)) <= 0) {
			DEBUG2("Server (file descriptor %d) closed connection.",
			       connptr->server_fd);
			return -1;
		}

		if (safe_write(connptr->client_fd, header, len) < 0) {
			safefree(header);
			return -1;
		}

		if (CHECK_CRLF(header, len))
			break;

		safefree(header);
	}

	safefree(header);
	return 0;
}

/*
 * Switch the sockets into nonblocking mode and begin relaying the bytes
 * between the two connections. We continue to use the buffering code
 * since we want to be able to buffer a certain amount for slower
 * connections (as this was the reason why I originally modified
 * tinyproxy oh so long ago...)
 *	- rjkaes
 */
static void
relay_connection(struct conn_s *connptr)
{
	fd_set rset, wset;
	struct timeval tv;
	time_t last_access;
	int ret;
	double tdiff;
	int maxfd = max(connptr->client_fd, connptr->server_fd) + 1;

	socket_nonblocking(connptr->client_fd);
	socket_nonblocking(connptr->server_fd);

	last_access = time(NULL);

	for (;;) {
		FD_ZERO(&rset);
		FD_ZERO(&wset);

		tv.tv_sec =
		    config.idletimeout - difftime(time(NULL), last_access);
		tv.tv_usec = 0;

		if (BUFFER_SIZE(connptr->sbuffer) > 0)
			FD_SET(connptr->client_fd, &wset);
		if (BUFFER_SIZE(connptr->cbuffer) > 0)
			FD_SET(connptr->server_fd, &wset);
		if (BUFFER_SIZE(connptr->sbuffer) < MAXBUFFSIZE)
			FD_SET(connptr->server_fd, &rset);
		if (BUFFER_SIZE(connptr->cbuffer) < MAXBUFFSIZE)
			FD_SET(connptr->client_fd, &rset);

		ret = select(maxfd, &rset, &wset, NULL, &tv);

		if (ret == 0) {
			tdiff = difftime(time(NULL), last_access);
			if (tdiff > config.idletimeout) {
				log_message(LOG_INFO,
					    "Idle Timeout (after select) as %g > %u.",
					    tdiff, config.idletimeout);
				return;
			} else {
				continue;
			}
		} else if (ret < 0) {
			log_message(LOG_ERR,
				    "relay_connection: select() error \"%s\". Closing connection (client_fd:%d, server_fd:%d)",
				    strerror(errno), connptr->client_fd,
				    connptr->server_fd);
			return;
		} else {
			/*
			 * Okay, something was actually selected so mark it.
			 */
			last_access = time(NULL);
		}

		if (FD_ISSET(connptr->server_fd, &rset)
		    && read_buffer(connptr->server_fd, connptr->sbuffer) < 0) {
			break;
		}
		if (FD_ISSET(connptr->client_fd, &rset)
		    && read_buffer(connptr->client_fd, connptr->cbuffer) < 0) {
			break;
		}
		if (FD_ISSET(connptr->server_fd, &wset)
		    && write_buffer(connptr->server_fd, connptr->cbuffer) < 0) {
			break;
		}
		if (FD_ISSET(connptr->client_fd, &wset)
		    && write_buffer(connptr->client_fd, connptr->sbuffer) < 0) {
			break;
		}
	}

	/*
	 * Here the server has closed the connection... write the
	 * remainder to the client and then exit.
	 */
	socket_blocking(connptr->client_fd);
	while (BUFFER_SIZE(connptr->sbuffer) > 0) {
		if (write_buffer(connptr->client_fd, connptr->sbuffer) < 0)
			break;
	}

	/*
	 * Try to send any remaining data to the server if we can.
	 */
	socket_blocking(connptr->server_fd);
	while (BUFFER_SIZE(connptr->cbuffer) > 0) {
		if (write_buffer(connptr->client_fd, connptr->cbuffer) < 0)
			break;
	}

	return;
}

#ifdef UPSTREAM_SUPPORT
/*
 * Establish a connection to the upstream proxy server.
 */
static int
connect_to_upstream(struct conn_s *connptr, struct request_s *request)
{
	char *combined_string;
	int len;

	connptr->server_fd =
	    opensock(config.upstream_name, config.upstream_port);

	if (connptr->server_fd < 0) {
		log_message(LOG_WARNING,
			    "Could not connect to upstream proxy.");
		httperr(connptr, 404, "Unable to connect to upstream proxy.");
		return -1;
	}

	log_message(LOG_CONN,
		    "Established connection to upstream proxy \"%s\" using file descriptor %d.",
		    config.upstream_name, connptr->server_fd);

	/*
	 * We need to re-write the "path" part of the request so that we
	 * can reuse the establish_http_connection() function. It expects a
	 * method and path.
	 */
	if (connptr->connect_method) {
		len = strlen(request->host) + 6;

		combined_string = safemalloc(len + 1);
		if (!combined_string) {
			return -1;
		}

		snprintf(combined_string, len, "%s:%d", request->host,
			 request->port);
	} else {
		len = strlen(request->host) + strlen(request->path) + 14;
		combined_string = safemalloc(len + 1);
		if (!combined_string) {
			return -1;
		}

		snprintf(combined_string, len, "http://%s:%d%s", request->host,
			 request->port, request->path);
	}

	safefree(request->path);
	request->path = combined_string;

	return establish_http_connection(connptr, request);
}
#endif

#ifdef TUNNEL_SUPPORT
/*
 * If tunnel has been configured then redirect any connections to it.
 */
static int
connect_to_tunnel(struct conn_s *connptr)
{
	char *request_buf;
	size_t len;
	int pos;

	request_buf = safemalloc(HTTP_LINE_LENGTH);
	if (request_buf) {
		len = recv(connptr->client_fd, request_buf, HTTP_LINE_LENGTH - 1, MSG_PEEK);
		for (pos = 0; pos < len && request_buf[pos] != '\n'; pos++)
			;
		request_buf[pos] = '\0';
	     
		log_message(LOG_CONN, "Request: %s", request_buf);

		safefree(request_buf);
	}
	log_message(LOG_INFO, "Redirecting to %s:%d",
		    config.tunnel_name, config.tunnel_port);

	connptr->server_fd =
		opensock(config.tunnel_name, config.tunnel_port);

	if (connptr->server_fd < 0) {
		log_message(LOG_WARNING,
			    "Could not connect to tunnel.");
		httperr(connptr, 404, "Unable to connect to tunnel.");

		return -1;
	}

	log_message(LOG_INFO,
		    "Established a connection to the tunnel \"%s\" using file descriptor %d.",
		    config.tunnel_name, connptr->server_fd);

	return 0;
}
#endif

/*
 * This is the main drive for each connection. As you can tell, for the
 * first few steps we are using a blocking socket. If you remember the
 * older tinyproxy code, this use to be a very confusing state machine.
 * Well, no more! :) The sockets are only switched into nonblocking mode
 * when we start the relay portion. This makes most of the original
 * tinyproxy code, which was confusing, redundant. Hail progress.
 * 	- rjkaes
 */
void
handle_connection(int fd)
{
	struct conn_s *connptr;
	struct request_s *request = NULL;

	char peer_ipaddr[PEER_IP_LENGTH];
	char peer_string[PEER_STRING_LENGTH];

	log_message(LOG_CONN, "Connect (file descriptor %d): %s [%s]",
		    fd,
		    getpeer_string(fd, peer_string),
		    getpeer_ip(fd, peer_ipaddr));

	connptr = initialize_conn(fd);
	if (!connptr)
		return;

	if (check_acl(fd) <= 0) {
		update_stats(STAT_DENIED);
		httperr(connptr, 403,
			"You do not have authorization for using this service.");
		goto send_error;
	}

	if (TUNNEL_CONFIGURED()) {
		if (connect_to_tunnel(connptr) < 0)
			goto internal_proxy;
		else
			goto relay_proxy;
	}

      internal_proxy:
	if (read_request_line(connptr) < 0) {
		update_stats(STAT_BADCONN);
		destroy_conn(connptr);
		return;
	}

	request = process_request(connptr);
	if (!request) {
		if (!connptr->response_message_sent) {
			update_stats(STAT_BADCONN);
			destroy_conn(connptr);
			return;
		}
		goto send_error;
	}

	if (UPSTREAM_CONFIGURED()) {
		if (connect_to_upstream(connptr, request) < 0)
			goto send_error;
	} else {
		connptr->server_fd = opensock(request->host, request->port);
		if (connptr->server_fd < 0) {
			httperr(connptr, 500, HTTP500ERROR);
			goto send_error;
		}

		log_message(LOG_CONN,
			    "Established connection to host \"%s\" using file descriptor %d.",
			    request->host, connptr->server_fd);

		if (!connptr->connect_method)
			establish_http_connection(connptr, request);
	}

      send_error:
	free_request_struct(request);

	if (process_client_headers(connptr) < 0) {
		update_stats(STAT_BADCONN);
		if (!connptr->response_message_sent) {
			destroy_conn(connptr);
			return;
		}
	}

	if (connptr->response_message_sent) {
		destroy_conn(connptr);
		return;
	}

	if (!connptr->connect_method || UPSTREAM_CONFIGURED()) {
		if (process_server_headers(connptr) < 0) {
			update_stats(STAT_BADCONN);
			destroy_conn(connptr);
			return;
		}
	} else {
		if (send_ssl_response(connptr) < 0) {
			log_message(LOG_ERR,
				    "handle_connection: Could not send SSL greeting to client.");
			update_stats(STAT_BADCONN);
			destroy_conn(connptr);
			return;
		}
	}

      relay_proxy:
	relay_connection(connptr);

	/*
	 * All done... close everything and go home... :)
	 */
	destroy_conn(connptr);
	return;
}
