#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <cassert>

#include "pmap.h"

void parsePmapOutput(std::vector<MemoryRegion> &regions, size_t &totalRSS, bool filter) {
    std::string line;
    totalRSS = 0;

    // Pin will map shared memory regions into a separate rw--- region right after the shared mapping
    // we need to ignore these regions
    bool skip_shared_mem = false;

    // Skip the first header line
    std::getline(std::cin, line);
    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        uint64_t address;
        size_t size, rss, dirty;
        std::string permissions;
        std::string mapping;

        // Skip malformed lines
        if (!(iss >> std::hex >> address >> std::dec >> size >> rss >> dirty >> permissions >> mapping)) {
            continue; 
        }

        // Filter checks
        if (filter) {
            // Skip the shared memory regions
            if (skip_shared_mem) {
                skip_shared_mem = false;
                continue;
            }

            // Check for shared memory label
            if (mapping.find("shared_mem") != std::string::npos) {
                skip_shared_mem = true;
                continue;
            }

            // Skip memory not marked as RW, or containing certain strings in the mapping
            if (permissions.compare("rw---") != 0 ||
                    mapping.find("pitracer") != std::string::npos ||
                    mapping.find("pin") != std::string::npos) {
                continue;
            }
        }
        

        // Size and RSS are in KB, convert to bytes
        size <<= 10;
        rss <<= 10;
        totalRSS += rss;

        // Track RW regions about certain RSS only (10MB)
        if (rss < (10 << 20) && filter) {
            continue;
        }
        regions.emplace_back(address, size, rss);
    }
}

std::vector<MemoryRegion> findLargestRegions(const std::vector<MemoryRegion> &regions, size_t totalRSS, float coverage, u64 max_regions) {
    std::vector<MemoryRegion> sortedRegions = regions;
    std::sort(sortedRegions.begin(), sortedRegions.end(), [](const MemoryRegion &a, const MemoryRegion &b) {
        return a.rss > b.rss; // Sort in descending order of RSS
    });

    std::vector<MemoryRegion> result;
    size_t cumulativeRSS = 0;

    for (const auto &region : sortedRegions) {
        result.push_back(region);
        cumulativeRSS += region.rss;
        if (cumulativeRSS >= totalRSS * coverage || result.size() >= max_regions) {
            break; // Stop once we reach 90% of the total RSS or the maximum number of regions
        }
    }

    return result;
}