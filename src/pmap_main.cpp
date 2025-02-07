#include "pmap.h"
using namespace std;
float coverage = 0.9;

// Finds the largest memory regions of a process that consume at least 80% of the total RSS
// For each region, prints the mapping of every virtual page (VPN and PFN)
// Input:
// - pid: the process ID
// - stdin: the output of pmap -x <pid>
int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: sudo %s <pid>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // return parse_all(argc, argv);

    // Find regions
    vector<MemoryRegion> regions;
    size_t totalRSS;
    parsePmapOutput(regions, totalRSS);
    vector<MemoryRegion> largestRegions = findLargestRegions(regions, totalRSS);
    // cerr << "Regions (" << coverage * 100 << "% RSS):\t" << largestRegions.size() << endl;

    // Print mappings
    pid_t pid = stoul(argv[1]);
    size_t pageSize = sysconf(_SC_PAGE_SIZE);
    for (const auto &region : largestRegions) {
        for (size_t i = 0; i < region.size; i += pageSize) {
            uintptr_t vaddr = stoul(region.address, nullptr, 16) + i;
            uintptr_t paddr;
            if (virt_to_phys_user(&paddr, pid, vaddr)) {
                printf("Failed to convert virtual address to physical\n");
                return EXIT_FAILURE;
            }
            cout << hex << vaddr << " " << paddr << endl;
        }
    }
}
