CC      ?= gcc
CFLAGS  ?= -O3 -march=native -pthread -Wall -Wextra -Werror
PKGS    := libdrm freetype2
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  := $(shell pkg-config --libs $(PKGS)) -ljpeg -lm

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := cellavg

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN) tests/test_delaunay

tests/test_delaunay: tests/test_delaunay.c src/delaunay.c src/delaunay.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_delaunay.c src/delaunay.c -lm

test: tests/test_delaunay
	./tests/test_delaunay
