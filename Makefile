CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2

.PHONY: all clean test

all: xcheck

xcheck: xcheck.c
	$(CC) $(CFLAGS) $< -o $@

test: xcheck
	python3 tests/test_xcheck.py

clean:
	rm -f xcheck
