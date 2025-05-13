#!/bin/bash
# Take process name, waits for a process containing this name has over 50% CPU usage
if [ $# -lt 1 ]; then
    echo "Usage: $0 <process_name> [max_regions]"
    exit 1
fi
TMP_DIR=/home/michael/ISCA_2025_results/tmp

# Name to match
process_name="$1"
cpu_threshold=25.0  # Define the minimum CPU usage percentage

echo "Waiting for a process named '$process_name' with CPU usage over $cpu_threshold%..." 1>&2

while true; do
    # Use `ps` to list processes sorted by CPU, exclude this script and grep
    candidate=$(ps -eo pid,comm,%cpu,%mem --sort=-%cpu | grep -E "^\s*[0-9]+ $process_name" | head -n 1)
    
    if [[ -n "$candidate" ]]; then
        # Extract PID, CPU, and memory
        pid=$(echo "$candidate" | awk '{print $1}')
        cpu=$(echo "$candidate" | awk '{print $3}')
        mem=$(echo "$candidate" | awk '{print $4}')

        # Check if the CPU usage exceeds the threshold
        full_process_name=$(echo "$candidate" | awk '{print $2}')
        if (( $(echo "$cpu > $cpu_threshold" | bc -l) )); then
            echo "Found process '$full_process_name' (PID: $pid) with CPU: $cpu% and Memory: $mem%." 1>&2
            break
        fi
    fi

    # Sleep briefly before checking again
    sleep 0.5
done

# Found process
echo "Monitoring contiguity of process $pid (name: $full_process_name)..." 1>&2
sudo perf stat -e instructions,L1-dcache-loads,L1-dcache-stores -p ${pid} -a &> ${TMP_DIR}/${process_name}.perf &

# Fields: Time, n_regions, r75, r50, r25, Tracked RSS, Total RSS, n_mappings, list_mappings
DIR=/home/michael/ISCA_2025_results/contiguity/
echo "Time,Tracked-VSize,Tracked-RSS,Total-RSS,n_mappings,4K,8K,16K,32K,64K,128K,256K,512K,1M,2M,4M,8M,16M,32M,64M,128M,256M,512M,1G"

# Initial sleep, default 5s
sleep 5

# Loop until the process with the given PID is no longer running
mkdir -p ${TMP_DIR}/ptables
while ps -p $pid > /dev/null; do
    PTIME=$(ps -p $pid -o etime=)
    TIME=$(python3 $DIR/src/python/parse_time.py $PTIME)
    CONTIG=$(sudo pmap -x $pid | sudo nice -n -20 $DIR/dump_pagemap $pid ${TMP_DIR}/ptables/pagemap $2)
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
        if [ "$CONTIG" != "$PREV_CONTIG" ]; then
            echo "$TIME,$CONTIG"
            PREV_CONTIG=$CONTIG
            mv ${TMP_DIR}/ptables/pagemap ${TMP_DIR}/ptables/pagemap_${TIME}.txt
        else
            rm ${TMP_DIR}/ptables/pagemap
        fi
    fi
    sleep 30
done

echo "$pid: $full_process_name is no longer running, exiting" 1>&2
