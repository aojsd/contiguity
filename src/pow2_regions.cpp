#include <iostream>
#include <assert.h>
#include "pmap.h"

using namespace std;

// =================================================================================================
// Given a start and end address, find the number of aligned pages of power-of-2 sizes
//  - Regions used by a larger page cannot be re-used by a smaller page
// =================================================================================================
void count_pow2(u64 start, u64 end, int pow_largest, u64* region_count) {
    // Return if pow_largest is less than the minimum we are considering
    u64 region_size = end - start;
    assert(region_size >= 0);
    if (pow_largest < CONT_LOWEST || region_size == 0) return;

    // Region too small
    u64 pow_mask = (1 << pow_largest) - 1;
    if (region_size < ((u64) 1 << pow_largest)) {
        count_pow2(start, end, pow_largest - 1, region_count);
        return;
    }
    
    // Check if an aligned page of order pow_largest can fit
    //  - End can simply be truncated to the nearest page
    //  - Start must be aligned to the nearest page greater than or equal to start
    u64 end_aligned = end >> pow_largest;
    u64 start_truncated = start >> pow_largest;
    u64 start_aligned = start_truncated + (start % (1 << pow_largest) == 0 ? 0 : 1);

    // No aligned page can fit, continue
    assert(start_aligned <= end_aligned);
    if (start_aligned == end_aligned) {
        count_pow2(start, end, pow_largest - 1, region_count);
        return;
    }
    
    // Region can fit, increment count
    u64 n_pages = end_aligned - start_aligned;
    region_count[pow_largest - CONT_LOWEST] += n_pages;

    // Lower sub-region
    count_pow2(start, start_aligned << pow_largest, pow_largest - 1, region_count);

    // Upper sub-region
    count_pow2(end_aligned << pow_largest, end, pow_largest - 1, region_count);
}

// Aligned version of count_pow2
void count_pow2_aligned(u64 start, u64 end, u64 v_start, int pow_largest, u64* region_count) {
    // Return if pow_largest is less than the minimum we are considering
    u64 region_size = end - start;
    assert(region_size >= 0);
    if (pow_largest < CONT_LOWEST || region_size == 0) return;

    // Region too small or virtual and physical alignment don't match, continue
    u64 pow_mask = (1 << pow_largest) - 1;
    if (region_size < ((u64) 1 << pow_largest)) {
        count_pow2_aligned(start, end, v_start, pow_largest - 1, region_count);
        return;
    }
    if ((start & pow_mask) != (v_start & pow_mask)) {
        count_pow2_aligned(start, end, v_start, pow_largest - 1, region_count);
        return;
    }
    
    // Check if an aligned page of order pow_largest can fit
    //  - End can simply be truncated to the nearest page
    //  - Start must be aligned to the nearest page greater than or equal to start
    u64 end_aligned = end >> pow_largest;
    u64 start_truncated = start >> pow_largest;
    u64 start_aligned = start_truncated + (start % (1 << pow_largest) == 0 ? 0 : 1);

    // No aligned page can fit, continue
    assert(start_aligned <= end_aligned);
    if (start_aligned == end_aligned) {
        count_pow2_aligned(start, end, v_start, pow_largest - 1, region_count);
        return;
    }
    
    // Region can fit, increment count
    u64 n_pages = end_aligned - start_aligned;
    region_count[pow_largest - CONT_LOWEST] += n_pages;

    // Lower sub-region
    count_pow2_aligned(start, start_aligned << pow_largest, v_start, pow_largest - 1, region_count);

    // Upper sub-region
    v_start += (end_aligned << pow_largest) - start;
    count_pow2_aligned(end_aligned << pow_largest, end, v_start, pow_largest - 1, region_count);
}