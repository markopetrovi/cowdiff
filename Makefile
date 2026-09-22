CC      ?= cc
# -Werror so that a warning is a broken build rather than a line of output
# nobody reads; override CFLAGS to get a build without it (the sanitizer and
# -O0 builds do).
CFLAGS  ?= -O3 -Wall -Wextra -Werror -std=gnu11 -march=native -mtune=native
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

# `clean` is run routinely between builds, so it leaves the scratch the tests
# and benchmarks generate alone: .testtmp/ is rebuilt by every test run anyway,
# but .bench/ is half a gigabyte and takes a rebuild cycle to reproduce, and
# deleting it out from under a measurement is the kind of surprise that stops
# people typing `make clean` at all.  This is the target that gives the space
# back.
distclean: clean
	rm -rf .testtmp .bench

install: cowdiff
	install -Dm755 cowdiff $(DESTDIR)$(PREFIX)/bin/cowdiff

.PHONY: all check clean distclean install
