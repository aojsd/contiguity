CXX = g++
CC = gcc
CXXFLAGS = -Wall -O3
CFLAGS = -Wall -O3

all: pagemap_dump

pagemap_dump: src/pagemap_dump.c src/top_rss.cpp src/pow2_regions.cpp src/pmap_main.cpp src/pmap.h
	$(CXX) $(CFLAGS) -o dump_pagemap src/pagemap_dump.c src/top_rss.cpp src/pmap_main.cpp src/pow2_regions.cpp

test: src/pow2_regions.cpp src/pmap.h src/test.cpp
	$(CXX) $(CXXFLAGS) -o src/test src/pow2_regions.cpp src/test.cpp

clean:
	rm -f src/test dump_pagemap