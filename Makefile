CC      ?= gcc
CFLAGS  ?= -O3 -march=native -pthread -Wall -Wextra -Werror
PKGS    := libdrm freetype2
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  := $(shell pkg-config --libs $(PKGS)) -ljpeg -lm
CXX     ?= g++
CXXFLAGS ?= -O3 -march=native -pthread -Wall -Wextra -Werror
CXXFLAGS += -std=c++11 $(shell pkg-config --cflags $(PKGS))
FAST_SLIC_DIR := candidate-dependencies/fast-slic
FAST_SLIC_SRC := $(addprefix $(FAST_SLIC_DIR)/src/,timer.cpp parallel.cpp fast-slic.cpp cca.cpp context.cpp context-impl.cpp lsc.cpp lsc-builder.cpp)
FAST_SLIC_ARCH_FLAGS := $(shell printf '' | $(CXX) $(CXXFLAGS) -dM -E -x c++ - 2>/dev/null | \
	awk '$$2 == "__AVX2__" { print "-DUSE_AVX2" } \
	$$2 == "__ARM_NEON" || $$2 == "__ARM_NEON__" { print "-DUSE_NEON" }')
CPPFLAGS += -Isrc -isystem $(FAST_SLIC_DIR)/src
CXXFLAGS += $(FAST_SLIC_ARCH_FLAGS)

ifeq ($(DEBUG),1)
CFLAGS := $(filter-out -O%,$(CFLAGS)) -O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined
CXXFLAGS := $(filter-out -O%,$(CXXFLAGS)) -O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined
endif

SRC := $(wildcard src/*.c)
CPP_SRC := src/slic_analyzer.cpp $(FAST_SLIC_SRC)
OBJ := $(SRC:.c=.o) $(CPP_SRC:.cpp=.o)
BIN := cellavg

.PHONY: all clean test debug

all: $(BIN)

debug:
	$(MAKE) clean
	$(MAKE) DEBUG=1 all

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/%.o: src/%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

src/slic_analyzer.o: src/slic_analyzer.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Wno-unknown-pragmas -Wno-unused-variable -Wno-unused-function -c -o $@ $<

$(FAST_SLIC_DIR)/src/%.o: $(FAST_SLIC_DIR)/src/%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -w -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN) tests/test_delaunay tests/test_slic

tests/test_delaunay: tests/test_delaunay.c src/delaunay.c src/delaunay.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_delaunay.c src/delaunay.c -lm

tests/test_slic: tests/test_slic.cpp src/slic_analyzer.o src/slic_visualizer.o src/config.o src/camera_controls.o $(FAST_SLIC_SRC:.cpp=.o)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ -ljpeg -lm

test: tests/test_delaunay tests/test_slic
	./tests/test_delaunay
	./tests/test_slic
