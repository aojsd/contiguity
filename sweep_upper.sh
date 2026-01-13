# Require remote host, output directory, and pin mode arguments (3 args)
if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <remote_host> <output_dir> <pin_mode>"
    exit 1
fi
REMOTE_HOST=$1
OUTPUT_DIR=$2
PIN_MODE=$3
# REBOOT="--NO_REBOOT"
FCALL="--FWORK process_command_ascii"

./contiguity_trials.sh 1 $REMOTE_HOST sync_microbench_2 $OUTPUT_DIR $PIN_MODE $REBOOT $FCALL
./contiguity_trials.sh 1 $REMOTE_HOST sync_microbench_4 $OUTPUT_DIR $PIN_MODE $REBOOT $FCALL
./contiguity_trials.sh 1 $REMOTE_HOST sync_microbench_8 $OUTPUT_DIR $PIN_MODE $REBOOT $FCALL
./contiguity_trials.sh 1 $REMOTE_HOST sync_microbench_32 $OUTPUT_DIR $PIN_MODE $REBOOT $FCALL
./contiguity_trials.sh 1 $REMOTE_HOST sync_microbench_64 $OUTPUT_DIR $PIN_MODE $REBOOT $FCALL
./contiguity_trials.sh 1 $REMOTE_HOST sync_microbench_256 $OUTPUT_DIR $PIN_MODE $REBOOT $FCALL

echo
echo
echo