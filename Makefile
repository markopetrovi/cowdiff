CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
PREFIX  ?= /usr/local

SRC = src/main.c src/extmap.c src/anchor.c src/delta.c src/linediff.c src/output.c
OBJ = $(SRC:.c=.o)

all: cowdiff tests/probe

cowdiff: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

$(OBJ): src/cowdiff.h

tests/probe: tests/probe.c
	$(CC) $(CFLAGS) -o $@ $<

check: all
	./tests/run.sh

clean:
	rm -f $(OBJ) cowdiff tests/probe

install: cowdiff
	install -Dm755 cowdiff $(DESTDIR)$(PREFIX)/bin/cowdiff

.PHONY: all check clean install
