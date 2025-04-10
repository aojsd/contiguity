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
#include <cassert>
#include "pmap.h"

using namespace std;

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
    if (argc < 3) {
        cerr << "Usage: sudo "<< argv[0] << " <pid> <outfile> [max_regions]\n";
        return EXIT_FAILURE;
    }
    string out_file = argv[2];

    // If max regions is specified, use it instead of coverage
    int max_regions = INT32_MAX;
    if (argc == 4) {
        coverage = 1;
        max_regions = stoi(argv[3]);
    }

    // Find regions
    vector<MemoryRegion> regions;
    size_t totalRSS;
    parsePmapOutput(regions, totalRSS, max_regions != -1);
    vector<MemoryRegion> largestRegions = findLargestRegions(regions, totalRSS, coverage, max_regions);
    // cerr << "Regions (" << coverage * 100 << "% RSS):\t" << largestRegions.size() << endl;

    // Open pagemap file for this pid
    pid_t pid = stoul(argv[1]);
    char pagemap_file[BUFSIZ];
    snprintf(pagemap_file, sizeof(pagemap_file), "/proc/%ju/pagemap", (uintmax_t)pid);
    int pagemap_fd = open(pagemap_file, O_RDONLY);

    // Open /proc/kpageflags
    char kpageflags_file[BUFSIZ];
    snprintf(kpageflags_file, sizeof(kpageflags_file), "/proc/kpageflags");
    int kflags_fd = open(kpageflags_file, O_RDONLY);

    // Region data
    u64 power2_regions[CONT_HIGHEST - CONT_LOWEST + 1] = {0};
    u64 n_regions = 0;
    u64 total_pages = 0;

    // Loop through virtual memory areas
    size_t pageSize = sysconf(_SC_PAGE_SIZE);
    size_t total_virtual_size = 0;
    vector<tuple<uintptr_t, uintptr_t, uint64_t>> mappings;
    for (const auto &region : largestRegions) {
        u64 last_VPN = 0;
        u64 last_PFN = 0;
        u64 region_size = 0;
        total_virtual_size += region.size;
        // for (size_t i = 0; i < region.size; i += pageSize) {
        for (size_t i = 0; i < region.size; ) {
            // =================================================================
            // Get virtual to physical mappings
            // =================================================================
            uintptr_t vaddr = region.address + i;
            uintptr_t paddr;
            if (virt_to_phys_user(&paddr, vaddr, pagemap_fd, kflags_fd)) {
                cerr << "Failed to convert virtual address to physical\n";
                return EXIT_FAILURE;
            }
            assert( (vaddr & 0xFFF) == 0);
            assert( (paddr & 0xFFF) == 0 || (paddr & 0xFFF) == 1);
            bool thp = paddr & 1;
            if (thp) i += pageSize * 512;
            else i += pageSize;

            // =================================================================
            // Analyze contiguity
            // =================================================================
            // Check if mapping is valid
            u64 VPN = vaddr >> 12;
            u64 PFN = paddr >> 12;
            if (PFN == 0) {
                assert(!thp);
                if (region_size > 0) {
                    n_regions++;

                    // Record region
                    total_pages += region_size;

                    // Get number of each power of 2 region
                    u64 start = last_PFN - region_size + 1;
                    u64 end = last_PFN + 1;
                    u64 v_start = last_VPN - region_size + 1;
                    count_pow2(start, end, v_start, CONT_HIGHEST, power2_regions);

                    // Track region start
                    region_starts_V.push_back(last_VPN - region_size + 1);
                    region_lengths.push_back(region_size);
                    region_starts_P.push_back(last_PFN - region_size + 1);

                    // Reset region
                    region_size = 0;
                }
            }
            // Continues contiguous region
            else {
                if (VPN == last_VPN + 1 && PFN == last_PFN + 1) {
                    // region_size++;
                    if (thp)
                        region_size += 512;
                    else
                        region_size++;
                }
                // New region started
                else {
                    if (region_size > 0) {
                        n_regions++;

                        // Record region
                        total_pages += region_size;

                        // Get number of each power of 2 region
                        u64 start = last_PFN - region_size + 1;
                        u64 end = last_PFN + 1;
                        u64 v_start = last_VPN - region_size + 1;
                        count_pow2(start, end, v_start, CONT_HIGHEST, power2_regions);

                        // Track region start
                        region_starts_V.push_back(last_VPN - region_size + 1);
                        region_lengths.push_back(region_size);
                        region_starts_P.push_back(last_PFN - region_size + 1);
                    }
                    if (thp)
                        region_size = 512;
                    else
                        region_size = 1;
                }
                // Update last VPN and PFN
                if (thp) {
                    last_VPN = VPN + 511;
                    last_PFN = PFN + 511;
                }
                else {
                    last_VPN = VPN;
                    last_PFN = PFN;
                }
            }
        }
    }
    close(pagemap_fd);
    close(kflags_fd);

    //==================================================================================================
    // Print results
    //==================================================================================================
    // Order:
    // 1. Total tracked RSS
    // 2. Percentage of total RSS
    // 3. Mappings Scanned
    // 4-18. Number of regions of size 2^4, 2^5, ..., 2^18
    double virtual_gb = double(total_virtual_size) / 1024 / 1024 / 1024;
    double tracked_rss_gb = double(total_pages) * 4096 / 1024 / 1024 / 1024;
    double rss_gb = double(totalRSS) / 1024 / 1024 / 1024;
    cout << dec << fixed << setprecision(3);
    cout << virtual_gb << " GB," << tracked_rss_gb << "GB," << rss_gb << "GB," << largestRegions.size();
    for (u64 r : power2_regions) {
        cout << "," << r;
    }
    cout << endl;

    // Write data on contiguous regions to file
    ofstream out(out_file);
    if (!out.is_open()) {
        cerr << "Failed to open file " << out_file << endl;
        return EXIT_FAILURE;
    }
    out << "VPN,PFN,Size\n";
    for (size_t i = 0; i < region_starts_V.size(); i++) {
        out << hex << region_starts_V[i] << "," << region_starts_P[i] << "," << region_lengths[i] << endl;
    }
    out.close();
}
