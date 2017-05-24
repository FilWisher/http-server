#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <assert.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include <signal.h>

#include "picohttpparser.h"
#include "http.h"

#define PORT_NO (8080)
#define SRV_ROOT ("/home/william")
#define LOG_PATH ("/dev/stderr")
#define NWORKERS (4)

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

	char name[64];
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

int
request_status(struct request *req, HTTP_STATUS status)
{
	char status_line[256];
	size_t n;
	// currently assumes http/1.1
	n = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %s\r\n", http_status_string[status]);
	return request_write(req, status_line, n);
}

void
transfer_file(struct request *req, int fd)
{
	ssize_t n;
	char buf[1024];

	char content_type[] = "Content-Type: text/html\r\n\r\n";

	if (request_status(req, HTTP_200) == -1)
		return;

	if (request_write(req, content_type, sizeof(content_type)) == -1)
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
	struct server *srv = req->cli.srv;

	char buf[4096];
	size_t buflen = 0, prevbuflen = 0;
	int ret;
	unsigned i;

	if (what & EV_TIMEOUT) {
		printf("%s: request timed out\n", srv->name);
		request_close(req);
		close(fd);
	}
	printf("%s starting read\n", srv->name);

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
	struct server *srv = arg;
	printf("%s accepting\n", srv->name);
	struct request *req;
	socklen_t len = sizeof(struct sockaddr_in);
	struct timeval tv;
	tv.tv_sec = 5; // timeout in seconds

	if ((req = malloc(sizeof(*req))) == NULL)
		return;

	// if ((req->cli.fd = accept4(fd, (struct sockaddr *)&req->cli.addr, &len, SOCK_NONBLOCK)) == -1) {
	if ((req->cli.fd = accept(fd, (struct sockaddr *)&req->cli.addr, &len)) == -1) {
		if (errno == EWOULDBLOCK || errno == EAGAIN)
			printf("%s didn't get it\n", srv->name);
		return;
	}
	
	req->cli.srv = arg;

	printf("%s setting read event\n", srv->name);
	event_set(&req->cli.ev, req->cli.fd, EV_WRITE|EV_PERSIST, client_read, req);
	if (event_add(&req->cli.ev, &tv) == -1)
		printf("error adding\n");
}

pid_t children[NWORKERS];

void
signal_handler(int sig, short event, void *arg)
{
	int i;
	pid_t pid;
	printf("shutting down\n");
	for (i = 0; i < NWORKERS; i++) {
		pid = children[i];
		printf("killing %d\n", (int)pid);
		if (pid != -1)
			kill(pid, SIGKILL);
	}
	printf("goodbye\n");
	exit(0);
}


int
main()
{
	int fd;
	struct sockaddr_in addr;
	struct server srv;
	int i;
	pid_t pid;

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


	for (i = 0; i < NWORKERS; i++) {
		pid = fork();
		if (pid == 0) {
			snprintf(srv.name, sizeof(srv.name), "worker(%d)", i);

			event_init();
			event_set(&srv.ev, fd, EV_READ | EV_PERSIST, server_accept, &srv);
			event_add(&srv.ev, 0);
			break;
		} else {
			printf("adding %d\n", pid);
			children[i] = pid;
			// close(fd);
		}
	}

	if (pid != 0) {
	    struct event sigint;
	    struct event sigterm;
	    struct event sighup;

	    event_init();
	    signal_set(&sigint, SIGINT, signal_handler, NULL);
	    signal_set(&sigterm, SIGTERM, signal_handler, NULL);
	    signal_set(&sighup, SIGHUP, signal_handler, NULL);

	    signal_add(&sigint, NULL);
	    signal_add(&sigterm, NULL);
	    signal_add(&sighup, NULL);

	    snprintf(srv.name, sizeof(srv.name), "master");
	}

	printf("%s dispatching\n", srv.name);
	event_dispatch();
}
