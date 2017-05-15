#include <stdio.h>
#define _GNU_SOURCE
#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>
#include <strings.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/limits.h>

#include <assert.h>
#include <event.h>

#include "picohttpparser.h"

#define PORT_NO (8081)

struct server {
	char root[PATH_MAX];
	struct event ev;
};

struct client {
  int fd;
  struct sockaddr_in addr;
  struct bufferevent *bev;
  struct server *srv;
  struct event ev;
};

void
client_close(struct client *cli)
{
  event_del(&cli->ev);
  close(cli->fd);
  free(cli);
}

void
server_setroot(struct server *srv, char *path)
{
	snprintf(srv->root, sizeof(srv->root), "%s", path);
}

int
respond(struct client *cli, char *buf, size_t len)
{
  ssize_t n;
  if ((n = write(cli->fd, buf, len)) == -1) {
    client_close(cli);
    free(cli);
    return -1;
  }
  return n;
}

// TODO: don't use bufferevent to write back to client
// just write using fd and event_add/event_del
// (we don't want it buffering)
void
transfer_file(struct client *cli, int fd)
{
  ssize_t n;
  char buf[1024];
  char *errormsg = "500: SERVER ERROR\n";

  printf("transfering file\n");
  while (1) {
    n = read(fd, buf, sizeof(buf));
    if (n == -1 && respond(cli, errormsg, sizeof(errormsg)))
      break;
    else if (n == 0)
      break;

    if (respond(cli, buf, n) == -1)
      break;
  }
}

#define MINIMUM(a, b) (a < b ? a : b)

void
send_file(struct client *cli, const char *filepath, size_t len)
{
  int fd;
  char errormsg[1024];
  size_t n;
  char path[PATH_MAX];

  snprintf(path, sizeof(path), "%s%.*s", cli->srv->root, (int)len, filepath);
  printf("path: %s\n", path);

  if ((fd = open(path, O_RDONLY)) == -1) {
    n = snprintf(errormsg, sizeof(errormsg), "404: %s NOTFOUND\n", path);
    write(cli->fd, errormsg, MINIMUM(n, sizeof(errormsg)));
    return;
  }

  transfer_file(cli, fd);
}

void
client_read(int fd, short what, void *arg)
{
  // size_t n;
  // char buf[PATH_MAX];
  // char path[PATH_MAX];
  // struct client *cli = arg;

  // n = bufferevent_read(bev, buf, sizeof(buf));
  // if (n <= 0)
  //    return;

  // buf[n-1] = '\0';
  // printf("root: %s\n", cli->srv->root);
  // printf("buf: %s\n", buf);
  // snprintf(path, PATH_MAX, "%s/%s", cli->srv->root, buf);
  // printf("path: %s\n", path);


  struct client *cli = arg;
  char buf[4096];
  const char *method;
  const char *path;
  size_t buflen = 0, prevbuflen = 0, methodlen, pathlen, nheaders;
  int ret, minversion;
  unsigned i;

  struct phr_header headers[100];

  while (1) {
    while ((ret = read(fd, buf + buflen, sizeof(buf) - buflen)) == -1 && errno == EINTR)
      ; /* empty */
    if (ret <= 0) {
      return;
    }
    prevbuflen = buflen;
    buflen += ret;

    nheaders = sizeof(headers) / sizeof(headers[0]);
    ret = phr_parse_request(buf, buflen, &method, &methodlen, &path, &pathlen, &minversion,
			    headers, &nheaders, prevbuflen);
    if (ret > 0)
      break; /* done */
    else if (ret == -1)
      return;

    assert(ret == -2);
    if (buflen == sizeof(buf))
      return;

  }


  printf("request is %d bytes long\n", ret);
  printf("method is %.*s\n", (int)methodlen, method);
  printf("path is %.*s\n", (int)pathlen, path);
  printf("HTTP version is 1.%d\n", minversion);
  printf("headers:\n");
  for (i = 0; i != nheaders; ++i) {
    printf("%.*s: %.*s\n", (int)headers[i].name_len, headers[i].name,
	   (int)headers[i].value_len, headers[i].value);
  }
  send_file(cli, path, pathlen);

  client_close(cli);
  close(fd);
}

void
client_write(int fd, short what, void *arg)
{
  printf("WRITE!\n");
}

void
server_accept(int fd, short what, void *arg)
{
  struct client *cli;
  socklen_t len = sizeof(struct sockaddr_in);

  if ((cli = malloc(sizeof(*cli))) == NULL)
    return;

  if ((cli->fd = accept4(fd, (struct sockaddr *)&cli->addr, &len, SOCK_NONBLOCK)) == -1)
    return;
	
  cli->srv = arg;

  event_set(&cli->ev, cli->fd, EV_READ|EV_PERSIST, client_write, cli);
  event_set(&cli->ev, cli->fd, EV_WRITE|EV_PERSIST, client_read, cli);
  if (event_add(&cli->ev, NULL) == -1)
    printf("error adding\n");
}

int
main()
{
	int fd;
	struct sockaddr_in addr;
	struct server srv;
	
	server_setroot(&srv, "/home/william");

	if ((fd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) == -1)
		perror("socket");
		
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT_NO);
	addr.sin_addr.s_addr = INADDR_ANY;
	
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
		perror("bind");
		
	if (listen(fd, 5) == -1)
		perror("listen");


	event_init();

	event_set(&srv.ev, fd, EV_READ | EV_PERSIST, server_accept, &srv);
	event_add(&srv.ev, 0);

	event_dispatch();
}
