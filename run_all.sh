# Run trials of all configurations of an application using remote "run_pin.sh" script
# Usage: ./run_all.sh <num_trials> <remote host> <output_dir> <THP>
if [ "$#" -lt 4 ]; then
    echo "Usage: ./pin_trials.sh <num_trials> <remote host> <app> <output_dir> <Other>"
    exit 1
fi

# Setup script to stop all background jobs on interrupt
trap 'kill $(jobs -p)' SIGINT

# Rename args
TRIALS=$1
HOST=$2
APP=$3
OUTDIR=$4
EXTRA=$5
set -x

# Empty
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR/$APP empty $EXTRA

# Disk
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR/$APP disk $EXTRA

# Disk skip
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR/$APP disk-skip $EXTRA

# Fields
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR/$APP fields $EXTRA