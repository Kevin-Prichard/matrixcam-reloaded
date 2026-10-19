CC      ?= gcc
CFLAGS  ?= -O3 -march=native -pthread -Wall -Wextra -Werror
PKGS    := libdrm freetype2
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  := $(shell pkg-config --libs $(PKGS)) -ljpeg

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := cellavg

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)
