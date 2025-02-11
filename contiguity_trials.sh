# Run trials of application using remote "run_pin.sh" script
# Usage: ./pin_trials.sh <num_trials> <remote_host> <app> <output_dir> <pin_mode> <THP>
if [ "$#" -lt 5 ]; then
    echo "Usage: ./pin_trials.sh <num_trials> <remote_host> <app> <output_dir> <pin_mode> <THP>"
    exit 1
fi

# Memcached sub-directories
mkdir -p $4
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
    DIST_FILE="-record_file /home/michael/ISCA_2025_results/tmp/${APP}.dist"
    DIST_FILE="${IOSLEEP} ${DIST_FILE}"
    if [ "$NAME" == "empty" ]; then
        PIN_ARGS="${IOSLEEP} -stage1 0 ${@:7} ${DIST_FILE}"
    elif [ "$NAME" == "disk" ]; then
        PIN_ARGS="${IOSLEEP} -stage1 0 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} -index_limit 200 ${@:7} ${DIST_FILE}"
    elif [ "$NAME" == "disk-skip" ]; then
        PIN_ARGS="${IOSLEEP} -skip_time 120 -stage1 0 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} -index_limit 200 ${@:7} ${DIST_FILE}"
    elif [ "$NAME" == "struct" ]; then
        PIN_ARGS="${IOSLEEP} -stage1 0 -comp1 3 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${@:7} ${DIST_FILE}"
    elif [ "$NAME" == "fields" ]; then
        PIN_ARGS="${IOSLEEP} -stage1 0 -comp1 3 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${@:7} ${DIST_FILE}"
    elif [ "$NAME" == "fields-empty" ]; then
        PIN_ARGS="${IOSLEEP} -stage1 0 -comp1 3 ${@:7} ${DIST_FILE}"
    elif [ "$NAME" == "fields-threads" ]; then
        PIN_ARGS="${IOSLEEP} -stage1 12 -comp1 3 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${@:7} ${DIST_FILE}"
    elif [ "$NAME" == "split" ]; then
        PIN_ARGS="${IOSLEEP} -stage1 0 -comp1 4 -stage2 1 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${@:7} ${DIST_FILE}"
    elif [ "$NAME" == "split-empty" ]; then
        PIN_ARGS="${IOSLEEP} -stage1 0 -comp1 4 -stage2 1 ${@:7} ${DIST_FILE}"
    elif [ "$NAME" == "pfor" ]; then
        PIN_ARGS="${IOSLEEP} -stage1 0 -comp1 4 -stage2 1 -comp2 1 -outprefix /home/michael/ssd/scratch/${APP}_tmp/${APP} ${@:7} ${DIST_FILE}"
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
sleep 90
mkdir -p $4/$5

# Turn on THP if specified
if [ "$6" == "1" ]; then
    ssh $2 "sudo sh -c 'echo always > /sys/kernel/mm/transparent_hugepage/enabled'" > /dev/null
fi

# Disable swap
ssh $2 "sudo swapoff -a"

# Create temp directory for trace files
ssh $2 "mkdir -p /home/michael/ssd/scratch/${APP}_tmp/"

# Trials
for i in $(seq 1 $1); do
    echo "Trial $i"

    # Drop caches
    ssh $2 "sudo /home/michael/ssd/drop_cache.sh"

    # For memcached, run YCSB on the local machine
    if [ "$3" == "memA" ] || [ "$3" == "memB" ] || [ "$3" == "memC" ] || [ "$3" == "memW" ] || [ "$3" == "memDY" ]; then
	# Run memcached as a background process
        ssh $2 "cd /home/michael/ISCA_2025_results/contiguity; ./loop.sh memcached > /home/michael/ISCA_2025_results/tmp/$5.txt" &
        ssh $2 "cd /home/michael/ISCA_2025_results; ./run_pin.sh ${APP} ${PIN_ARGS}" &

        # Run from YCSB root directory
        DIR=$(pwd)
        cd $YCSB_ROOT
        $YCSB_LOAD 2>&1| grep -e "\[OVERALL\]" -e "\[READ\]" -e "\[UPDATE\]" -e "\[INSERT\]" -e "FAILED\]"
        $YCSB_RUN 2>&1| grep -e "\[OVERALL\]" -e "\[READ\]" -e "\[UPDATE\]" -e "\[INSERT\]" -e "FAILED\]"
        cd - > /dev/null

        # End the memcached server
        ssh $2 "sudo pkill -2 -f memcached"
        wait $(jobs -p)
    else
        ssh $2 "cd /home/michael/ISCA_2025_results/contiguity; ./loop.sh $3 > /home/michael/ISCA_2025_results/tmp/$5.txt" &

        # Execute the remote script, produces single output in ~/ISCA_2025_results/tmp/<app>.out
        ssh $2 "cd /home/michael/ISCA_2025_results; ./run_pin.sh ${APP} ${PIN_ARGS}"
        wait $(jobs -p)
    fi

    # Print size of trace directory
    ssh $2 "du -sh /home/michael/ssd/scratch/${APP}_tmp/"
    ssh $2 "rm -rf /home/michael/ssd/scratch/${APP}_tmp"
    ssh $2 "mkdir -p /home/michael/ssd/scratch/${APP}_tmp/"

    # Copy the output to the local machine, renaming it to include the trial number
    scp $2:/home/michael/ISCA_2025_results/tmp/$5.txt $4/$5/$5_$i.txt
done

# Clean up temp directory
ssh $2 "rm -rf /home/michael/ssd/scratch/${APP}_tmp"