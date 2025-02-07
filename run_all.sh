# Run trials of all applications using remote "run_pin.sh" script
# Usage: ./run_all.sh <num_trials> <output_dir> <pin_mode> <THP>
if [ "$#" -lt 4 ]; then
    echo "Usage: ./pin_trials.sh <num_trials> <output_dir> <pin_mode> <THP: 0 or 1>"
    exit 1
fi

# Setup script to stop all background jobs on interrupt
trap 'kill $(jobs -p)' SIGINT

# MemA
mkdir -p $2/memA
./contiguity_trials.sh $1 bingo memA $2/memA $3 $4 &

# PR
mkdir -p $2/pr
./contiguity_trials.sh $1 gussie pr $2/pr $3 $4 &

# Llama
mkdir -p $2/llama
./contiguity_trials.sh $1 bertie llama $2/llama $3 $4 &

# Wait for all background jobs to finish
wait $(jobs -p)