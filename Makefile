CC      ?= cc
CFLAGS  ?= -O3 -Wall -Wextra -std=gnu11 -march=native -mtune=native
PREFIX  ?= /usr/local

SRC = src/main.c src/extmap.c src/anchor.c src/delta.c src/linediff.c \
      src/output.c src/walk.c
OBJ = $(SRC:.c=.o)

all: cowdiff tests/probe

cowdiff: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

$(OBJ): src/cowdiff.h

tests/probe: tests/probe.c
	$(CC) $(CFLAGS) -o $@ $<

check: all
	./tests/run.sh
	python3 tests/extentcheck.py ./cowdiff ./tests/probe

clean:
	rm -f $(OBJ) cowdiff tests/probe

install: cowdiff
	install -Dm755 cowdiff $(DESTDIR)$(PREFIX)/bin/cowdiff

.PHONY: all check clean install
