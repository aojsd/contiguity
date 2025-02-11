#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <cctype>
#include <algorithm>
#include <map>
#include <vector>
#include <tuple>
#include <fstream>
#include <cstdint>
#include "pmap.h"

using namespace std;

#define u64 unsigned long long

float coverage = 0.9;
map<u64, int> region_sizes;
vector<u64> region_starts_V;
vector<u64> region_lengths;
vector<u64> region_starts_P;

// Finds the largest memory regions of a process that consume at least 80% of the total RSS
// For each region, prints the mapping of every virtual page (VPN and PFN)
// Input:
// - pid: the process ID
// - stdin: the output of pmap -x <pid>
int main(int argc, char **argv)
{
    if (argc < 2) {
        cerr << "Usage: sudo "<< argv[0] << " <pid>\n";
        return EXIT_FAILURE;
    }

    // return parse_all(argc, argv);

    // Find regions
    vector<MemoryRegion> regions;
    size_t totalRSS;
    parsePmapOutput(regions, totalRSS);
    vector<MemoryRegion> largestRegions = findLargestRegions(regions, totalRSS);
    // cerr << "Regions (" << coverage * 100 << "% RSS):\t" << largestRegions.size() << endl;

    // Get mappings
    pid_t pid = stoul(argv[1]);
    size_t pageSize = sysconf(_SC_PAGE_SIZE);
    vector<tuple<uintptr_t, uintptr_t>> mappings;
    for (const auto &region : largestRegions) {
        for (size_t i = 0; i < region.size; i += pageSize) {
            uintptr_t vaddr = stoul(region.address, nullptr, 16) + i;
            uintptr_t paddr;
            if (virt_to_phys_user(&paddr, pid, vaddr)) {
                cerr << "Failed to convert virtual address to physical\n";
                return EXIT_FAILURE;
            }
            // cout << hex << vaddr << " " << paddr << endl;
            mappings.push_back({vaddr, paddr});
        }
    }

    //==================================================================================================
    // Extract region data
    //==================================================================================================
    u64 n_regions = 0;
    u64 last_VPN = 0;
    u64 last_PFN = 0;
    u64 region_size = 0;
    u64 total_pages = 0;
    u64 lowest_VPN = UINT64_MAX;
    for (const auto &mapping : mappings) {
        u64 VPN = get<0>(mapping) >> 12;
        u64 PFN = get<1>(mapping) >> 12;
        lowest_VPN = VPN < lowest_VPN ? VPN : lowest_VPN;

        // Check if mapping is valid
        if (PFN == 0) {
            if (region_size > 0) {
                if (region_sizes.find(region_size) == region_sizes.end()) {
                    region_sizes[region_size] = 1;
                }
                else {
                    region_sizes[region_size]++;
                }
                n_regions++;

                // Record region
                region_starts_V.push_back((last_VPN - region_size + 1) << 12);
                region_lengths.push_back(region_size);
                region_starts_P.push_back(last_PFN - region_size + 1);
                total_pages += region_size;

                // Reset region
                region_size = 0;
                last_VPN = 0;
                last_PFN = 0;
            }
        }
        // Continues contiguous region
        else {
            if (VPN == last_VPN + 1 && PFN == last_PFN + 1) {
                region_size++;
            }
            // New region started
            else {
                if (region_size > 0) {
                    if (region_sizes.find(region_size) == region_sizes.end()) {
                        region_sizes[region_size] = 1;
                    }
                    else {
                        region_sizes[region_size]++;
                    }
                    n_regions++;

                    // Record region
                    region_starts_V.push_back((last_VPN - region_size + 1) << 12);
                    region_lengths.push_back(region_size);
                    region_starts_P.push_back(last_PFN - region_size + 1);
                    total_pages += region_size;
                }
                region_size = 1;
            }
            last_VPN = VPN;
            last_PFN = PFN;
        }
    }

    //==================================================================================================
    // Get the number of regions needed to cover X% of RSS (starting with the largest regions)
    //==================================================================================================
    u64 r75 = 0, r50 = 0, r25 = 0;
    u64 sz75 = (total_pages >> 2) * 3;
    u64 sz50 = total_pages >> 1;
    u64 sz25 = total_pages >> 2;
    u64 cov = 0;
    for (auto it = region_sizes.rbegin(); it != region_sizes.rend(); it++) {
        u64 r_size = it->first;
        int r_count = it->second;
        u64 r_total = r_size * r_count;

        // Check if coverage will exceed each size
        if (cov < sz25) {
            if (cov + r_total >= sz25) {
                u64 diff = sz25 - cov;
                r25 += diff / r_size;
                r25 = diff % r_size == 0 ? r25 : r25 + 1;
            }
            else r25 += r_count;
        }

        if (cov < sz50) {
            if (cov + r_total >= sz50) {
                u64 diff = sz50 - cov;
                r50 += diff / r_size;
                r50 = diff % r_size == 0 ? r50 : r50 + 1;
            }
            else r50 += r_count;
        }

        if (cov < sz75) {
            if (cov + r_total >= sz75) {
                u64 diff = sz75 - cov;
                r75 += diff / r_size;
                r75 = diff % r_size == 0 ? r75 : r75 + 1;
            }
            else r75 += r_count;
        }
        else break;

        // Error cases
        if (r75 < r50 || r75 < r25 || r50 < r25) {
            cerr << "Error: " << r75 << " !< " << r50 << " !< " << r25 << endl;
            return -1;
        }
        cov += r_total;
    }

    //==================================================================================================
    // Print results
    //==================================================================================================
    // Order:
    // 1. Total regions
    // 2. 75-coverage
    // 3. 50-coverage
    // 4. 25-coverage
    // 5. Total tracked RSS
    // 6. Percentage of total RSS
    // 7. Mappings Scanned
    // 8. List of base addresses for each mapping scanned
    double tracked_rss_gb = double(total_pages) * 4096 / 1024 / 1024 / 1024;
    double rss_gb = double(totalRSS) / 1024 / 1024 / 1024;
    cout << dec << fixed << setprecision(3) << n_regions << "\t" << r75 << "\t" << r50 << "\t" << r25 << "\t";
    cout << tracked_rss_gb << "GB\t" << rss_gb << "GB\t" << largestRegions.size() << "\t";
    cout << hex << largestRegions[0].address;
    for (size_t i = 1; i < largestRegions.size(); i++) {
        cout << "--" << largestRegions[i].address;
    }
    cout << endl;
}
