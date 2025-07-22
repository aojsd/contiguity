#!/bin/bash

# Get the absolute path to the directory where this script is located.
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)

# Source the shared configuration file using the absolute path.
source "${SCRIPT_DIR}/.arg_parsing.sh"

# Remote directory for contiguity experiments
CONTIGUITY="/home/michael/ISCA_2025_results/contiguity"

# ==========================================================================================================
# Script Functions
# ==========================================================================================================

# Constructs and runs the remote application command, capturing its PID.
# Arguments:
#   $1: Remote host
#   $2: Application name (e.g., "memcached-12T")
#   $3: Application log file path on remote
#   $4...: Pin arguments
# Outputs:
#   The PID of the remotely executed application.
run_and_capture_pid() {
    local remote_host="$1"
    local app_name="$2"
    local remote_log_file="$3"
    shift 3
    local pin_args="$@"

    local threads=6
    if [[ $app_name =~ -([0-9]+)T$ ]]; then
        threads="${BASH_REMATCH[1]}"
        echo "Threads: ${threads}"
    fi

    local app_cmd
    case "$app_name" in
        *llama*)
            local llama_root="/home/michael/software/llama.cpp"
            app_cmd="${llama_root}/build/bin/llama-cli -n 500 -s 0 --no-mmap -t ${threads} -m ${llama_root}/models/llama-2-7b-chat.Q5_K_M.gguf"
            ;;
        *pr*)
            export OMP_NUM_THREADS=${threads}
            local pr_root="/home/michael/software/gapbs"
            app_cmd="${pr_root}/pr -f ${pr_root}/benchmark/graphs/twitter.sg -n 1"
            ;;
        *memcached*)
            app_cmd="/home/michael/software/memcached/memcached -p 11211 -m 16384 -t ${threads} -u michael"
            ;;
        *)
            echo "Error: Invalid application '$app_name'" >&2
            return 1
            ;;
    esac

    local pin="/home/michael/software/PiTracer/pin"
    local pt="/home/michael/software/PiTracer/source/tools/PiTracer/obj-intel64/pitracer.so"
    
    local final_cmd_to_run
    if [ -z "$pin_args" ]; then
        echo "--- ${remote_host}: Running application natively ---" >&2
        final_cmd_to_run="${CG} /usr/bin/time -v ${app_cmd}"
    else
        echo "--- ${remote_host}: Running application with Pin args: ${pin_args} ---" >&2
        local pin="/home/michael/software/PiTracer/pin"
        local pt="/home/michael/software/PiTracer/source/tools/PiTracer/obj-intel64/pitracer.so"
        final_cmd_to_run="${CG} /usr/bin/time -v ${pin} -t ${pt} ${pin_args} -- ${app_cmd}"
    fi

    # This command correctly handles the 'tee' pipeline and the 'time' fork.
    local remote_command="
        # Start the command in a backgrounded subshell.
        { ${final_cmd_to_run}; } > \"${remote_log_file}\" 2>&1 &

        # Capture the PID of the subshell process.
        SUBSHELL_PID=\$!
        disown \$SUBSHELL_PID

        # Step 1: Find the child of the subshell, which is the 'time' process.
        TIME_PID=\$(pgrep -P \$SUBSHELL_PID)
        while [ -z \"\$TIME_PID\" ]; do
            sleep 0.01
            TIME_PID=\$(pgrep -P \$SUBSHELL_PID)
        done

        # Step 2: Find the child of the 'time' process, which is the actual application.
        APP_PID=\$(pgrep -P \$TIME_PID)
        while [ -z \"\$APP_PID\" ]; do
            sleep 0.01
            APP_PID=\$(pgrep -P \$TIME_PID)
        done

        # Echo the final, correct application PID.
        echo \$APP_PID
    "
    
    # Execute and capture the clean PID output.
    ssh -n -T "${remote_host}" "${remote_command}"
}

# Prepares the remote system for a single benchmark trial.
# Arguments:
#   $1: Remote host
#   $2: Current trial number
prepare_remote_system() {
    local remote_host="$1"
    local trial_num="$2"
    
    echo "--- Preparing remote system for trial ${trial_num} ---"

    # --- Step 1: Pre-Reboot Cleanup ---
    # Build and run the commands that must happen before a reboot.
    local pre_reboot_cmds=""
    pre_reboot_cmds+="rm -rf /home/michael/ssd/scratch/*; "
    pre_reboot_cmds+="rm -rf /home/michael/ISCA_2025_results/tmp/*; "
    pre_reboot_cmds+="mkdir -p /home/michael/ssd/scratch/${APP}_tmp/;"
    
    echo "Cleaning up remote directories..."
    ssh "${remote_host}" "${pre_reboot_cmds}"

    # --- Step 2: Reboot (if enabled) ---
    # This remains a separate, blocking command.
    if [ "$NO_REBOOT" != "1" ]; then
        echo "Rebooting ${remote_host}..."
        ssh "${remote_host}" "sudo reboot"
        echo "Waiting 60 seconds for reboot..."
        sleep 60
    fi

    # --- Step 3: Build and Run Post-Reboot Commands ---
    # Create a string of all setup commands to run in a single SSH session.
    # This makes it easy to add, remove, or comment out individual steps.
    echo "Applying fragmentation, kernel, and system settings..."
    local post_reboot_cmds="set -ex;" # Start with set -ex for debugging and exit-on-error

    # Fragment system if requested
    local random_freelist_arg="0" # Default to 0 (no random freelist)
    if [ "$FRAGMENT" != "0" ]; then
        random_freelist_arg="1" # Fragmentation implies random freelist
        post_reboot_cmds+="cd ${CONTIGUITY}; ./kern_fragment.sh 1 ${FRAGMENT} 100; ./kern_fragment.sh 0; "
    fi
    post_reboot_cmds+="${CONTIGUITY}/random_freelist.sh ${random_freelist_arg}; "

    # Apply system settings
    if [ "$THP" == "1" ]; then
        local thp_dir="/sys/kernel/mm/transparent_hugepage"
        post_reboot_cmds+="sudo sh -c 'echo always > ${thp_dir}/enabled'; "
        post_reboot_cmds+="sudo sh -c 'echo ${THP_SCAN} > ${thp_dir}/khugepaged/pages_to_scan'; "
        post_reboot_cmds+="sudo sh -c 'echo ${THP_SLEEP} > ${thp_dir}/khugepaged/scan_sleep_millisecs'; "
    fi
    if [ "$DIRTY_BYTES" != "0" ]; then
        post_reboot_cmds+="sudo sh -c 'echo ${DIRTY_BYTES} > /proc/sys/vm/dirty_bytes'; "
        post_reboot_cmds+="sudo sh -c 'echo $((DIRTY_BYTES >> 1)) > /proc/sys/vm/dirty_background_bytes'; "
    fi
    if [ "$ZERO_COMPACT" == "1" ]; then
        post_reboot_cmds+="sudo sh -c 'echo 0 > /proc/sys/vm/compaction_proactiveness'; "
    fi
    if [ "$NO_COMPACT" == "1" ]; then
        post_reboot_cmds+="sudo sh -c 'echo never > /sys/kernel/mm/transparent_hugepage/defrag'; "
    fi
    if [ "$TIME_DILATION" != "0" ]; then
        post_reboot_cmds+="echo ${TIME_DILATION} | sudo tee /proc/sys/time_dilation/time_dilation > /dev/null; "
    fi
    post_reboot_cmds+="sudo mkdir -p /sys/fs/cgroup/pin; "
    post_reboot_cmds+="sudo chmod o+w /sys/fs/cgroup/cgroup.procs /sys/fs/cgroup/pin/cgroup.procs; "

    # Apply other system-wide settings
    post_reboot_cmds+="sudo sh -c 'echo 0 > /proc/sys/kernel/nmi_watchdog'; "
    post_reboot_cmds+="sudo sh -c 'echo 0 > /proc/sys/vm/swappiness'; "
    post_reboot_cmds+="sudo swapoff -a; "
    post_reboot_cmds+="sudo sh -c 'echo off > /sys/devices/system/cpu/smt/control'; "
    post_reboot_cmds+="sudo chown michael /dev/hugepages; "
    post_reboot_cmds+="sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'; "
    post_reboot_cmds+="echo 0 | sudo tee /proc/sys/kernel/randomize_va_space > /dev/null; "

    # Insert sleep dilation kernel module
    post_reboot_cmds+="sudo insmod ${CONTIGUITY}/kernel_work/sleep_dilation/sleep_dilation.ko; "

    # Drop caches right before the run
    post_reboot_cmds+="sudo /home/michael/ssd/drop_cache.sh;"

    # Execute the entire command string in one go
    ssh "${remote_host}" "${post_reboot_cmds}"

    # --- Step 4: Local Variable and Background Process ---
    # These still run locally or separately.
    CG="cgexec -g cpu:pin"
    
    if [ "$NOCACHE" == "1" ]; then
        local c_dir="/home/michael/software/PiTracer/source/tools/PiTracer/consumer"
        ssh "${remote_host}" "${c_dir}/consumer ${OUTP_DIR} ${CONSUMER_ZSTD}" &
    fi
}


# ==========================================================================================================
# Main Script Logic
# ==========================================================================================================

# Check for help flag
for arg in "$@"; do
    if [[ "$arg" == "-h" || "$arg" == "--help" ]]; then
        usage "" "full"
        exit 0
    fi
done

# --- Parse Command-Line Arguments ---
if [ "$#" -lt 5 ]; then
    usage "contiguity_trials" "brief"
    exit 1
fi

# --- Parse Positional Arguments ---
NUM_TRIALS=$1
REMOTE_HOST=$2
APP_NAME=$3
OUTPUT_DIR=$4
PIN_MODE=$5
shift 5 # Consume the positional arguments

# --- Parse Optional Arguments ---
parse_trial_args "$@"
if [ $? -eq 1 ]; then
    usage "contiguity_trials" "full"
    exit 0
fi

# --- Initial Setup ---
DIRTY_BYTES=$((DIRTY * 4096))
CURR_DIR=$SCRIPT_DIR

# Set output directories to be relative unless OUTPUT_DIR is absolute
if [[ "$OUTPUT_DIR" == /* ]]; then
    OUTDIR=$(realpath "$OUTPUT_DIR")
else
    OUTDIR="${CURR_DIR}/${OUTPUT_DIR}/${APP_NAME}/${PIN_MODE}"
fi
APP_OUT_DIR="${OUTDIR}/app"
DIST_OUT_DIR="${OUTDIR}/dist"
THP_DIR="${OUTDIR}/thp"
PTABLE_DIR="${OUTDIR}/ptables"
SYS_DIR="${OUTDIR}/sys"
mkdir -p "$APP_OUT_DIR" "$DIST_OUT_DIR" "$THP_DIR" "$SYS_DIR" "$PTABLE_DIR"


# --- Application and Pin Mode Specific Setup ---
if [[ $APP_NAME =~ ^(.+)-([0-9]+)T$ ]]; then
    NAME="${BASH_REMATCH[1]}"
    THREADS="${BASH_REMATCH[2]}"
else
    NAME=$APP_NAME
fi

if [[ $APP_NAME == mem* ]]; then
    case "$APP_NAME" in
        memA*)  WORKLOAD="workloada";;
        memB*)  WORKLOAD="workloadb";;
        memC*)  WORKLOAD="workloadc";;
        memW*)  WORKLOAD="workloadw";;
        memDY*) WORKLOAD="workload_dynamic";;
        *)      echo "Invalid memcached workload: $APP_NAME"; exit 1;;
    esac

    APP="memcached"
    if [ -n "$THREADS" ]; then
        APP="memcached-${THREADS}T"
    fi
    NAME="memcached"
else
    APP=$APP_NAME
fi

# YCSB commands (if needed)
TARGET_IP=$(ssh -G "${REMOTE_HOST}" | awk '/^hostname/ { print $2 }')
YCSB_ROOT=/home/michael/software/YCSB
YCSB_HOST_ARGS="-p memcached.hosts=${TARGET_IP} -p memcached.port=11211 -p memcached.opTimeoutMillis=1000000"
YCSB_ARGS="-s -threads 12 -p hdrhistogram.percentiles=90,99,99.9,99.99 ${YCSB_HOST_ARGS}"
YCSB_LOAD="python2 ${YCSB_ROOT}/bin/ycsb load memcached -P ${YCSB_ROOT}/workloads/${WORKLOAD} ${YCSB_ARGS}"
YCSB_RUN="python2 ${YCSB_ROOT}/bin/ycsb run memcached -P ${YCSB_ROOT}/workloads/${WORKLOAD} ${YCSB_ARGS}"

# Pin Arguments
OUTPREFIX="/home/michael/ssd/scratch/${APP}_tmp/${APP}"
OUTP_DIR="/home/michael/ssd/scratch/${APP}_tmp"
PIN_ARGS=""
DIST_FILE=""
if [ "$DIST" == "1" ]; then
    DIST_FILE="-record_file /home/michael/ISCA_2025_results/tmp/${APP}.dist"
fi

case "$PIN_MODE" in
    native)
        PIN_ARGS=""
        ;;
    empty)
        PIN_ARGS="-stage1 0 -bpages 16 ${PIN_EXTRA} ${DIST_FILE}"
        ;;
    empty-sleep)
        PIN_ARGS="-stage1 0 -bpages 16 -iosleep 1 ${PIN_EXTRA} ${DIST_FILE}"
        ;;
    disk)
        PIN_ARGS="-stage1 0 -bpages 16 -index_limit 20000 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
        ;;
    disk-nocache)
        PIN_ARGS="-comp1 -1 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
        NOCACHE=1
        ;;
    disk-largebuf)
        PIN_ARGS="-stage1 0 -outprefix ${OUTPREFIX} -index_limit 200 ${PIN_EXTRA} ${DIST_FILE}"
        ;;
    struct)
        PIN_ARGS="-buf_type 0 -stage1 0 -comp1 3 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
        ;;
    fields|fields-sync)
        PIN_ARGS="-stage1 0 -comp1 3 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
        ;;
    fields-sync)
        PIN_ARGS="-fsync 1 -stage1 0 -comp1 3 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
        ;;
    pitracer)
        PIN_ARGS="-comp1 -1 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
        NOCACHE=1
        CONSUMER_ZSTD=1
        ;;
    *)
        echo "Invalid Pin mode: $PIN_MODE"
        exit 1
        ;;
esac

# ==========================================================================================================
# Run Trials
# ==========================================================================================================
for i in $(seq 1 "$NUM_TRIALS"); do
    echo "========================= Trial $i of ${NUM_TRIALS}: ${APP_NAME} (${PIN_MODE}) ========================="

    # 1. PREPARE the remote system
    prepare_remote_system "$REMOTE_HOST" "$i"

    # 2. RUN the benchmark and profilers
    echo "--- Starting benchmark and profilers ---"
    REGIONS=""
    if [ "$TRACK_PIN" == "1" ]; then
        REGIONS="-1"
    fi

    # Define the remote log file path for the application
    REMOTE_APP_LOG="/home/michael/ISCA_2025_results/tmp/${APP}.out"
    
    # Call the new function to run the app and capture its PID
    # The PIN_ARGS variable is already set by the script's setup logic
    APP_PID=$(run_and_capture_pid "$REMOTE_HOST" "$APP" "$REMOTE_APP_LOG" $PIN_ARGS)
    echo "--- Application started remotely with PID: ${APP_PID} ---"

    # Tail the remote log file in the background to monitor the application output
    # Output will appear on your local terminal.
    ssh -n -T "${REMOTE_HOST}" "tail -f --pid=${APP_PID} ${REMOTE_APP_LOG}" > /dev/null &
    TAIL_PID=$! # Capture PID of the local 'tail' process.

    ssh "${REMOTE_HOST}" "cd ${CONTIGUITY}; ./loop.sh ${APP_PID} ${NAME} ${REGIONS} > /home/michael/ISCA_2025_results/tmp/${PIN_MODE}.txt" &
    if [[ $APP_NAME == mem* ]]; then
        # --- Memcached Path ---
        if [ "$CPU_LIMIT" != "0" ]; then
            sleep 5
            ssh "${REMOTE_HOST}" "echo '${MAX_CPU} 100000' | sudo tee /sys/fs/cgroup/pin/cpu.max"
        fi
        
        cd "$YCSB_ROOT"
        $YCSB_LOAD 2>&1 | grep -e "\[OVERALL\]" -e "\[READ\]" -e "\[UPDATE\]" -e "\[INSERT\]" -e "FAILED\]" | tee "$APP_OUT_DIR/load_$i.out"
        ssh "${REMOTE_HOST}" "cat /proc/vmstat" > "$THP_DIR/vmstat_loaded_${APP}_$i.txt"
        $YCSB_RUN 2>&1 | grep -e "\[OVERALL\]" -e "\[READ\]" -e "\[UPDATE\]" -e "\[INSERT\]" -e "FAILED\]" | tee "$APP_OUT_DIR/run_$i.out"
        cd - > /dev/null
    else
        # --- Generic Application Path ---
        if [ "$CPU_LIMIT" != "0" ]; then
            sleep 5
            ssh "${REMOTE_HOST}" "echo '${MAX_CPU} 100000' | sudo tee /sys/fs/cgroup/pin/cpu.max"
        fi

        if [[ $PIN_MODE == *"disk"* ]] || [[ $PIN_MODE == *"fields"* ]] || [[ $PIN_MODE == *"sleep"* ]]; then
            sleep 300
        else
            sleep 10
        fi
        ssh "${REMOTE_HOST}" "cat /proc/vmstat" > "$THP_DIR/vmstat_loaded_${APP}_$i.txt"
        
        # This command blocks until the remote process with APP_PID finishes.
        wait ${TAIL_PID}
    fi
    ssh "${REMOTE_HOST}" "cat /proc/vmstat" > "$THP_DIR/vmstat_${APP}_$i.txt"
    echo "--- Benchmark finished ---"

    # 3. CLEAN UP remote processes
    echo "--- Halting remote processes ---"
    if [[ $APP_NAME == mem* ]]; then
        ssh "${REMOTE_HOST}" "sudo pkill -2 -f /home/michael/software/memcached/memcached"
    fi
    ssh "${REMOTE_HOST}" "sudo pkill -2 -f perf"
    ssh "${REMOTE_HOST}" "sudo pkill -2 -f kthread_cputime.bt"
    ssh "${REMOTE_HOST}" "sudo pkill -2 -f pid_syscall_profiler.bt"
    wait $(jobs -p) # Wait for all background jobs (ssh <host> loop.sh) to finish

    if [ "$CPU_LIMIT" != "0" ]; then
        ssh "${REMOTE_HOST}" "echo '100000 100000' | sudo tee /sys/fs/cgroup/pin/cpu.max"
    fi
    if [ "$TIME_DILATION" != "0" ]; then
        ssh "${REMOTE_HOST}" "sudo sh -c 'echo 0 > /proc/sys/time_dilation/time_dilation'" > /dev/null
    fi

    # 4. COLLECT & PROCESS results
    echo "--- Collecting and processing results ---"
    P_SRC_CONTIG="${CONTIGUITY}/src/python"
    ssh "${REMOTE_HOST}" "python3 ${P_SRC_CONTIG}/check_ptables.py /home/michael/ISCA_2025_results/tmp/ptables >> /home/michael/ISCA_2025_results/tmp/${NAME}.perf"
    
    ssh "${REMOTE_HOST}" "echo -n 'Trace directory size: '; du -sh /home/michael/ssd/scratch/${APP}_tmp/"
    
    TMP_DIR="/home/michael/ISCA_2025_results/tmp"
    scp "${REMOTE_HOST}:${TMP_DIR}/${PIN_MODE}.txt"              "${OUTDIR}/${APP}_${PIN_MODE}_${i}.txt"
    scp "${REMOTE_HOST}:${TMP_DIR}/${APP}.out"                   "${APP_OUT_DIR}/${APP}_${PIN_MODE}_${i}.out"
    scp "${REMOTE_HOST}:${TMP_DIR}/${NAME}.perf"                 "${APP_OUT_DIR}/${APP}_${PIN_MODE}_${i}.perf"
    scp "${REMOTE_HOST}:${TMP_DIR}/${NAME}.kthread_cputime"      "${SYS_DIR}/${APP}_${PIN_MODE}_${i}.kthread_cputime"
    scp "${REMOTE_HOST}:${TMP_DIR}/${NAME}.syscalls"             "${SYS_DIR}/${APP}_${PIN_MODE}_${i}.syscalls"
    if [ "$DIST" == "1" ]; then
        scp "${REMOTE_HOST}:${TMP_DIR}/${APP}.dist" "${DIST_OUT_DIR}/${APP}_${PIN_MODE}_dist_${i}.txt"
    fi
    rm -rf "${OUTDIR}/ptables_${i}"
    ssh "${REMOTE_HOST}" "rm -f ${TMP_DIR}/ptables/pagemap"
    scp -r "${REMOTE_HOST}:${TMP_DIR}/ptables" "${PTABLE_DIR}/ptables_${i}"

    # Cleanup
    ssh "${REMOTE_HOST}" "rm -rf ${TMP_DIR}/*"
    ssh "${REMOTE_HOST}" "rm -rf /home/michael/ssd/scratch/${APP}_tmp"

    # Process the results
    P_SRC_KWORK="${CONTIGUITY}/kernel_work/python"
    python3 "${P_SRC_KWORK}/kthread_parse.py" "${SYS_DIR}/${APP}_${PIN_MODE}_${i}.kthread_cputime"
    python3 "${P_SRC_KWORK}/syscall_parse.py" "${SYS_DIR}/${APP}_${PIN_MODE}_${i}.syscalls"
done

echo "========================= All trials completed. ========================="