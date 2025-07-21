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

# ================================================================
# Live profilers - Interrupted in contiguity_trials.sh (SIGINT)
# ================================================================
# Set the target pid for the sleep_dilation module
echo ${pid} | sudo tee /sys/kernel/sleep_dilation/target_pid

# Attach perf to the process
sudo perf stat -e instructions,L1-dcache-loads,L1-dcache-stores -p ${pid} -a &> ${TMP_DIR}/${process_name}.perf &

# Profile kernel thread activity
sudo ${CONT_DIR}/kernel_work/kthread_cputime.bt > ${TMP_DIR}/${process_name}.kthread_cputime &

# Track syscall activity
sudo ${CONT_DIR}/kernel_work/pid_syscall_profiler.bt ${pid} > ${TMP_DIR}/${process_name}.syscalls &


# =============================================
# Contiguity tracking
# =============================================
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
    CONTIG=$(sudo pmap -x $pid | sudo nice -n -20 $DIR/dump_pagemap $pid ${TMP_DIR}/ptables/pagemap $max_regions)
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
