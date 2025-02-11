CXX = g++
CC = gcc
CXXFLAGS = -Wall -O3
CFLAGS = -Wall -O3

all: pagemap_dump

pagemap_dump: src/pagemap_dump.c src/top_rss.cpp src/pmap_main.cpp src/pmap.h
	$(CXX) $(CFLAGS) -o dump_pagemap src/pagemap_dump.c src/top_rss.cpp src/pmap_main.cpp

clean:
	rm -f check_contiguity dump_pagemap