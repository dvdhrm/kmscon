#
# kmscon - Makefile
# Written 2011 by David Herrmann <dh.herrmann@googlemail.com>
#

#
# To compile the kmscon application, use:
#   $ make
# To compile the test suites, use:
#   $ make tests
#
# This makefile is in no way complete nor sophisticated. If you have time to
# replace it with autotools, I would be glad to apply your patches.
#

CFLAGS=-g -O0 -Wall `pkg-config --cflags --libs egl gbm gl cairo` -Isrc

all:
	gcc -o kmscon src/*.c $(CFLAGS)

tests:
	gcc -o test_output tests/test_output.c src/output.c $(CFLAGS)

clean:
	@rm -vf kmscon test_output

.PHONY: all tests clean
