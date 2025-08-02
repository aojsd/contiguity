#!/bin/bash
# Take pid, process name
if [ $# -lt 2 ]; then
    echo "Usage: $0 <pid> <process_name>"
    exit 1
fi
TMP_DIR=/home/michael/ISCA_2025_results/tmp
CONT_DIR=/home/michael/ISCA_2025_results/contiguity

# Args
pid="$1"
process_name="$2"

# ================================================================
# Live profilers - Interrupted in contiguity_trials.sh (SIGINT)
# ================================================================
# Set the target pid for the sleep_dilation module
echo ${pid} | sudo tee /sys/kernel/sleep_dilation/target_pid 1>&2

# Profile kernel thread activity
sudo ${CONT_DIR}/kernel_work/kthread_cputime.bt > ${TMP_DIR}/${process_name}.kthread_cputime &

# Profile system calls
sudo ${CONT_DIR}/kernel_work/pid_syscall_profiler.bt ${pid} > ${TMP_DIR}/${process_name}.syscalls &

# Attach perf to the whole process
sudo perf stat -e instructions,L1-dcache-loads,L1-dcache-stores -p ${pid} -a &> ${TMP_DIR}/${process_name}.perf &

# Profile threads of the process
#  - If name contains 'pr', wait for 20s so all threads can start
if [[ "$process_name" == *"pr"* ]]; then
    sleep 20
fi
EVENTS="instructions,L1-dcache-loads,L1-dcache-stores"
sudo perf stat --per-thread -e ${EVENTS} -p ${pid} -a &> ${TMP_DIR}/${process_name}_threads.perf &