CXX = g++
CC = gcc
CXXFLAGS = -Wall -O3 -std=c++17
CFLAGS = -Wall -O3
PMAP_DIR=src/pagemap_dump

all: pagemap_dump memcached_requests sync_microbench

pagemap_dump: $(PMAP_DIR)/pagemap_dump.c $(PMAP_DIR)/top_rss.cpp $(PMAP_DIR)/pow2_regions.cpp $(PMAP_DIR)/pmap_main.cpp $(PMAP_DIR)/pmap.h
	$(CXX) $(CFLAGS) -o bin/dump_pagemap $(PMAP_DIR)/pagemap_dump.c $(PMAP_DIR)/top_rss.cpp $(PMAP_DIR)/pow2_regions.cpp $(PMAP_DIR)/pmap_main.cpp $(PMAP_DIR)/pmap.h

memcached_requests: src/memcached_requests.cpp
	$(CXX) $(CXXFLAGS) -o bin/memcached_requests src/memcached_requests.cpp

sync_microbench: src/sync_microbenchmark.cpp
	$(CXX) $(CXXFLAGS) -o bin/sync_microbench src/sync_microbenchmark.cpp

test: src/pow2_regions.cpp src/pmap.h src/test.cpp
	$(CXX) $(CXXFLAGS) -o src/test src/pow2_regions.cpp src/test.cpp

clean:
	rm -f src/test bin/*