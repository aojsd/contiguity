#!/usr/bin/env python3
#
# fault_khugepaged_parse.py
#
# Parses output from fault_vs_khugepaged.bt. Handles two formats:
#   - New streaming format: "scan <N>" lines + scalar/keyed maps at END
#   - Old format: bpftrace histogram maps (@hist_faults_per_scan, etc.)
#
# Translates scan status codes to human-readable names, formats histograms,
# and generates a summary report. Overwrites the input file.
#
# Usage: python3 fault_khugepaged_parse.py <path_to_bpftrace_output>
#
import sys
import re
import math
from collections import defaultdict

# From include/trace/events/huge_memory.h SCAN_STATUS enum
# Identical on kernels 6.13 through 6.16.
SCAN_STATUS = {
    0: "FAIL",
    1: "SUCCEED",
    2: "PMD_NULL",
    3: "PMD_NONE",
    4: "PMD_MAPPED",
    5: "EXCEED_NONE_PTE",
    6: "EXCEED_SWAP_PTE",
    7: "EXCEED_SHARED_PTE",
    8: "PTE_NON_PRESENT",
    9: "PTE_UFFD_WP",
    10: "PTE_MAPPED_HUGEPAGE",
    11: "PAGE_RO",
    12: "LACK_REFERENCED_PAGE",
    13: "PAGE_NULL",
    14: "SCAN_ABORT",
    15: "PAGE_COUNT",
    16: "PAGE_LRU",
    17: "PAGE_LOCK",
    18: "PAGE_ANON",
    19: "PAGE_COMPOUND",
    20: "ANY_PROCESS",
    21: "VMA_NULL",
    22: "VMA_CHECK",
    23: "ADDRESS_RANGE",
    24: "DEL_PAGE_LRU",
    25: "ALLOC_HUGE_PAGE_FAIL",
    26: "CGROUP_CHARGE_FAIL",
    27: "TRUNCATED",
    28: "PAGE_HAS_PRIVATE",
    29: "STORE_FAILED",
    30: "COPY_MC",
    31: "PAGE_FILLED",
}

def parse_size(s: str) -> int:
    """Parses strings like '4K', '256', '1M' into an integer."""
    s = s.upper()
    if s.endswith('K'):
        return int(s[:-1]) * 1024
    if s.endswith('M'):
        return int(s[:-1]) * 1024 * 1024
    if s.endswith('G'):
        return int(s[:-1]) * 1024 * 1024 * 1024
    return int(s)

def format_size(n: int) -> str:
    """Formats an integer back into a human-readable string like '4K'."""
    if n >= 1024**3 and n % 1024**3 == 0:
        return f"{n // 1024**3}G"
    if n >= 1024**2 and n % 1024**2 == 0:
        return f"{n // 1024**2}M"
    if n >= 1024 and n % 1024 == 0:
        return f"{n // 1024}K"
    return str(n)

def build_log2_histogram(values):
    """
    Bucket a list of integer values into power-of-2 ranges matching
    bpftrace's hist() output: [2^i, 2^(i+1)).
    Values of 0 go into a [0, 1) bucket.
    Returns list of (start, end, count) tuples (only non-zero buckets).
    """
    if not values:
        return []
    buckets = defaultdict(int)
    for v in values:
        if v <= 0:
            buckets[0] += 1
        else:
            exp = int(math.log2(v))
            buckets[exp] += 1
    result = []
    for exp, count in sorted(buckets.items()):
        if exp == 0:
            # bpftrace puts 0 and 1 into [0, 1) and [1, 2) respectively,
            # but for our purposes values of 0 mean "no faults between scans"
            result.append((0, 1, count))
        else:
            result.append((2**exp, 2**(exp + 1), count))
    return result

def format_histogram(buckets):
    """Format a list of (start, end, count) buckets into ASCII histogram lines."""
    lines = []
    if not buckets:
        return lines
    max_count = max(c for _, _, c in buckets)
    for start, end, count in sorted(buckets):
        bar = '\u2588' * int(40 * count / max_count) if max_count > 0 else ''
        label = f"[{format_size(start)}, {format_size(end)})".ljust(20)
        lines.append(f"    {label}{count:<10,} |{bar}")
    return lines

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <bpftrace_output_file>", file=sys.stderr)
        sys.exit(1)

    input_file = sys.argv[1]

    try:
        with open(input_file, 'r') as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"Error: File not found at '{input_file}'", file=sys.stderr)
        sys.exit(1)

    # --- Parse ---
    scan_event_re = re.compile(r'^scan\s+(\d+)$')
    scalar_re = re.compile(r'^@(\w+):\s*(\d+)$')
    keyed_re = re.compile(r'^@(\w+)\[(.*?)\]:\s*(\d+)$')
    hist_header_re = re.compile(r'^@(\w+)(?:\[(.*?)\])?:')
    hist_line_re = re.compile(r'\[(\d+[KMG]?),\s*(\d+[KMG]?)\)\s+(\d+)')

    scan_events = []       # ordered list of fault counts between scans
    scalars = {}
    keyed_maps = defaultdict(dict)
    histograms = defaultdict(list)
    current_hist_name = None

    for line in lines:
        line = line.rstrip()

        if m := scan_event_re.match(line):
            scan_events.append(int(m.group(1)))
            continue

        if m := scalar_re.match(line):
            scalars[m.group(1)] = int(m.group(2))
            current_hist_name = None
            continue

        if m := keyed_re.match(line):
            keyed_maps[m.group(1)][m.group(2)] = int(m.group(3))
            current_hist_name = None
            continue

        if m := hist_header_re.match(line):
            current_hist_name = m.group(1)
            continue

        if current_hist_name:
            if m := hist_line_re.search(line):
                histograms[current_hist_name].append(
                    (parse_size(m.group(1)), parse_size(m.group(2)), int(m.group(3)))
                )
                continue

        if line.strip() == '':
            current_hist_name = None

    # If we have streaming scan events, build the histogram from them
    if scan_events:
        histograms['hist_faults_per_scan'] = build_log2_histogram(scan_events)

    # --- Generate Report ---
    out = []
    out.append("=" * 70)
    out.append("PAGE FAULT vs KHUGEPAGED REPORT")
    out.append("=" * 70)

    total_faults = scalars.get('total_faults', 0)
    total_scans = scalars.get('total_scans', 0)

    out.append(f"\n--- Summary ---")
    out.append(f"  Total page faults (target PID): {total_faults:>12,}")
    out.append(f"  Total scans (all procs):        {total_scans:>12,}")
    if scan_events:
        out.append(f"  Scan events recorded:           {len(scan_events):>12,}")

    # Scan status breakdown
    if keyed_maps.get('scan_status'):
        out.append(f"\n--- Scan Status (All Processes) ---")
        out.append(f"  {'STATUS':<30} {'COUNT':>10}")
        out.append(f"  {'-' * 42}")
        for key, count in sorted(keyed_maps['scan_status'].items(),
                                  key=lambda x: int(x[1]), reverse=True):
            status_name = SCAN_STATUS.get(int(key), f"UNKNOWN_{key}")
            out.append(f"  {status_name:<30} {count:>10,}")

    # Faults-per-scan histogram
    if histograms.get('hist_faults_per_scan'):
        out.append(f"\n--- Faults Between Any Scan ---")
        out.extend(format_histogram(histograms['hist_faults_per_scan']))

    # Write
    try:
        with open(input_file, 'w') as f:
            f.write('\n'.join(out) + '\n')
        print(f"Success: Report written to '{input_file}'.")
    except IOError as e:
        print(f"Error writing: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
