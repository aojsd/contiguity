#!/bin/bash
# verify_tracepoints.sh
#
# Run with sudo on each target machine to verify that all tracepoints
# required by page_alloc_trace.bt and fault_vs_khugepaged.bt exist
# and have the expected field names.
#
# Usage: sudo ./verify_tracepoints.sh

set -e

echo "=== Machine Info ==="
echo "Hostname: $(hostname)"
echo "Kernel:   $(uname -r)"
echo "Date:     $(date -Iseconds)"
echo ""

echo "=== bpftrace Version ==="
bpftrace --version
echo ""

# Locate tracefs
TRACING_ROOT=""
if [ -d /sys/kernel/tracing/events ]; then
    TRACING_ROOT="/sys/kernel/tracing/events"
elif [ -d /sys/kernel/debug/tracing/events ]; then
    TRACING_ROOT="/sys/kernel/debug/tracing/events"
else
    echo "ERROR: Cannot find tracing events directory"
    exit 1
fi
echo "Tracing events root: ${TRACING_ROOT}"
echo ""

check_tracepoint() {
    local subsystem=$1
    local event=$2
    local path="${TRACING_ROOT}/${subsystem}/${event}"

    echo "--- ${subsystem}:${event} ---"
    if [ -d "$path" ]; then
        echo "  EXISTS: YES"
        if [ -f "${path}/format" ]; then
            echo "  FIELDS:"
            grep "field:" "${path}/format" | grep -v "common_" | sed 's/^/    /'
        fi
    else
        echo "  EXISTS: NO  *** MISSING ***"
    fi
    echo ""
}

echo "=== Required Tracepoints ==="
echo ""

echo ">> Script 1: page_alloc_trace.bt"
check_tracepoint "kmem" "mm_page_alloc"
check_tracepoint "kmem" "mm_page_free"
check_tracepoint "kmem" "mm_page_alloc_extfrag"

echo ">> Script 2: fault_vs_khugepaged.bt"
check_tracepoint "exceptions" "page_fault_user"
check_tracepoint "huge_memory" "mm_collapse_huge_page"
check_tracepoint "huge_memory" "mm_khugepaged_scan_pmd"

echo "=== BTF Access Test (curtask->mm) ==="
echo "Testing bpftrace can access curtask->mm..."
RESULT=$(timeout 5 bpftrace -e 'BEGIN { printf("curtask->mm = %p\n", curtask->mm); exit(); }' 2>&1)
if echo "$RESULT" | grep -q "curtask->mm = 0x"; then
    echo "  BTF ACCESS: OK"
    echo "  $RESULT"
else
    echo "  BTF ACCESS: FAILED"
    echo "  Output: $RESULT"
fi
echo ""

echo "=== Verification Complete ==="
