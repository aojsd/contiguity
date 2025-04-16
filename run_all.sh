# Run trials of all configurations of an application using remote "run_pin.sh" script
# Usage: ./run_all.sh <num_trials> <remote host> <output_dir> <other>
if [ "$#" -lt 4 ]; then
    echo "Usage: ./run_all.sh <num_trials> <remote host> <app> <output_dir> <Other>"
    exit 1
fi

# Rename args
TRIALS=$1
HOST=$2
APP=$3
OUTDIR=$4

# Parse extra arguments
eval "$(python3 src/python/bash_parser.py "${@:5}")"
echo "THP setting: ${THP}"
echo "Dirty bytes setting (bytes): ${DIRTY}"
echo "Dirty background bytes (bytes): ${DIRTY_BG}"
echo "CPU usage limit: ${CPU_LIMIT}"
echo "Extra Pin arguments: ${PIN_EXTRA}"

set -x
ARG_ARRAY=(${@:5})

# Native
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR native "${ARG_ARRAY[@]}"

# Empty
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR empty "${ARG_ARRAY[@]}"

# ZSTD
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR fields "${ARG_ARRAY[@]}"

# Pitracer
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR pitracer "${ARG_ARRAY[@]}"

# Disk no-cache
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR disk-nocache "${ARG_ARRAY[@]}"

# Disk
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR disk "${ARG_ARRAY[@]}"

# Disk Large-buf
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR disk-largebuf "${ARG_ARRAY[@]}"

# Empty-sleep
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR empty-sleep "${ARG_ARRAY[@]}"