CC=gcc
CFLAGS=-Wall -Wextra -pedantic -std=c99
LDFLAGS=-levent

default: server

server: server.c picohttpparser.c
	$(CC) $(CFLAGS) $(LDFLAGS) picohttpparser.c server.c -o server

clean:
	@ rm -rf server

.PHONY: clean
