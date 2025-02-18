#!/bin/bash
# Take process name, waits for a process containing this name has over 50% CPU usage
if [ $# -lt 1 ]; then
    echo "Usage: $0 <process_name> [initial sleep]"
    exit 1
fi

# Name to match
process_name="$1"
cpu_threshold=50.0  # Define the minimum CPU usage percentage

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

# Fields: Time, n_regions, r75, r50, r25, Tracked RSS, Total RSS, n_mappings, list_mappings
DIR=/home/michael/ISCA_2025_results/contiguity/
echo "Time,Tracked-RSS,Total-RSS,n_mappings,64K,128K,256K,512K,1M,2M,4M,8M,16M,32M,64M,128M,256M,512M,1G"
# echo "Time   regions r75 r50 r25 Tracked-RSS Total-RSS n_mappings list_mappings"

# Initial sleep, default 5s
if [ $# -eq 2 ]; then
    sleep $2
else
    sleep 5
fi

# Loop until the process with the given PID is no longer running
while ps -p $pid > /dev/null; do
    PTIME=$(ps -p $pid -o etime=)
    TIME=$(python3 $DIR/parse_time.py $PTIME)
    CONTIG=$(sudo pmap -x $pid | sudo nice -n -20 $DIR/dump_pagemap $pid)
    RET=$?

    # Check that CONTIG is not just whitespace or empty
    if [[ -z "${CONTIG// }" ]]; then
        # If SIGSEGV, then process just hasn't started
        if [ $RET -eq 139 ]; then
            # echo "$TIME   0 0 0 0 0GB 0GB 0 0"
            sleep 1
        else
            echo "$pid: $full_process_name has exited" 1>&2
            break
        fi
    else
        echo "$TIME,$CONTIG"
    fi
    sleep 30
done

echo "$pid: $full_process_name is no longer running, exiting" 1>&2
