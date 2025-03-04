# Run trials of application using remote "run_pin.sh" script
# Usage: ./pin_trials.sh <num_trials> <remote_host> <app> <output_dir> <pin_mode> <Other>
if [ "$#" -lt 5 ]; then
    echo "Usage: ./pin_trials.sh <num_trials> <remote_host> <app> <output_dir> <pin_mode> <Other>"
    exit 1
fi

# Ouput subdir: <output_dir>/<app>/<pin_mode>
OUTDIR=$4/$3/$5
APP_OUT_DIR=$OUTDIR/app
DIST_OUT_DIR=$OUTDIR/dist
THP_DIR=$OUTDIR/thp
mkdir -p $APP_OUT_DIR
mkdir -p $DIST_OUT_DIR
mkdir -p $THP_DIR

# Parse extra arguments
eval "$(python3 bash_parser.py "${@:6}")"
echo "THP setting: ${THP}"
echo "THP pages per scan: ${THP_SCAN}"
echo "THP sleep timer: ${THP_SLEEP}"
echo "Dirty bytes setting (pages): ${DIRTY}"
echo "Dirty background bytes (pages): ${DIRTY_BG}"
echo "CPU usage limit: ${CPU_LIMIT}"
echo "Loop initial sleep time: ${LOOP_SLEEP}"
echo "Extra Pin arguments: ${PIN_EXTRA}"
echo "Output directory: ${OUTDIR}"

# if $DIST == 1
if [ "$DIST" == "1" ]; then
    echo "Collecting access distribution"
    DIST_FILE="-record_file /home/michael/ISCA_2025_results/tmp/${APP}.dist"
else
    DIST_FILE=""
fi

# Memcached sub-directories
mkdir -p $OUTDIR
if [ "$3" == "memA" ] || [ "$3" == "memB" ] || [ "$3" == "memC" ] || [ "$3" == "memW" ] || [ "$3" == "memDY" ]; then
    # Select memcached workload
    if [ "$3" == "memA" ]; then
        WORKLOAD="workloada"
        MEM="memcached"
    elif [ "$3" == "memB" ]; then
        WORKLOAD="workloadb"
        MEM="memcached"
    elif [ "$3" == "memC" ]; then
        WORKLOAD="workloadc"
        MEM="memcached"
    elif [ "$3" == "memW" ]; then
        WORKLOAD="workloadw"
        MEM="memcached"
    elif [ "$3" == "memDY" ]; then
        WORKLOAD="workload_dynamic"
        MEM="memcached"
    else
        echo "Invalid memcached workload: $3"
        exit 1
    fi
    APP="memcached"
else
    APP=$3
fi

# YCSB commands
TARGET_IP=$(ssh -G $2 | awk '/^hostname/ { print $2 }')
YCSB_ROOT=/home/michael/software/YCSB
YCSB_HOST_ARGS="-p memcached.hosts=${TARGET_IP} -p memcached.port=11211 -p memcached.opTimeoutMillis=100000"
YCSB_ARGS="-s -threads 12 -p hdrhistogram.percentiles=90,99,99.9,99.99 ${YCSB_HOST_ARGS}"
YCSB_LOAD="python2 ${YCSB_ROOT}/bin/ycsb load memcached -P ${YCSB_ROOT}/workloads/${WORKLOAD} ${YCSB_ARGS}"
YCSB_RUN="python2 ${YCSB_ROOT}/bin/ycsb run memcached -P ${YCSB_ROOT}/workloads/${WORKLOAD} ${YCSB_ARGS}"

# Copy run_pin.sh to remote machine
scp /home/michael/ISCA_2025_results/run_pin.sh $2:/home/michael/ISCA_2025_results/run_pin.sh
ssh $2 "chmod +x /home/michael/ISCA_2025_results/run_pin.sh"


# ==========================================================================================================
# Select Pin mode
# ==========================================================================================================
# Check for IO-sleep mode
if [[ $5 =~ ^([a-zA-Z]+)-([0-9]+)$ ]]; then
    NAME="${BASH_REMATCH[1]}"
    IOSLEEP="${BASH_REMATCH[2]}"
    IOSLEEP="-iosleep $IOSLEEP"
else
    NAME=$5
    IOSLEEP=""
fi

PIN_ARGS=""
if [ "$5" == "native" ]; then
    DIST_FILE=""
else
    DIST_FILE="${IOSLEEP} ${DIST_FILE}"
    if [ "$NAME" == "native" ]; then
        PIN_ARGS=""
    elif [ "$NAME" == "empty" ]; then
        PIN_ARGS="-stage1 0 -bpages 16 ${PIN_EXTRA} ${DIST_FILE}"
    elif [ "$NAME" == "disk" ]; then
        PIN_ARGS="-stage1 0 -bpages 16 -index_limit 200000 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${PIN_EXTRA} ${DIST_FILE}"
    elif [ "$NAME" == "disk-largebuf" ]; then
        PIN_ARGS="-stage1 0 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} -index_limit 200 ${PIN_EXTRA} ${DIST_FILE}"
    elif [ "$NAME" == "struct" ]; then
        PIN_ARGS="-stage1 0 -comp1 3 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${PIN_EXTRA} ${DIST_FILE}"
    elif [ "$NAME" == "fields" ]; then
        PIN_ARGS="-stage1 0 -comp1 3 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${PIN_EXTRA} ${DIST_FILE}"
    elif [ "$NAME" == "fields-empty" ]; then
        PIN_ARGS="-stage1 0 -comp1 3 ${PIN_EXTRA} ${DIST_FILE}"
    elif [ "$NAME" == "fields-threads" ]; then
        PIN_ARGS="-stage1 12 -comp1 3 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${PIN_EXTRA} ${DIST_FILE}"
    elif [ "$NAME" == "split" ]; then
        PIN_ARGS="-stage1 0 -comp1 4 -stage2 1 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${PIN_EXTRA} ${DIST_FILE}"
    elif [ "$NAME" == "split-empty" ]; then
        PIN_ARGS="-stage1 0 -comp1 4 -stage2 1 ${PIN_EXTRA} ${DIST_FILE}"
    elif [ "$NAME" == "pfor" ]; then
        PIN_ARGS="-stage1 0 -comp1 4 -stage2 1 -comp2 1 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${PIN_EXTRA} ${DIST_FILE}"
    else
        echo "Invalid Pin mode: $NAME"
        exit 1
    fi
fi



# ==========================================================================================================
# Run trials
# ==========================================================================================================
# Reboot only once to capture how contiguity changes with each trial
echo "Rebooting $2"
ssh $2 "sudo reboot"
sleep 60


# System settings
if [ "$THP" == "1" ]; then
    THP_KERNEL_DIR="/sys/kernel/mm/transparent_hugepage"
    ssh $2 "sudo sh -c 'echo always > $THP_KERNEL_DIR/enabled'" > /dev/null
    ssh $2 "sudo sh -c 'echo $THP_SCAN > $THP_KERNEL_DIR/khugepaged/pages_to_scan'" > /dev/null
    ssh $2 "sudo sh -c 'echo $THP_SLEEP > $THP_KERNEL_DIR/khugepaged/scan_sleep_millisecs'" > /dev/null
fi
if [ "$DIRTY" != "0" ]; then
    ssh $2 "sudo sh -c 'echo $DIRTY > /proc/sys/vm/dirty_bytes'" > /dev/null
    ssh $2 "sudo sh -c 'echo $DIRTY_BG > /proc/sys/vm/dirty_background_bytes'" > /dev/null
fi
if [ "$CPU_LIMIT" != "0" ]; then
    # Set MAX_CPU = (CPU_LIMIT / 100 * 100000)
    MAX_CPU=$(echo "$CPU_LIMIT 100000" | awk '{print $1 * $2 / 100}')

    # Make cgroup for pin
    ssh $2 "sudo mkdir /sys/fs/cgroup/pin"
    ssh $2 "echo "$MAX_CPU 100000" | sudo tee /sys/fs/cgroup/pin/cpu.max"

    # Command to run pin in cgroup
    CG="sudo cgexec -g cpu:pin"
fi

# Always set swappiness to 0
ssh $2 "sudo sh -c 'echo 0 > /proc/sys/vm/swappiness'"

# Disable swap
ssh $2 "sudo swapoff -a"

# Always disable hyperthreading
ssh $2 "sudo sh -c 'echo off > /sys/devices/system/cpu/smt/control'"

# Create temp directory for trace files
ssh $2 "mkdir -p /home/michael/ssd/scratch/${APP}_tmp/"

# Trials
for i in $(seq 1 $1); do
    echo "========================================================================================"
    echo "$3: Trial $i"
    echo "========================================================================================"

    # Drop caches
    ssh $2 "sudo /home/michael/ssd/drop_cache.sh"

    # For memcached, run YCSB on the local machine
    if [ "$3" == "memA" ] || [ "$3" == "memB" ] || [ "$3" == "memC" ] || [ "$3" == "memW" ] || [ "$3" == "memDY" ]; then
        REGIONS=100

        # Run memcached as a background process
        ssh $2 "cd /home/michael/ISCA_2025_results/contiguity; ./loop.sh memcached ${REGIONS} > /home/michael/ISCA_2025_results/tmp/$5.txt" &
        ssh $2 "cd /home/michael/ISCA_2025_results; ${CG} ./run_pin.sh ${APP} ${PIN_ARGS}" &

        # Get khugepaged runtime using perf
        ssh $2 "sudo perf stat -e task-clock,cycles -p \$(pgrep khugepaged) -a &> /home/michael/ISCA_2025_results/tmp/khugepaged_${APP}_$i.txt" &
        ssh $2 "sudo perf stat -e task-clock,cycles -p \$(pgrep kcompactd) -a &> /home/michael/ISCA_2025_results/tmp/kcompactd_${APP}_$i.txt" &

        # Run from YCSB root directory
        DIR=$(pwd)
        cd $YCSB_ROOT
        $YCSB_LOAD 2>&1| grep -e "\[OVERALL\]" -e "\[READ\]" -e "\[UPDATE\]" -e "\[INSERT\]" -e "FAILED\]"
        $YCSB_RUN 2>&1| grep -e "\[OVERALL\]" -e "\[READ\]" -e "\[UPDATE\]" -e "\[INSERT\]" -e "FAILED\]"
        cd - > /dev/null

        # End the memcached server
        ssh $2 "sudo pkill -2 -f memcached"

        # End the khugepaged perf process
        ssh $2 "sudo pkill -2 -f perf"
        wait $(jobs -p)

        # Get THP stats from /proc/vmstat
        ssh $2 "grep -e 'thp' -e 'compact' /proc/vmstat" > $THP_DIR/vmstat_${APP}_$i.txt
    else
        ssh $2 "cd /home/michael/ISCA_2025_results/contiguity; ./loop.sh $3 > /home/michael/ISCA_2025_results/tmp/$5.txt" &

        # Execute the remote script, produces single output in ~/ISCA_2025_results/tmp/<app>.out
        ssh $2 "sudo perf stat -e task-clock,cycles -p \$(pgrep khugepaged) -a &> /home/michael/ISCA_2025_results/tmp/khugepaged_${APP}_$i.txt" &
        ssh $2 "sudo perf stat -e task-clock,cycles -p \$(pgrep kcompactd) -a &> /home/michael/ISCA_2025_results/tmp/kcompactd_${APP}_$i.txt" &
        ssh $2 "cd /home/michael/ISCA_2025_results; ${CG} ./run_pin.sh ${APP} ${PIN_ARGS}"
        ssh $2 "sudo pkill -2 -f perf"
        wait $(jobs -p)

        # Get THP stats from /proc/vmstat
        ssh $2 "grep -e 'thp' -e 'compact' /proc/vmstat" > $THP_DIR/vmstat_${APP}_$i.txt
    fi

    # Print size of trace directory
    ssh $2 "du -sh /home/michael/ssd/scratch/${APP}_tmp/"
    ssh $2 "rm -rf /home/michael/ssd/scratch/${APP}_tmp"
    ssh $2 "mkdir -p /home/michael/ssd/scratch/${APP}_tmp/"

    # Copy the output to the local machine, renaming it to include the trial number
    scp $2:/home/michael/ISCA_2025_results/tmp/$5.txt $OUTDIR/$5_$i.txt
    scp $2:/home/michael/ISCA_2025_results/tmp/${APP}.out $APP_OUT_DIR/${APP}_$i.out
    scp $2:/home/michael/ISCA_2025_results/tmp/khugepaged_${APP}_$i.txt $THP_DIR/khugepaged_${APP}_$i.txt
    scp $2:/home/michael/ISCA_2025_results/tmp/kcompactd_${APP}_$i.txt $THP_DIR/kcompactd_${APP}_$i.txt
    scp $2:/home/michael/ISCA_2025_results/tmp/${APP}_$i.perf $APP_OUT_DIR/${APP}.perf
    if [ "${DIST_FILE}" == "" ]; then
        scp $2:/home/michael/ISCA_2025_results/tmp/${APP}.dist $DIST_OUT_DIR/dist_$3_$i.txt
        ssh $2 "rm /home/michael/ISCA_2025_results/tmp/${APP}.dist"
    fi
    ssh $2 "rm /home/michael/ISCA_2025_results/tmp/$5.txt"
    ssh $2 "rm /home/michael/ISCA_2025_results/tmp/${APP}.out"
    ssh $2 "rm /home/michael/ISCA_2025_results/tmp/khugepaged_${APP}_$i.txt"
    ssh $2 "rm /home/michael/ISCA_2025_results/tmp/kcompactd_${APP}_$i.txt"
    ssh $2 "rm /home/michael/ISCA_2025_results/tmp/${APP}_$i.perf"
done

# Clean up temp directory
ssh $2 "rm -rf /home/michael/ssd/scratch/${APP}_tmp"