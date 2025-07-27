#!/bin/bash

# Get the absolute path to the directory where this script is located.
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)

# Source the shared configuration file using the absolute path.
source "${SCRIPT_DIR}/.arg_parsing.sh"

# ==========================================================================================================
# Main Script Logic
# ==========================================================================================================

# Check for help flag
for arg in "$@"; do
    if [[ "$arg" == "-h" || "$arg" == "--help" ]]; then
        usage "" "full"
        exit 0
    fi
done

# --- Parse Command-Line Arguments ---
if [ "$#" -lt 4 ]; then
    usage "run_all" "brief"
    exit 1
fi
FAST_MODE=0
EXP=0

# --- Parse Positional Arguments first ---
TRIALS=$1
HOST=$2
APP=$3
OUTDIR=$4
shift 4 # Consume positional arguments so only optional ones remain

# Pre-scan for a config file to establish base settings.
# This allows command-line flags to override config file settings.
# This loop only handles args specific to run_all.sh
# All other args are passed through to the ARG_ARRAY.
ARG_ARRAY=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --fast|-f|--FAST)
            FAST_MODE=1; shift 1;;
        --exp|-e|--EXP)
            EXP="1"; shift 1;;
        --config)
            # This logic remains as it is specific to this script
            parse_config_file "$2"
            shift 2;;
        *)
            # THIS IS THE KEY CHANGE:
            # Pass any unrecognized argument and its value to the child script.
            ARG_ARRAY+=("$1")
            # Check if the next argument is a value (doesn't start with '-')
            if [[ -n "$2" && "$2" != -* ]]; then
                ARG_ARRAY+=("$2")
                shift 2
            else
                shift 1
            fi
            ;;
    esac
done

# Check for fast mode
echo "--- Running all configurations for application: ${APP} ---"
if [ "$FAST_MODE" == "1" ]; then echo "--- FAST MODE ENABLED ---"; fi
echo "Passing extra args: ${ARG_ARRAY[@]}"

# --- Execute Trials for Each Pin Mode ---
# Experiment mode --> don't skip native, empty, and fields if EXP is 0
if [ "$EXP" == "0" ]; then
    set -x
    ./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR native "${ARG_ARRAY[@]}"
    ./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR empty "${ARG_ARRAY[@]}"
    # ./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR fields "${ARG_ARRAY[@]}"
else
    echo "--- Skipping native, empty, and fields configurations in experiment mode. ---"
    set -x
fi
# ./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR pitracer "${ARG_ARRAY[@]}"

# If in fast mode, exit now
if [ "$FAST_MODE" == "1" ]; then
    echo "--- Fast mode: skipping remaining pin configurations. ---"
    set +x
    echo "--- All runs complete (fast mode). ---"
    exit 0
fi

./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR disk-nocache "${ARG_ARRAY[@]}"
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR disk "${ARG_ARRAY[@]}"
# ./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR "disk-largebuf" "${ARG_ARRAY[@]}"
# ./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR "empty-sleep" "${ARG_ARRAY[@]}"

set +x
echo "--- All runs complete. ---"