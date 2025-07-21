# Runs single trial of the specified application with specified pin arguments
# Usage: ./run_pin.sh <app> <pin_args>
if [ "$#" -lt 1 ]; then
    echo "Usage: ./run_pin.sh <app> <pin_args>"
    exit 1
fi

# Check if "-XT" is in the app name, X should be an integer for number of threads (e.g. "-12T")
#  - Default to 6 threads if not specified
THREADS=6
if [[ $1 =~ -([0-9]+)T$ ]]; then
    THREADS="${BASH_REMATCH[1]}"
    echo "Threads: ${THREADS}"
fi

# Select application to run
if [[ "$1" == *"llama"* ]]; then
    # llama
    LLAMA_ROOT=/home/michael/software/llama.cpp
    TOKENS=500
    # TASKS=10
    # HSWAG="--hellaswag --hellaswag-tasks ${TASKS} -f hellaswag_val_full.txt"
    # PROG="../build/bin/llama-perplexity ${HSWAG}"
    PROG="${LLAMA_ROOT}/build/bin/llama-cli -n ${TOKENS} -s 0 --no-mmap -t ${THREADS}"
    MODEL="-m ${LLAMA_ROOT}/models/llama-2-7b-chat.Q5_K_M.gguf"
    LLAMA_CMD="${PROG} ${MODEL}"
    CMD="${LLAMA_CMD}"
    cd ${LLAMA_ROOT}
    echo "Running Llama"

elif [[ "$1" == *"pr"* ]]; then
    # pr (PageRank)
    export OMP_NUM_THREADS=${THREADS}
    PR_ROOT=/home/michael/software/gapbs
    PR_CMD="${PR_OMP} ${PR_ROOT}/pr -f ${PR_ROOT}/benchmark/graphs/twitter.sg -n 1"
    CMD="${PR_CMD}"
    echo "Running PageRank"

elif [[ "$1" == *"memcached"* ]]; then
    # memcached - command runs memcached server
    MEM_CMD="/home/michael/software/memcached/memcached -p 11211 -m 16384 -t ${THREADS} -u michael"
    CMD="${MEM_CMD}"
    echo "Running Memcached"
else
    echo "Invalid application: $1"
    exit 1
fi

# Set up pin environment
PIN=/home/michael/software/PiTracer/pin
PT=/home/michael/software/PiTracer/source/tools/PiTracer/obj-intel64/pitracer.so

# Disable ASLR for deterministic IP results
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space > /dev/null

# Check if pin args are provided
if [ "$#" -lt 3 ]; then
    echo "No pin args provided, running without pin"
    mkdir -p /home/michael/ISCA_2025_results/tmp
    if [[ "$1" == *"memcached"* ]]; then
        { /usr/bin/time -v ${CMD} ; } 2>&1 | tee /home/michael/ISCA_2025_results/tmp/$1.out &
    else
        { /usr/bin/time -v ${CMD} ; } 2>&1 | tee /home/michael/ISCA_2025_results/tmp/$1.out
    fi
else  
    echo "Running with pin args: ${@:2}"
    if [[ "$1" == *"memcached"* ]]; then
        { /usr/bin/time -v ${PIN} -t ${PT} ${@:2} -- ${CMD} ; } 2>&1 | tee /home/michael/ISCA_2025_results/tmp/$1.out &
    else
        { /usr/bin/time -v ${PIN} -t ${PT} ${@:2} -- ${CMD} ; } 2>&1 | tee /home/michael/ISCA_2025_results/tmp/$1.out
    fi
fi
