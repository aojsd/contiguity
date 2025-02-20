#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>

#include "pmap.h"

void parsePmapOutput(std::vector<MemoryRegion> &regions, size_t &totalRSS) {
    std::string line;
    totalRSS = 0;

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
        // Skip memory not marked as RW, or containing certain strings in the mapping
        else if (permissions.compare("rw---") != 0 ||
                 mapping.find("pitracer") != std::string::npos ||
                 mapping.find("pin") != std::string::npos) {
            continue;
        }

        // Size and RSS are in KB, convert to bytes
        size <<= 10;
        rss <<= 10;
        totalRSS += rss;

        // Track RW regions about certain RSS only (10MB)
        if (rss < (10 << 20)) {
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