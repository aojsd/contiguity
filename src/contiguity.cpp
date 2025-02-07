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

#define u64 unsigned long long

map<u64, int> region_sizes;
vector<u64> region_starts_V;
vector<u64> region_lengths;
vector<u64> region_starts_P;

bool isInteger(const string& s) {
    // Check if s is a hexadecimal number (will not start with 0x)
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    });
}

int main(int argc, char* argv[]) {
    string line;
    u64 regions = 0;
    u64 last_VPN = 0;
    u64 last_PFN = 0;
    u64 region_size = 0;
    u64 total_pages = 0;
    u64 lowest_VPN = UINT64_MAX;
    while (getline(cin, line)) {
        istringstream iss(line);
        string first, second;

        // Extract the first two space-separated entries
        if (iss >> first >> second) {
            // Check if both are integers
            if (isInteger(first) && isInteger(second)) {
                u64 VPN = stol(first, 0, 16) >> 12;
                u64 PFN = stol(second, 0, 16) >> 12;
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
    // u64 largest_32 = 0;
    // u64 largest_128 = 0;
    // int i = 0;
    // for (auto it = region_sizes.rbegin(); it != region_sizes.rend(); it++) {
    //     u64 r_size = it->first;
    //     int r_count = it->second;
    //     if (i < 32) {
    //         if (32 - i < r_count) {
    //             largest_32 += (32 - i) * r_size;
    //         }
    //         else {
    //             largest_32 += r_count * r_size;
    //         }
    //     }
    //     if (i < 128) {
    //         if (128 - i < r_count) {
    //             largest_128 += (128 - i) * r_size;
    //         }
    //         else {
    //             largest_128 += r_count * r_size;
    //         }
    //     }
    //     i += r_count;
    // }
    // double p32 = (double)largest_32 / total_pages * 100;
    // double p128 = (double)largest_128 / total_pages * 100;

    // Get the number of regions needed to cover 75% of RSS (starting with the largest regions)
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

    // Print the results
    double rss_gb = double(total_pages) * 4096 / 1024 / 1024 / 1024;
    cout << dec << fixed << setprecision(3) << regions << "\t" << r75 << "\t" << r50 << "\t" << r25 << "\t";
    cout << rss_gb << " GB\t" << hex << lowest_VPN << endl;

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
