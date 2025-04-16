# Run trials of all configurations of an application using remote "run_pin.sh" script
# Usage: ./run_fast.sh <num_trials> <remote host> <output_dir> <other>
if [ "$#" -lt 4 ]; then
    echo "Usage: ./run_fast.sh <num_trials> <remote host> <app> <output_dir> <Other>"
    exit 1
fi

# Rename args
TRIALS=$1
HOST=$2
APP=$3
OUTDIR=$4

set -x
ARG_ARRAY=(${@:5})

# Native
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR native "${ARG_ARRAY[@]}"

# Empty
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR empty "${ARG_ARRAY[@]}"

# Fields
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR fields "${ARG_ARRAY[@]}"

# Pitracer
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR pitracer "${ARG_ARRAY[@]}"
set +x