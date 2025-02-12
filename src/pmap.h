#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>

typedef struct {
    uint64_t pfn : 55;
    unsigned int soft_dirty : 1;
    unsigned int file_page : 1;
    unsigned int swapped : 1;
    unsigned int present : 1;
} PagemapEntry;
int pagemap_get_entry(PagemapEntry *entry, int pagemap_fd, uintptr_t vaddr);
int virt_to_phys_user(uintptr_t *paddr, pid_t pid, uintptr_t vaddr);
int parse_all(int argc, char **argv);


extern float coverage;
struct MemoryRegion {
    uint64_t address;
    size_t size;
    size_t rss;

    MemoryRegion(uint64_t addr, size_t sz, size_t rs) : address(addr), size(sz), rss(rs) {}
};
void parsePmapOutput(std::vector<MemoryRegion> &regions, size_t &totalRSS);
std::vector<MemoryRegion> findLargestRegions(const std::vector<MemoryRegion> &regions, size_t totalRSS);