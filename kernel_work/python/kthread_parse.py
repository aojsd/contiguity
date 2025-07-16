#!/usr/bin/env python3
#
# A script to parse and consolidate bpftrace output from a file.
# It reads from the specified file, processes the data, and then
# overwrites the file with the consolidated summary.
#
# Usage:
#   1. Save bpftrace output: sudo ./your_script.bt > output.txt
#   2. Run this script on the file: python3 consolidate.py output.txt
#
import sys
import re
from collections import defaultdict

def parse_size(s: str) -> int:
    """Parses strings like "4K", "256", "1M" into an integer."""
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

def get_base_name(thread_name: str) -> str:
    """
    Finds the base name for a thread.
    Example: 'kworker/u4:0' -> 'kworker'
    """
    return thread_name.split('/')[0]

def main():
    """Reads a bpftrace output file, processes it, and overwrites it."""
    # --- Input File Handling ---
    if len(sys.argv) != 2:
        print(f"Error: Please provide a single file path as an argument.", file=sys.stderr)
        print(f"Usage: {sys.argv[0]} <path_to_bpftrace_output_file>", file=sys.stderr)
        sys.exit(1)

    input_file = sys.argv[1]

    try:
        with open(input_file, 'r') as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"Error: File not found at '{input_file}'", file=sys.stderr)
        sys.exit(1)

    # --- Data Processing ---
    consolidated_totals = defaultdict(int)
    consolidated_hists = defaultdict(lambda: defaultdict(int))
    consolidated_threads = defaultdict(set)

    total_re = re.compile(r'^@total_runtime\[(.*?)\]:\s*(\d+)$')
    hist_header_re = re.compile(r'^@invocations\[(.*?)\]:')
    hist_line_re = re.compile(r'\[(\d+[KMG]?), (\d+[KMG]?)\)\s+(\d+)')

    current_hist_base_name = None

    for line in lines:
        if m := total_re.match(line):
            thread_name, value = m.groups()
            base_name = get_base_name(thread_name)
            consolidated_totals[base_name] += int(value)
            consolidated_threads[base_name].add(thread_name)
            current_hist_base_name = None
            continue

        if m := hist_header_re.match(line):
            thread_name = m.groups()[0]
            base_name = get_base_name(thread_name)
            consolidated_threads[base_name].add(thread_name)
            current_hist_base_name = base_name
            continue

        if current_hist_base_name:
            if m := hist_line_re.search(line):
                start_str, end_str, count = m.groups()
                start_val = parse_size(start_str)
                end_val = parse_size(end_str)
                consolidated_hists[current_hist_base_name][(start_val, end_val)] += int(count)

    # --- Generate Formatted Output ---
    output_lines = ["--- Consolidated Kernel Thread On-CPU Time ---"]

    for base_name in sorted(consolidated_totals.keys()):
        total_ns = consolidated_totals[base_name]
        total_ms = total_ns / 1_000_000
        num_threads = len(consolidated_threads[base_name])

        output_lines.append("\n" + "="*40)
        output_lines.append(f" Thread Group: {base_name} ({num_threads} threads)")
        output_lines.append("="*40)
        output_lines.append(f"  Total Combined On-CPU Time: {total_ns:,} ns ({total_ms:,.3f} ms)")

        if base_name in consolidated_hists:
            output_lines.append("\n  Combined On-CPU Duration Histogram:")
            hist_data = consolidated_hists[base_name]
            max_count = max(hist_data.values()) if hist_data else 1
            
            for (start_val, end_val), count in sorted(hist_data.items()):
                bar = '█' * int(40 * count / max_count)
                start_str = format_size(start_val)
                end_str = format_size(end_val)
                output_lines.append(f"    [{start_str}, {end_str})".ljust(22) + f"{count:<10} |{bar}")

    # --- Print to stdout ---
    # print("\n".join(output_lines))

    # --- Overwrite Original File ---
    try:
        with open(input_file, 'w') as f:
            f.write("\n".join(output_lines))
            f.write("\n")
        print(f"✅ Success! File '{input_file}' was overwritten with consolidated data.")
    except IOError as e:
        print(f"Error: Could not write to file '{input_file}'.\n{e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()