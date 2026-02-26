#!/bin/bash
# Take pid, process name
if [ $# -lt 3 ]; then
    echo "Usage: $0 <pid> <process_name> <pin_mode>"
    exit 1
fi
TMP_DIR=/home/michael/ISCA_2025_results/tmp
CONT_DIR=/home/michael/ISCA_2025_results/contiguity

# Args
pid="$1"
process_name="$2"
pin_mode="$3"

# ================================================================
# Live profilers - Interrupted in contiguity_trials.sh (SIGINT)
# ================================================================
# Set the target pid for the sleep_dilation module
echo ${pid} | sudo tee /sys/kernel/sleep_dilation/target_pid 1>&2

# Profile kernel thread activity
sudo ${CONT_DIR}/kernel_work/kthread_cputime.bt > ${TMP_DIR}/${process_name}.kthread_cputime &

# Profile system calls
sudo ${CONT_DIR}/kernel_work/pid_syscall_profiler.bt ${pid} > ${TMP_DIR}/${process_name}.syscalls &

# Profile page faults vs khugepaged collapse/scan activity
sudo ${CONT_DIR}/kernel_work/fault_vs_khugepaged.bt ${pid} > ${TMP_DIR}/${process_name}.fault_khugepaged &

# Only attach perf if the pin_mode is "native"
if [ "${pin_mode}" == "native" ]; then
    EVENTS="instructions,L1-dcache-loads,L1-dcache-stores,rob_misc_events.pause_inst"
    sudo perf stat -e ${EVENTS} -p ${pid} -a &> ${TMP_DIR}/${process_name}.perf &
else
    touch ${TMP_DIR}/${process_name}.perf
fi
