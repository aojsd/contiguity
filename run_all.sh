# Run trials of all configurations of an application using remote "run_pin.sh" script
# Usage: ./run_all.sh <num_trials> <remote host> <output_dir> <THP>
if [ "$#" -lt 4 ]; then
    echo "Usage: ./pin_trials.sh <num_trials> <remote host> <app> <output_dir> <Other>"
    exit 1
fi

# Rename args
TRIALS=$1
HOST=$2
APP=$3
OUTDIR=$4

# Parse extra arguments
eval "$(python3 bash_parser.py "${@:5}")"
echo "THP setting: ${THP}"
echo "Dirty bytes setting (pages): ${DIRTY}"
echo "Dirty background bytes (pages): ${DIRTY_BG}"
echo "CPU usage limit: ${CPU_LIMIT}"
echo "Extra Pin arguments: ${PIN_EXTRA}"

set -x
ARG_ARRAY=(--THP ${THP} --DIRTY ${DIRTY} --CPU ${CPU_LIMIT} --PIN "${PIN_EXTRA}")
EMPTY_ARG_ARRAY=(--THP ${THP} --PIN "${PIN_EXTRA}")

# Fields
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR fields "${ARG_ARRAY[@]}"

# Disk
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR disk "${ARG_ARRAY[@]}"

# Disk skip
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR disk-largebuf "${ARG_ARRAY[@]}"