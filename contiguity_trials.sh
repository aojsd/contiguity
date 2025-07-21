#!/bin/bash

# Get the absolute path to the directory where this script is located.
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)

# Source the shared configuration file using the absolute path.
source "${SCRIPT_DIR}/.arg_parsing.sh"

# ==========================================================================================================
# Script Functions
# ==========================================================================================================

# Prepares the remote system for a single benchmark trial.
# Arguments:
#   $1: Remote host
#   $2: Current trial number
prepare_remote_system() {
    local remote_host="$1"
    local trial_num="$2"
    
    echo "--- Preparing remote system for trial ${trial_num} ---"

    # Clear previous run data and create directories for the current trial
    ssh "${remote_host}" "rm -rf /home/michael/ssd/scratch/*"
    ssh "${remote_host}" "rm -rf /home/michael/ISCA_2025_results/tmp/*"
    ssh "${remote_host}" "mkdir -p /home/michael/ssd/scratch/${APP}_tmp/"

    # Reboot machine for a clean slate, unless disabled
    if [ "$NO_REBOOT" != "1" ]; then
        echo "Rebooting ${remote_host}..."
        ssh "${remote_host}" "sudo reboot"
        sleep 60
    fi

    # Fragment system if requested
    if [ "$FRAGMENT" != "0" ]; then
        echo "Generating ${FRAGMENT}GB of memory fragmentation..."
        RANDOM_FREELIST=1 # Fragmentation implies random freelist
        ssh "${remote_host}" "cd /home/michael/ISCA_2025_results/contiguity; ./kern_fragment.sh 1 ${FRAGMENT} 100"
        ssh "${remote_host}" "cd /home/michael/ISCA_2025_results/contiguity; ./kern_fragment.sh 0"
    fi
    ssh "${remote_host}" "ISCA_2025_results/contiguity/random_freelist.sh ${RANDOM_FREELIST}"

    # Apply system settings
    echo "Applying kernel and system settings..."
    if [ "$THP" == "1" ]; then
        local thp_dir="/sys/kernel/mm/transparent_hugepage"
        ssh "${remote_host}" "sudo sh -c 'echo always > ${thp_dir}/enabled'"
        ssh "${remote_host}" "sudo sh -c 'echo ${THP_SCAN} > ${thp_dir}/khugepaged/pages_to_scan'"
        ssh "${remote_host}" "sudo sh -c 'echo ${THP_SLEEP} > ${thp_dir}/khugepaged/scan_sleep_millisecs'"
    fi
    if [ "$DIRTY_BYTES" != "0" ]; then
        ssh "${remote_host}" "sudo sh -c 'echo ${DIRTY_BYTES} > /proc/sys/vm/dirty_bytes'"
        ssh "${remote_host}" "sudo sh -c 'echo $((DIRTY_BYTES >> 1)) > /proc/sys/vm/dirty_background_bytes'"
    fi
    if [ "$ZERO_COMPACT" == "1" ]; then
        ssh "${remote_host}" "sudo sh -c 'echo 0 > /proc/sys/vm/compaction_proactiveness'"
    fi
    if [ "$NO_COMPACT" == "1" ]; then
        ssh "${remote_host}" "sudo sh -c 'echo never > /sys/kernel/mm/transparent_hugepage/defrag'"
    fi

    # Configure CPU limits via cgroup
    CG=""
    if [ "$CPU_LIMIT" != "0" ]; then
        MAX_CPU=$(echo "$CPU_LIMIT 1000" | awk '{print $1 * $2}')
        ssh "${remote_host}" "sudo mkdir -p /sys/fs/cgroup/pin"
        ssh "${remote_host}" "sudo chmod o+w /sys/fs/cgroup/cgroup.procs /sys/fs/cgroup/pin/cgroup.procs"
        CG="cgexec -g cpu:pin"
    fi
    if [ "$TIME_DILATION" != "0" ]; then
        ssh "${remote_host}" "echo ${TIME_DILATION} | sudo tee /proc/sys/time_dilation/time_dilation" > /dev/null
    fi
    
    # Apply other system-wide settings
    ssh "${remote_host}" "sudo sh -c 'echo 0 > /proc/sys/kernel/nmi_watchdog'"
    ssh "${remote_host}" "sudo sh -c 'echo 0 > /proc/sys/vm/swappiness'"
    ssh "${remote_host}" "sudo swapoff -a"
    ssh "${remote_host}" "sudo sh -c 'echo off > /sys/devices/system/cpu/smt/control'"
    ssh "${remote_host}" "sudo chown michael /dev/hugepages"
    ssh "${remote_host}" "sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'"

    # Start the consumer process for pitracer/nocache modes
    if [ "$NOCACHE" == "1" ]; then
        local c_dir="/home/michael/software/PiTracer/source/tools/PiTracer/consumer"
        ssh "${remote_host}" "${c_dir}/consumer ${OUTP_DIR} ${CONSUMER_ZSTD}" &
    fi
    
    # Drop caches right before the run
    ssh "${remote_host}" "sudo /home/michael/ssd/drop_cache.sh"
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
CURR_DIR=$(pwd)
OUTDIR="${CURR_DIR}/${OUTPUT_DIR}/${APP_NAME}/${PIN_MODE}"
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

# Copy run_pin.sh to remote machine
scp /home/michael/ISCA_2025_results/run_pin.sh "${REMOTE_HOST}:/home/michael/ISCA_2025_results/"
ssh "${REMOTE_HOST}" "chmod +x /home/michael/ISCA_2025_results/run_pin.sh"

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
    ssh "${REMOTE_HOST}" "cd /home/michael/ISCA_2025_results/contiguity; ./loop.sh ${NAME} ${REGIONS} > /home/michael/ISCA_2025_results/tmp/${PIN_MODE}.txt" &
    if [[ $APP_NAME == mem* ]]; then
        # --- Memcached Path ---
        ssh "${REMOTE_HOST}" "cd /home/michael/ISCA_2025_results; ${CG} ./run_pin.sh ${APP} ${PIN_ARGS}" &
        
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
        ssh "${REMOTE_HOST}" "cd /home/michael/ISCA_2025_results; ${CG} ./run_pin.sh ${APP} ${PIN_ARGS}" &
        PIN_PID=$!

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
        
        wait $PIN_PID
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
    wait $(jobs -p) # Wait for all local background jobs (like ssh) to finish

    if [ "$CPU_LIMIT" != "0" ]; then
        ssh "${REMOTE_HOST}" "echo '100000 100000' | sudo tee /sys/fs/cgroup/pin/cpu.max"
    fi
    if [ "$TIME_DILATION" != "0" ]; then
        ssh "${REMOTE_HOST}" "sudo sh -c 'echo 0 > /proc/sys/time_dilation/time_dilation'" > /dev/null
    fi

    # 4. COLLECT & PROCESS results
    echo "--- Collecting and processing results ---"
    P_SRC_CONTIG="/home/michael/ISCA_2025_results/contiguity/src/python"
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
    ssh "${REMOTE_HOST}" "rm -f /home/michael/ISCA_2025_results/tmp/ptables/pagemap"
    scp -r "${REMOTE_HOST}:/home/michael/ISCA_2025_results/tmp/ptables" "${PTABLE_DIR}/ptables_${i}"
    
    P_SRC_KWORK="/home/michael/ISCA_2025_results/contiguity/kernel_work/python"
    python3 "${P_SRC_KWORK}/kthread_parse.py" "${SYS_DIR}/${APP}_${PIN_MODE}_${i}.kthread_cputime"
    python3 "${P_SRC_KWORK}/syscall_parse.py" "${SYS_DIR}/${APP}_${PIN_MODE}_${i}.syscalls"

    # 5. FINAL cleanup for the trial
    ssh "${REMOTE_HOST}" "rm -rf /home/michael/ssd/scratch/${APP}_tmp"
done

echo "========================= All trials completed. ========================="