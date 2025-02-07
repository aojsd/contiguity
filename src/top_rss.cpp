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
        std::string address;
        size_t size, rss, dirty;
        std::string permissions;
        std::string mapping;

        if (!(iss >> address >> size >> rss >> dirty >> permissions >> mapping)) {
            continue; // Skip malformed lines
        }
        else if (permissions.compare("rw---") != 0) {
            continue; // Skip special regions
        }

        // Size and RSS are in KB, convert to bytes
        size <<= 10;
        rss <<= 10;

        regions.emplace_back(address, size, rss);
        totalRSS += rss;
    }
}

std::vector<MemoryRegion> findLargestRegions(const std::vector<MemoryRegion> &regions, size_t totalRSS) {
    std::vector<MemoryRegion> sortedRegions = regions;
    std::sort(sortedRegions.begin(), sortedRegions.end(), [](const MemoryRegion &a, const MemoryRegion &b) {
        return a.rss > b.rss; // Sort in descending order of RSS
    });

    std::vector<MemoryRegion> result;
    size_t cumulativeRSS = 0;

    for (const auto &region : sortedRegions) {
        result.push_back(region);
        cumulativeRSS += region.rss;
        if (cumulativeRSS >= totalRSS * coverage) {
            break; // Stop once we reach 90% of the total RSS
        }
    }

    return result;
}