#include <iostream>
#include "pmap.h"

using namespace std;

int main(int argc, char** argv) {
    u64 pow2_regions[15] = {0};
    u64 start = 0b0000000000000010010000000000000;
    u64 end   = 0b0000000000010000000000000000000;
    count_pow2(start, end, 18, pow2_regions);

    for (int i = 0; i < 15; i++) {
        u64 kb_page = 1 << (i + 6);
        if (kb_page >= 1024) {
            kb_page >>= 10;
            if (kb_page >= 1024) cout << (kb_page >> 10) << "GB:\t";
            else cout << (kb_page) << "MB:\t";
        } else {
            cout << kb_page << "KB:\t";
        }

        cout << hex << pow2_regions[i] << dec << endl;
    }
    return 0;
}