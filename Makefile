CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wconversion -Og -g $(shell pkg-config --cflags libbluray)
LIBS := $(shell pkg-config --libs libbluray)


bdtool: bdtool.c strbuf.h Makefile
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)
