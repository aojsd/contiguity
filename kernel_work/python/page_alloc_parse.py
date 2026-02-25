#!/usr/bin/env python3
#
# page_alloc_parse.py
#
# Parses output from page_alloc_trace.bt. Consolidates allocation/free
# statistics by process name and migratetype. Overwrites the input file
# with a formatted report.
#
# Usage: python3 page_alloc_parse.py <path_to_bpftrace_output>
#
import sys
import re
from collections import defaultdict

# Migratetype names from include/linux/mmzone.h
MIGRATETYPE_NAMES = {
    0: "UNMOVABLE",
    1: "MOVABLE",
    2: "RECLAIMABLE",
    3: "HIGHATOMIC",
    4: "CMA",
    5: "ISOLATE",
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

def get_base_name(comm: str) -> str:
    """Consolidate thread variants: kworker/u4:0 -> kworker"""
    return comm.split('/')[0]

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

    # --- Parse bpftrace output ---
    scalar_re = re.compile(r'^@(\w+):\s*(\d+)$')
    keyed_re = re.compile(r'^@(\w+)\[(.*?)\]:\s*(\d+)$')
    hist_header_re = re.compile(r'^@(\w+)(?:\[(.*?)\])?:')
    hist_line_re = re.compile(r'\[(\d+[KMG]?),\s*(\d+[KMG]?)\)\s+(\d+)')

    scalars = {}
    keyed_maps = defaultdict(dict)
    histograms = defaultdict(lambda: defaultdict(list))
    current_hist_name = None
    current_hist_key = None

    for line in lines:
        line = line.rstrip()

        if m := scalar_re.match(line):
            name, value = m.groups()
            scalars[name] = int(value)
            current_hist_name = None
            continue

        if m := keyed_re.match(line):
            name, key_str, value = m.groups()
            keyed_maps[name][key_str] = int(value)
            current_hist_name = None
            continue

        if m := hist_header_re.match(line):
            name = m.group(1)
            key = m.group(2) if m.group(2) else "__global__"
            current_hist_name = name
            current_hist_key = key
            continue

        if current_hist_name:
            if m := hist_line_re.search(line):
                start_str, end_str, count_str = m.groups()
                histograms[current_hist_name][current_hist_key].append(
                    (parse_size(start_str), parse_size(end_str), int(count_str))
                )
                continue

        if line.strip() == '':
            current_hist_name = None

    # --- Generate Report ---
    out = []
    out.append("=" * 70)
    out.append("BUDDY ALLOCATOR TRACE REPORT")
    out.append("=" * 70)

    # Summary
    alloc_count = scalars.get('alloc_count', 0)
    alloc_pages = scalars.get('alloc_pages', 0)
    free_count = scalars.get('free_count', 0)
    free_pages = scalars.get('free_pages', 0)
    extfrag = scalars.get('extfrag_count', 0)
    extfrag_ownership = scalars.get('extfrag_ownership_change', 0)
    net_pages = alloc_pages - free_pages

    out.append(f"\n--- Summary ---")
    out.append(f"  Total allocations:       {alloc_count:>12,}")
    out.append(f"  Total pages allocated:   {alloc_pages:>12,}  ({alloc_pages * 4 / 1024:,.1f} MiB)")
    out.append(f"  Total frees:             {free_count:>12,}")
    out.append(f"  Total pages freed:       {free_pages:>12,}  ({free_pages * 4 / 1024:,.1f} MiB)")
    out.append(f"  Net pages:               {net_pages:>12,}  ({net_pages * 4 / 1024:,.1f} MiB)")
    out.append(f"  Extfrag fallbacks:       {extfrag:>12,}")
    out.append(f"  Extfrag ownership steals:{extfrag_ownership:>12,}")

    # Allocation order histogram
    if "__global__" in histograms.get("alloc_order", {}):
        out.append(f"\n--- Allocation Order Histogram ---")
        buckets = sorted(histograms["alloc_order"]["__global__"])
        if buckets:
            max_count = max(c for _, _, c in buckets)
            for start, end, count in buckets:
                bar = '█' * int(40 * count / max_count) if max_count > 0 else ''
                label = f"[{start}, {end})".ljust(15)
                pages = f"({1 << start}-{1 << (end-1)} pages)".ljust(20)
                out.append(f"  {label}{pages}{count:<12,} |{bar}")

    # Free order histogram
    if "__global__" in histograms.get("free_order", {}):
        out.append(f"\n--- Free Order Histogram ---")
        buckets = sorted(histograms["free_order"]["__global__"])
        if buckets:
            max_count = max(c for _, _, c in buckets)
            for start, end, count in buckets:
                bar = '█' * int(40 * count / max_count) if max_count > 0 else ''
                label = f"[{start}, {end})".ljust(15)
                out.append(f"  {label}{count:<12,} |{bar}")

    # Migratetype breakdown
    out.append(f"\n--- Allocations by Migratetype ---")
    out.append(f"  {'TYPE':<20} {'COUNT':>12} {'PAGES':>12} {'MiB':>10}")
    out.append(f"  {'-' * 56}")
    for key_str, count in sorted(keyed_maps.get('alloc_by_migtype', {}).items(),
                                  key=lambda x: int(x[1]), reverse=True):
        mt = int(key_str)
        mt_name = MIGRATETYPE_NAMES.get(mt, f"TYPE_{mt}")
        pages = keyed_maps.get('pages_by_migtype', {}).get(key_str, 0)
        out.append(f"  {mt_name:<20} {count:>12,} {pages:>12,} {pages * 4 / 1024:>10,.1f}")

    # Extfrag type breakdown
    if keyed_maps.get('extfrag_types'):
        out.append(f"\n--- Extfrag Fallback Types (requested -> fallback) ---")
        out.append(f"  {'REQUESTED':<15} {'FALLBACK':<15} {'COUNT':>10}")
        out.append(f"  {'-' * 42}")
        for key_str, count in sorted(keyed_maps['extfrag_types'].items(),
                                      key=lambda x: int(x[1]), reverse=True):
            parts = key_str.split(', ')
            if len(parts) == 2:
                alloc_mt = MIGRATETYPE_NAMES.get(int(parts[0]), parts[0])
                fall_mt = MIGRATETYPE_NAMES.get(int(parts[1]), parts[1])
                out.append(f"  {alloc_mt:<15} {fall_mt:<15} {count:>10,}")

    # Top allocators by page count (consolidated)
    out.append(f"\n--- Top Allocators by Page Count (Consolidated) ---")
    out.append(f"  {'PROCESS':<25} {'ALLOCS':>12} {'PAGES':>12} {'MiB':>10}")
    out.append(f"  {'-' * 61}")

    consolidated_pages = defaultdict(int)
    consolidated_allocs = defaultdict(int)
    for comm, pages in keyed_maps.get('pages_by_comm', {}).items():
        base = get_base_name(comm)
        consolidated_pages[base] += pages
    for comm, count in keyed_maps.get('alloc_by_comm', {}).items():
        base = get_base_name(comm)
        consolidated_allocs[base] += count

    for base, pages in sorted(consolidated_pages.items(),
                               key=lambda x: x[1], reverse=True)[:30]:
        allocs = consolidated_allocs.get(base, 0)
        out.append(f"  {base:<25} {allocs:>12,} {pages:>12,} {pages * 4 / 1024:>10,.1f}")

    # Top free-ers by page count (consolidated)
    out.append(f"\n--- Top Free-ers by Page Count (Consolidated) ---")
    out.append(f"  {'PROCESS':<25} {'FREES':>12} {'PAGES':>12} {'MiB':>10}")
    out.append(f"  {'-' * 61}")

    consolidated_free_pages = defaultdict(int)
    consolidated_frees = defaultdict(int)
    for comm, pages in keyed_maps.get('free_pages_by_comm', {}).items():
        base = get_base_name(comm)
        consolidated_free_pages[base] += pages
    for comm, count in keyed_maps.get('free_by_comm', {}).items():
        base = get_base_name(comm)
        consolidated_frees[base] += count

    for base, pages in sorted(consolidated_free_pages.items(),
                               key=lambda x: x[1], reverse=True)[:30]:
        frees = consolidated_frees.get(base, 0)
        out.append(f"  {base:<25} {frees:>12,} {pages:>12,} {pages * 4 / 1024:>10,.1f}")

    # Write output
    try:
        with open(input_file, 'w') as f:
            f.write('\n'.join(out) + '\n')
        print(f"Success: Report written to '{input_file}'.")
    except IOError as e:
        print(f"Error writing: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
