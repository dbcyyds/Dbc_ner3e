CXX ?= aarch64-linux-android-clang++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -static-libstdc++

.PHONY: all install clean test

all: vocdump

vocdump: vocdump.cpp
	$(CXX) $(CXXFLAGS) -o $@ vocdump.cpp

test: vocdump
	./vocdump --selftest

install: vocdump
	su -c 'install -m 755 vocdump /data/local/tmp/nr3e-voc'

clean:
	rm -f vocdump
