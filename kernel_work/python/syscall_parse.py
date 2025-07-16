#!/usr/bin/env python3
#
# A script to parse the output from the syscall tracing bpftrace script.
# It translates syscall numbers to names, groups data by syscall and then
# by thread, and presents a consolidated report.
#
# Usage (stdout):
#   1. sudo ./syscall_trace.bt <PID> > syscall_output.txt
#   2. python3 parse_syscalls.py syscall_output.txt
#
# Usage (overwrite):
#   (See commented out code at the end of the script)
#
import sys
import re
import subprocess
from collections import defaultdict
from functools import lru_cache

# --- Syscall Name Translation ---

@lru_cache(maxsize=None)
def get_syscall_name(syscall_nr: str) -> str:
    """
    Translates a syscall number to its name using the 'ausyscall' utility.
    Uses a cache to avoid calling the subprocess for the same number repeatedly.
    Falls back to returning the number if 'ausyscall' is not available.
    """
    try:
        # The --exact flag ensures we only get the name for the given number
        result = subprocess.run(
            ['ausyscall', '--exact', syscall_nr],
            capture_output=True,
            text=True,
            check=True
        )
        # The output is just the name, so we strip any whitespace
        return result.stdout.strip()
    except (FileNotFoundError, subprocess.CalledProcessError):
        # If ausyscall is not found or fails, just return the number
        return f"syscall_{syscall_nr}"

# --- Helper functions for parsing and formatting ---

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

def main():
    """Reads a bpftrace output file, processes it, and prints a report."""
    if len(sys.argv) != 2:
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
    # NEW: Added "invocations" to the data structure
    data = defaultdict(lambda: defaultdict(lambda: {"total_ns": 0, "hist": [], "invocations": 0}))

    # Regex to parse bpftrace map output with composite keys
    total_re = re.compile(r'^@cns\[(\d+), (\d+)\]:\s*(\d+)')
    hist_header_re = re.compile(r'^@ns\[(\d+), (\d+)\]:')
    hist_line_re = re.compile(r'\[(\d+[KMG]?), (\d+[KMG]?)\)\s+(\d+)')

    current_syscall_name = None
    current_tid = None

    for line in lines:
        # Match total time lines
        if m := total_re.match(line):
            syscall_nr, tid, total_ns = m.groups()
            syscall_name = get_syscall_name(syscall_nr)
            data[syscall_name][tid]["total_ns"] = int(total_ns)
            continue

        # Match histogram headers
        if m := hist_header_re.match(line):
            syscall_nr, tid = m.groups()
            current_syscall_name = get_syscall_name(syscall_nr)
            current_tid = tid
            continue

        # Match histogram data lines
        if current_syscall_name and current_tid:
            if m := hist_line_re.search(line):
                start_str, end_str, count_str = m.groups()
                count = int(count_str)
                start_val = parse_size(start_str)
                end_val = parse_size(end_str)
                data[current_syscall_name][current_tid]["hist"].append((start_val, end_val, count))
                data[current_syscall_name][current_tid]["invocations"] += count # NEW: Sum invocations from hist
                continue

        # Reset context if a line doesn't match
        current_syscall_name = None
        current_tid = None


    # --- Generate Formatted Output ---
    output_lines = []
    pid = " (PID from file)" # Placeholder, as we don't know the PID from the file

    output_lines.append(f"--- Syscall Latency Report{pid} ---")

    # Sort by syscall name for consistent output
    for syscall_name, threads in sorted(data.items()):
        output_lines.append("\n" + "="*60)
        output_lines.append(f"Syscall: {syscall_name}")
        output_lines.append("="*60)

        is_first_thread = True
        # Sort by thread ID
        for tid, stats in sorted(threads.items(), key=lambda item: int(item[0])):
            # Add a newline separator for all but the first thread in the group
            if not is_first_thread:
                output_lines.append("")

            total_ns = stats["total_ns"]
            total_ms = total_ns / 1_000_000
            invocations = stats["invocations"]

            output_lines.append(f"\tThread ID: {tid}")
            output_lines.append(f"\t\tTotal Invocations: {invocations:,}")
            output_lines.append(f"\t\tTotal Time: {total_ns:,} ns ({total_ms:,.3f} ms)")

            if stats["hist"]:
                output_lines.append("\t\tLatency Histogram (ns):")
                hist_data = sorted(stats["hist"])
                max_count = max(c for _, _, c in hist_data) if hist_data else 1

                for start_val, end_val, count in hist_data:
                    bar = '█' * int(40 * count / max_count)
                    start_str = format_size(start_val)
                    end_str = format_size(end_val)
                    
                    # Build the label part with space-based padding for alignment
                    label = f"[{start_str}, {end_str})".ljust(20)
                    
                    # Prepend tabs for indentation, then add the aligned content
                    line = f"\t\t\t{label}{count:<10} |{bar}"
                    output_lines.append(line)
            
            is_first_thread = False

    # --- Print to stdout ---
    # print("\n".join(output_lines))

    # --- Overwrite Original File ---
    try:
        with open(input_file, 'w') as f:
            f.write("\n".join(output_lines))
            f.write("\n")
        print(f"✅ Success! File '{input_file}' was overwritten with the report.")
    except IOError as e:
        print(f"Error: Could not write to file '{input_file}'.\n{e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
