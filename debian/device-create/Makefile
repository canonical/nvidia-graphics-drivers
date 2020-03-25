#!/usr/bin/make -f

PROGRAM = ub-device-create
PROGRAM_FILES = ub-device-create.c
CC = gcc
CFLAGS =-g -Wall $(shell pkg-config --cflags --libs pciaccess libkmod)

all: build

build:
	$(CC) -o $(PROGRAM) $(PROGRAM_FILES) $(CFLAGS)

clean:
	@rm -f $(PROGRAM)
