#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <assert.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include "picohttpparser.h"

#define PORT_NO (8081)
#define SRV_ROOT ("/var/www/html")
#define LOG_PATH ("/dev/stderr")

#define MINIMUM(a, b) (a < b ? a : b)

// TODO: parse config with yacc
// TODO: use worker processes to distribute workload
// TODO: dispatch on filepath
// TODO: configure for TLS
// TODO: set response headers and serialize automatically

struct server {
	struct event ev;

	int port;
	char root[PATH_MAX];

	FILE *log_file;
	char log_path[PATH_MAX];
};

struct client {
	int fd;
	struct sockaddr_in addr;
	struct bufferevent *bev;
	struct server *srv;
	struct event ev;
};

struct request {
	struct client cli;

	size_t methodlen;
	const char *method;

	size_t pathlen;
	const char *path;

	int minor_version;

	size_t nheaders;
	struct phr_header headers[100];
};

void
server_log(struct server *srv, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	vfprintf(srv->log_file, fmt, ap);
	fflush(stderr);
	
	va_end(ap);
}


void
request_close(struct request *req)
{
	event_del(&req->cli.ev);
	close(req->cli.fd);
	free(req);
}

int
request_write(struct request *req, char *buf, size_t len)
{
	ssize_t n;
	printf("responding: %s (%d)\n", buf, (int)len);
	if ((n = write(req->cli.fd, buf, len)) == -1) {
		request_close(req);
		return -1;
	}
	return n;
}

void
transfer_file(struct request *req, int fd)
{
	ssize_t n;
	char buf[1024];

	char status_ok[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";

	if (request_write(req, status_ok, sizeof(status_ok)) == -1)
		return;

	while (1) {
		n = read(fd, buf, sizeof(buf));
		if (n == -1)
		  break;
		else if (n == 0) {
		  close(fd);
		  break;
		} else
		    request_write(req, buf, n);
	}
}

void
send_file(struct request *req, const char *filepath, size_t len)
{
	int fd;
	char errormsg[1024];
	size_t n;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s%.*s", req->cli.srv->root, (int)len, filepath);

	if ((fd = open(path, O_RDONLY)) == -1) {
		n = snprintf(errormsg, sizeof(errormsg), "404: %s NOTFOUND\n", path);
		write(req->cli.fd, errormsg, MINIMUM(n, sizeof(errormsg)));
		return;
	}

	transfer_file(req, fd);
}

void
client_read(int fd, short what, void *arg)
{
	struct request *req = arg;

	char buf[4096];
	size_t buflen = 0, prevbuflen = 0;
	int ret;
	unsigned i;

	while (1) {
		while ((ret = read(fd, buf + buflen, sizeof(buf) - buflen)) == -1 && errno == EINTR)
			; /* empty */
		if (ret <= 0) {
			return;
		}
		prevbuflen = buflen;
		buflen += ret;

		req->nheaders = sizeof(req->headers) / sizeof(req->headers[0]);
		ret = phr_parse_request(buf, buflen,
					&req->method, &req->methodlen,
					&req->path, &req->pathlen,
					&req->minor_version,
					req->headers, &req->nheaders, prevbuflen);
		if (ret > 0)
			break; /* done */
		else if (ret == -1)
			return;

		assert(ret == -2);
		if (buflen == sizeof(buf))
			return;

	}

	server_log(req->cli.srv, "request is %d bytes long \n", ret);
	server_log(req->cli.srv, "method is is %.*s\n",
		   (int)req->methodlen, req->method);
	server_log(req->cli.srv, "path is %.*s\n",
		   (int)req->pathlen, req->path);
	server_log(req->cli.srv, "HTTP version is 1.%d\n",
		   (int)req->pathlen, req->path);
	for (i = 0; i != req->nheaders; ++i)
		server_log(req->cli.srv, "%.*s: %.*s\n",
			   (int)req->headers[i].name_len,
			   req->headers[i].name,
			    (int)req->headers[i].value_len,
			   req->headers[i].value); 
	send_file(req, req->path, req->pathlen);

	free(req);
	close(fd);
}

void
server_accept(int fd, short what, void *arg)
{
	struct request *req;
	socklen_t len = sizeof(struct sockaddr_in);

	if ((req = malloc(sizeof(*req))) == NULL)
		return;

	if ((req->cli.fd = accept4(fd, (struct sockaddr *)&req->cli.addr, &len, SOCK_NONBLOCK)) == -1)
		return;
	
	req->cli.srv = arg;

	event_set(&req->cli.ev, req->cli.fd, EV_WRITE|EV_PERSIST, client_read, req);
	if (event_add(&req->cli.ev, NULL) == -1)
		printf("error adding\n");
}

int
main()
{
	int fd;
	struct sockaddr_in addr;
	struct server srv;

	// set server root and log_path
	snprintf(srv.root, sizeof(srv.root), "%s", SRV_ROOT);
	snprintf(srv.log_path, PATH_MAX, "%s", LOG_PATH);

	if ((srv.log_file = fopen(srv.log_path, "a")) == NULL) {
		perror("fopen logfile");
		return 1;
	}

	if ((fd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) == -1) {
		perror("socket");
		return 1;
	}
		
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT_NO);
	addr.sin_addr.s_addr = INADDR_ANY;
	
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("bind");
		return 1;
	}
		
	if (listen(fd, 5) == -1) {
		perror("listen");
		return 1;
	}

	event_init();

	event_set(&srv.ev, fd, EV_READ | EV_PERSIST, server_accept, &srv);
	event_add(&srv.ev, 0);

	event_dispatch();
}
