CC=gcc
CFLAGS=-Wall -Wextra -pedantic -std=c99
LDFLAGS=-levent

default: server

server: server.c httpparser.c
	$(CC) $(CFLAGS) $(LDFLAGS) httpparser.c server.c -o server

clean:
	@ rm -rf server

.PHONY: clean
