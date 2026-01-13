#!/bin/bash
# Take pid, process name, waits for a process containing this name has over 50% CPU usage
if [ $# -lt 2 ]; then
    echo "Usage: $0 <pid> <process_name_for_logs> [max_regions]"
    exit 1
fi
TMP_DIR=/home/michael/ISCA_2025_results/tmp
CONT_DIR=/home/michael/ISCA_2025_results/contiguity

# Args
pid="$1"
process_name="$2"
max_regions="$3"

# Found process
full_process_name=$(ps -p $pid -o comm=)
echo "Monitoring contiguity of process $pid (name: $full_process_name)..." 1>&2

# =============================================
# Contiguity tracking
# =============================================
# Fields: Time, n_regions, r75, r50, r25, Tracked RSS, Total RSS, n_mappings, list_mappings
DIR=/home/michael/ISCA_2025_results/contiguity/
echo "Time,Tracked-VSize,Tracked-RSS,Total-RSS,n_mappings,4K,8K,16K,32K,64K,128K,256K,512K,1M,2M,4M,8M,16M,32M,64M,128M,256M,512M,1G"

# Initial sleep, default 5s
# sleep 5

# Loop until the process with the given PID is no longer running
mkdir -p ${TMP_DIR}/ptables
while ps -p $pid > /dev/null; do
    PTIME=$(ps -p $pid -o etime=)
    TIME=$(python3 $DIR/src/python/parse_time.py $PTIME)
    CONTIG=$(sudo pmap -x $pid | sudo nice -n -20 $DIR/bin/dump_pagemap $pid ${TMP_DIR}/ptables/pagemap $max_regions)
    RET=$?

    # Check that CONTIG is not just whitespace or empty
    if [[ -z "${CONTIG// }" ]]; then
        # If SIGSEGV, then process just hasn't started or has exited
        if [ $RET -eq 139 ]; then
            sleep 1
        else
            echo "$pid: $full_process_name has exited" 1>&2
            break
        fi
    else
        # Check if CONTIG has changed
        echo "$TIME,$CONTIG"
    fi
    sleep 30
done

echo "$pid: $full_process_name is no longer running, exiting" 1>&2
