#!/bin/bash
#
# run_bpftrace_test.sh
#
# Self-contained test: starts memcached, attaches bpftrace scripts,
# runs memcached_requests with memDY workload, collects output.
#
# Usage: sudo ./run_bpftrace_test.sh
#   (must be run as root for bpftrace; drops to $SUDO_USER for memcached)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CONT_DIR="${BASE_DIR}/contiguity"
TMP_DIR="${BASE_DIR}/tmp"

MEMCACHED="${HOME}/software/memcached/memcached"
MEMCACHED_RQ="${CONT_DIR}/bin/memcached_requests"
WORKLOAD="${HOME}/software/YCSB/workloads/workload_traces/memDY.dat"

# If run via sudo, get the real user
REAL_USER="${SUDO_USER:-$(whoami)}"
REAL_HOME=$(eval echo "~${REAL_USER}")

# Fix paths if running as root via sudo
if [ "$(whoami)" = "root" ] && [ -n "${SUDO_USER:-}" ]; then
    MEMCACHED="${REAL_HOME}/software/memcached/memcached"
    WORKLOAD="${REAL_HOME}/software/YCSB/workloads/workload_traces/memDY.dat"
fi

mkdir -p "${TMP_DIR}"

echo "=== bpftrace Test: memcached + memDY ==="
echo "  memcached:  ${MEMCACHED}"
echo "  requests:   ${MEMCACHED_RQ}"
echo "  workload:   ${WORKLOAD}"
echo "  tmp dir:    ${TMP_DIR}"
echo ""

# Check prerequisites
for f in "${MEMCACHED}" "${MEMCACHED_RQ}" "${WORKLOAD}"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: Missing file: $f"
        exit 1
    fi
done

# Clean up on exit
cleanup() {
    echo ""
    echo "=== Cleaning up ==="
    [ -n "${MEMCACHED_PID:-}" ] && kill "${MEMCACHED_PID}" 2>/dev/null && echo "  Killed memcached (${MEMCACHED_PID})"
    [ -n "${BPF_ALLOC_PID:-}" ] && kill "${BPF_ALLOC_PID}" 2>/dev/null && echo "  Killed page_alloc_events.bt (${BPF_ALLOC_PID})"
    [ -n "${BPF_FAULT_PID:-}" ] && kill "${BPF_FAULT_PID}" 2>/dev/null && echo "  Killed fault_vs_khugepaged.bt (${BPF_FAULT_PID})"
    [ -n "${BPF_AGGR_PID:-}" ] && kill "${BPF_AGGR_PID}" 2>/dev/null && echo "  Killed page_alloc_trace.bt (${BPF_AGGR_PID})"
    wait 2>/dev/null
    # Restore original THP settings
    if [ -n "${ORIG_THP:-}" ]; then
        echo "  Restoring THP settings (${ORIG_THP}, sleep=${ORIG_SCAN_SLEEP:-10000}ms, pages=${ORIG_PAGES_TO_SCAN:-4096})"
        echo "${ORIG_THP}" > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true
        echo "${ORIG_SCAN_SLEEP:-10000}" > /sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs 2>/dev/null || true
        echo "${ORIG_PAGES_TO_SCAN:-4096}" > /sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan 2>/dev/null || true
    fi
}
trap cleanup EXIT

# 0. Enable THP=always and make khugepaged aggressive
ORIG_THP=$(cat /sys/kernel/mm/transparent_hugepage/enabled | grep -oP '\[\K[^\]]+')
ORIG_SCAN_SLEEP=$(cat /sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs)
ORIG_PAGES_TO_SCAN=$(cat /sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan)
echo "--- Configuring THP (was: ${ORIG_THP}, sleep=${ORIG_SCAN_SLEEP}ms, pages=${ORIG_PAGES_TO_SCAN}) ---"
echo always > /sys/kernel/mm/transparent_hugepage/enabled
# Aggressive scan: no sleep between rounds, scan 65536 pages (256MB) per round
echo 0 > /sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs
echo 65536 > /sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan
echo "  THP:              $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
echo "  scan_sleep_ms:    $(cat /sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs)"
echo "  pages_to_scan:    $(cat /sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan)"

# 1. Start memcached (6 threads)
#    Started as root with -u flag — memcached drops privileges itself.
#    This way $! is the real memcached PID (no sudo wrapper in between).
echo ""
echo "--- Starting memcached (6 threads) ---"
"${MEMCACHED}" -p 11211 -l 127.0.0.1 -m 4096 -t 6 -u "${REAL_USER}" &
MEMCACHED_PID=$!
sleep 1

if ! kill -0 "${MEMCACHED_PID}" 2>/dev/null; then
    echo "ERROR: memcached failed to start"
    exit 1
fi
echo "  memcached PID: ${MEMCACHED_PID}"

# 2. Start bpftrace scripts
echo ""
echo "--- Starting bpftrace scripts ---"

# Per-event alloc logger (for scatter plot)
bpftrace "${SCRIPT_DIR}/page_alloc_events.bt" \
    > "${TMP_DIR}/alloc_events.raw" 2>/dev/null &
BPF_ALLOC_PID=$!

# Fault vs khugepaged (for histograms)
bpftrace "${SCRIPT_DIR}/fault_vs_khugepaged.bt" "${MEMCACHED_PID}" \
    > "${TMP_DIR}/fault_khugepaged.raw" 2>/dev/null &
BPF_FAULT_PID=$!

# Aggregated page alloc trace
bpftrace "${SCRIPT_DIR}/page_alloc_trace.bt" \
    > "${TMP_DIR}/page_alloc_trace.raw" 2>/dev/null &
BPF_AGGR_PID=$!

sleep 2  # Let bpftrace attach

echo "  page_alloc_events PID:  ${BPF_ALLOC_PID}"
echo "  fault_vs_khugepaged PID: ${BPF_FAULT_PID}"
echo "  page_alloc_trace PID:   ${BPF_AGGR_PID}"

# 3. Run memcached_requests (as real user)
echo ""
echo "--- Running memcached_requests (memDY workload) ---"
echo "  (This may take a while for the full 6GB workload)"
echo ""

runuser -u "${REAL_USER}" -- "${MEMCACHED_RQ}" "${WORKLOAD}" --live -c 6 \
    2>&1 | tee "${TMP_DIR}/memcached_rq.out" || true

echo ""
echo "--- Workload complete ---"

# 4. Stop bpftrace (send SIGINT for clean map dump)
echo ""
echo "--- Stopping bpftrace scripts (SIGINT for map dump) ---"
kill -INT "${BPF_ALLOC_PID}" 2>/dev/null || true
kill -INT "${BPF_FAULT_PID}" 2>/dev/null || true
kill -INT "${BPF_AGGR_PID}" 2>/dev/null || true
BPF_ALLOC_PID=""
BPF_FAULT_PID=""
BPF_AGGR_PID=""

sleep 3  # Let bpftrace flush

# 5. Stop memcached
kill "${MEMCACHED_PID}" 2>/dev/null || true
MEMCACHED_PID=""

# Fix ownership of output files
chown "${REAL_USER}:" "${TMP_DIR}/alloc_events.raw" \
                      "${TMP_DIR}/fault_khugepaged.raw" \
                      "${TMP_DIR}/page_alloc_trace.raw" \
                      "${TMP_DIR}/memcached_rq.out" 2>/dev/null || true

echo ""
echo "=== Output files ==="
echo "  ${TMP_DIR}/alloc_events.raw       (per-event alloc log for scatter plot)"
echo "  ${TMP_DIR}/fault_khugepaged.raw    (fault vs khugepaged histograms)"
echo "  ${TMP_DIR}/page_alloc_trace.raw    (aggregated alloc trace)"
echo "  ${TMP_DIR}/memcached_rq.out        (memcached_requests output)"
echo ""
wc -l "${TMP_DIR}/alloc_events.raw" "${TMP_DIR}/fault_khugepaged.raw" "${TMP_DIR}/page_alloc_trace.raw" 2>/dev/null || true
echo ""
echo "=== Next step ==="
echo "  python3 ${CONT_DIR}/results/REBUTTAL_ISCA_2026/plot_bpftrace_test.py \\"
echo "    ${TMP_DIR}/alloc_events.raw \\"
echo "    ${TMP_DIR}/fault_khugepaged.raw \\"
echo "    --outdir ${CONT_DIR}/results/REBUTTAL_ISCA_2026/figures"
