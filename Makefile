
all: build/sync_clipboards
.PHONY: all

CXX=g++
CFLAGS=-Wall -Wextra -Werror
CXXFLAGS=$(CFLAGS) -std=c++17
LIBS=-lfmt -lxcb-xfixes -lxcb

build/sync_clipboards: build/main.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

build/%.o: src/%.cpp build/stamp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/stamp:
	mkdir -p build
	touch $@
