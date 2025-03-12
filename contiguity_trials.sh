# Run trials of application using remote "run_pin.sh" script
# Usage: ./pin_trials.sh <num_trials> <remote_host> <app> <output_dir> <pin_mode> <Other>
if [ "$#" -lt 5 ]; then
    echo "Usage: ./pin_trials.sh <num_trials> <remote_host> <app> <output_dir> <pin_mode> <Other>"
    exit 1
fi

# Empty the remote scratch directory
ssh $2 "rm -rf /home/michael/ssd/scratch/*"

# Ouput subdir: <output_dir>/<app>/<pin_mode>
CURR_DIR=$(pwd)
OUTDIR=$CURR_DIR/$4/$3/$5
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
if [ "$ZERO_COMPACT" == "1" ]; then
    echo "Compaction Proactiveness: 0"
fi
if [ "$NO_COMPACT" == "1" ]; then
    echo "Memory Compaction: Disabled"
fi
echo "Dirty bytes setting (bytes): ${DIRTY}"
echo "Dirty background bytes (bytes): ${DIRTY_BG}"
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

# If the app has a suffix "-XT", set THREADS to X (e.g, "-12T" sets THREADS=12)
#  - Also retrieve the prefix before -XT
if [[ $3 =~ ^(.+)-([0-9]+)T$ ]]; then
    NAME="${BASH_REMATCH[1]}"
    THREADS="${BASH_REMATCH[2]}"
else
    NAME=$3
fi

# Memcached sub-directories
mkdir -p $OUTDIR
if [[ $3 == mem* ]]; then
    # Select memcached workload
    if [[ "$3" == memA* ]]; then
        WORKLOAD="workloada"
    elif [[ "$3" == memB* ]]; then
        WORKLOAD="workloadb"
    elif [[ "$3" == memC* ]]; then
        WORKLOAD="workloadc"
    elif [[ "$3" == memW* ]]; then
        WORKLOAD="workloadw"
    elif [[ "$3" == memDY* ]]; then
        WORKLOAD="workload_dynamic"
    else
        echo "Invalid memcached workload: $3"
        exit 1
    fi
    # if threads is not blank, add suffix to app name, otherwise do not add the suffix
    if [ -z "$THREADS" ]; then
        APP="memcached"
    else
        APP="memcached-${THREADS}T"
    fi
    NAME="memcached"
else
    APP=$3
fi

# YCSB commands
TARGET_IP=$(ssh -G $2 | awk '/^hostname/ { print $2 }')
YCSB_ROOT=/home/michael/software/YCSB
YCSB_HOST_ARGS="-p memcached.hosts=${TARGET_IP} -p memcached.port=11211 -p memcached.opTimeoutMillis=1000000"
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
PIN_ARGS=""
OUTPREFIX="/home/michael/ssd/scratch/${APP}_tmp/${APP}"
OUTP_DIR="/home/michael/ssd/scratch/${APP}_tmp"
if [ "$5" == "native" ]; then
    PIN_ARGS=""
elif [ "$5" == "empty" ]; then
    PIN_ARGS="-stage1 0 -bpages 16 ${PIN_EXTRA} ${DIST_FILE}"
elif [ "$5" == "disk" ]; then
    PIN_ARGS="-stage1 0 -bpages 16 -index_limit 20000 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
elif [ "$5" == "disk-nocache" ]; then
    PIN_ARGS="-buf_type 0 -comp1 -1 -stage1 0 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
elif [ "$5" == "disk-largebuf" ]; then
    PIN_ARGS="-stage1 0 -outprefix ${OUTPREFIX} -index_limit 200 ${PIN_EXTRA} ${DIST_FILE}"
elif [ "$5" == "struct" ]; then
    PIN_ARGS="-buf_type 0 -stage1 0 -comp1 3 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
elif [ "$5" == "fields" ] || [ "$5" == "fields-sync" ]; then
    PIN_ARGS="-stage1 0 -comp1 3 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
elif [ "$5" == "fields-sync" ]; then
    PIN_ARGS="-fsync 1 -stage1 0 -comp1 3 -outprefix ${OUTPREFIX} ${PIN_EXTRA} ${DIST_FILE}"
else
    echo "Invalid Pin mode: $5"
    exit 1
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
if [ "$ZERO_COMPACT" == "1" ]; then
    ssh $2 "sudo sh -c 'echo 0 > /proc/sys/vm/compaction_proactiveness'" > /dev/null
fi
if [ "$NO_COMPACT" == "1" ]; then
    ssh $2 "sudo sh -c echo never > /sys/kernel/mm/transparent_hugepage/defrag" > /dev/null
fi

# Always disable NMI watchdog
ssh $2 "sudo sh -c 'echo 0 > /proc/sys/kernel/nmi_watchdog'"

# Always set swappiness to 0
ssh $2 "sudo sh -c 'echo 0 > /proc/sys/vm/swappiness'"

# Disable swap
ssh $2 "sudo swapoff -a"

# Always disable hyperthreading
ssh $2 "sudo sh -c 'echo off > /sys/devices/system/cpu/smt/control'"

# Create temp directory for trace files
ssh $2 "mkdir -p /home/michael/ssd/scratch/${APP}_tmp/"

# For nocache runs:
# - change permissions of /dev/hugepages
# - reserve 768 hugepages
# - start the consumer process
if [[ $5 == *"-nocache" ]]; then
    ssh $2 "sudo chown michael /dev/hugepages"
    ssh $2 "sudo sh -c 'echo 768 > /proc/sys/vm/nr_hugepages'"
    ssh $2 "cd /home/michael/software/PiTracer/source/tools/PiTracer/consumer; ./consumer ${OUTP_DIR}" &
fi

# Trials
for i in $(seq 1 $1); do
    echo "========================================================================================"
    echo "$3: Trial $i"
    echo "========================================================================================"

    # Drop caches
    ssh $2 "sudo /home/michael/ssd/drop_cache.sh"

    # Start the contiguity script
    ssh $2 "cd /home/michael/ISCA_2025_results/contiguity; ./loop.sh ${NAME} ${REGIONS} > /home/michael/ISCA_2025_results/tmp/$5.txt" &

    # For memcached, run YCSB on the local machine
    if [[ $3 == mem* ]]; then
        REGIONS=100

        # Run memcached as a background process
        ssh $2 "cd /home/michael/ISCA_2025_results; ${CG} ./run_pin.sh ${APP} ${PIN_ARGS}" &

        # Get khugepaged runtime using perf
        ssh $2 "sudo perf stat -e task-clock,cycles -p \$(pgrep khugepaged) -a &> /home/michael/ISCA_2025_results/tmp/khugepaged_${APP}_$i.txt" &
        ssh $2 "sudo perf stat -e task-clock,cycles -p \$(pgrep kcompactd) -a &> /home/michael/ISCA_2025_results/tmp/kcompactd_${APP}_$i.txt" &

        # Run from YCSB root directory
        DIR=$(pwd)
        cd $YCSB_ROOT
        # Load
        $YCSB_LOAD 2>&1| grep -e "\[OVERALL\]" -e "\[READ\]" -e "\[UPDATE\]" -e "\[INSERT\]" -e "FAILED\]" | tee $APP_OUT_DIR/load_$i.out

        # Get midway vm stats
        ssh $2 "cat /proc/vmstat" > $THP_DIR/vmstat_loaded_${APP}_$i.txt
        $YCSB_RUN 2>&1| grep -e "\[OVERALL\]" -e "\[READ\]" -e "\[UPDATE\]" -e "\[INSERT\]" -e "FAILED\]" | tee $APP_OUT_DIR/run_$i.out
        cd - > /dev/null

        # End the memcached server
        ssh $2 "sudo pkill -2 -f memcached"

        # End the khugepaged perf process
        ssh $2 "sudo pkill -2 -f perf"
        wait $(jobs -p)

        # Get THP stats from /proc/vmstat
        ssh $2 "cat /proc/vmstat" > $THP_DIR/vmstat_${APP}_$i.txt
    else
        # ssh $2 "cd /home/michael/ISCA_2025_results/contiguity; ./loop.sh ${NAME} > /home/michael/ISCA_2025_results/tmp/$5.txt" &

        # Execute the remote script, produces single output in ~/ISCA_2025_results/tmp/<app>.out
        ssh $2 "sudo perf stat -e task-clock,cycles -p \$(pgrep khugepaged) -a &> /home/michael/ISCA_2025_results/tmp/khugepaged_${APP}_$i.txt" &
        ssh $2 "sudo perf stat -e task-clock,cycles -p \$(pgrep kcompactd) -a &> /home/michael/ISCA_2025_results/tmp/kcompactd_${APP}_$i.txt" &
        ssh $2 "cd /home/michael/ISCA_2025_results; ${CG} ./run_pin.sh ${APP} ${PIN_ARGS}" &
        PIN_PID=$!

        # For mid-way vm stats, take a snapshot at 10s for empty, and native configurations. Snapshot at 5min for disk configurations.
        if [[ $5 == *"disk"* ]] || [[ $5 == *"fields"* ]]; then
            sleep 300
            ssh $2 "cat /proc/vmstat" > $THP_DIR/vmstat_loaded_${APP}_$i.txt
        fi

        # Wait for the run to finish
        wait $PIN_PID
        ssh $2 "sudo pkill -2 -f perf"
        wait $(jobs -p)

        # Get THP stats from /proc/vmstat
        ssh $2 "cat /proc/vmstat" > $THP_DIR/vmstat_${APP}_$i.txt
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
    scp $2:/home/michael/ISCA_2025_results/tmp/${NAME}.perf $APP_OUT_DIR/${APP}_$i.perf
    if [ "$DIST" == "1" ]; then
        scp $2:/home/michael/ISCA_2025_results/tmp/${APP}.dist $DIST_OUT_DIR/dist_$3_$i.txt
        ssh $2 "rm /home/michael/ISCA_2025_results/tmp/${APP}.dist"
    fi
    ssh $2 "rm /home/michael/ISCA_2025_results/tmp/$5.txt"
    ssh $2 "rm /home/michael/ISCA_2025_results/tmp/${APP}.out"
    ssh $2 "rm /home/michael/ISCA_2025_results/tmp/khugepaged_${APP}_$i.txt"
    ssh $2 "rm /home/michael/ISCA_2025_results/tmp/kcompactd_${APP}_$i.txt"
    ssh $2 "rm /home/michael/ISCA_2025_results/tmp/${NAME}.perf"
done

# Clean up temp directory
ssh $2 "rm -rf /home/michael/ssd/scratch/${APP}_tmp"