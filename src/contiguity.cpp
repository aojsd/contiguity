#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <cctype>
#include <algorithm>
#include <map>
#include <vector>
#include <fstream>
#include <cstdint>

using namespace std;

map<unsigned long, int> region_sizes;
vector<unsigned long> region_starts_V;
vector<unsigned long> region_lengths;
vector<unsigned long> region_starts_P;

bool isInteger(const string& s) {
    // Check if s is a hexadecimal number (will not start with 0x)
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    });
}

int main(int argc, char* argv[]) {
    string line;
    unsigned long regions = 0;
    unsigned long last_VPN = 0;
    unsigned long last_PFN = 0;
    unsigned long region_size = 0;
    unsigned long total_pages = 0;
    unsigned long lowest_VPN = UINT64_MAX;
    while (getline(cin, line)) {
        istringstream iss(line);
        string first, second;

        // Extract the first two space-separated entries
        if (iss >> first >> second) {
            // Check if both are integers
            if (isInteger(first) && isInteger(second)) {
                unsigned long VPN = stol(first, 0, 16) >> 12;
                unsigned long PFN = stol(second, 0, 16) >> 12;
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
                        regions++;

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
                            regions++;

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
        }
    }
    // Calculate the percentage of memory covered by the largest 32 and 128 regions
    unsigned long largest_32 = 0;
    unsigned long largest_128 = 0;
    int i = 0;
    for (auto it = region_sizes.rbegin(); it != region_sizes.rend(); it++) {
        unsigned long r_size = it->first;
        int r_count = it->second;
        if (i < 32) {
            if (32 - i < r_count) {
                largest_32 += (32 - i) * r_size;
            }
            else {
                largest_32 += r_count * r_size;
            }
        }
        if (i < 128) {
            if (128 - i < r_count) {
                largest_128 += (128 - i) * r_size;
            }
            else {
                largest_128 += r_count * r_size;
            }
        }
        i += r_count;
    }
    double p32 = (double)largest_32 / total_pages * 100;
    double p128 = (double)largest_128 / total_pages * 100;

    // Print the results
    cout << dec << fixed << setprecision(5) << regions << "\t" << p32 << "%\t" << p128 << "%\t" << total_pages << "\t" << lowest_VPN << endl;

    // Take an optional dump file as command line argument
    if (argc > 1) {
        ofstream dump_file(argv[1]);
        for (auto it = region_sizes.begin(); it != region_sizes.end(); it++) {
            dump_file << it->first << " " << it->second << endl;
        }
        dump_file.close();
    }
    return 0;
}
