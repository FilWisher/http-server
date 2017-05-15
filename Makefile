CC=gcc
CFLAGS=-Wall -Wextra -pedantic -std=c99 -g
LDFLAGS=-levent

default: server file

server: server.c
	$(CC) $(CFLAGS) $(LDFLAGS) server.c -o server

file: file.c httpparser.c
	$(CC) $(CFLAGS) $(LDFLAGS) httpparser.c file.c -o file

clean:
	@ rm -rf server

.PHONY: clean
