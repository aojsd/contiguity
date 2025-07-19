#!/bin/bash

# ==========================================================================================================
# Script Functions
# ==========================================================================================================

# Prints the script's usage instructions.
usage() {
    echo "Usage: $0 <num_trials> <remote_host> <app> <output_dir> [OPTIONS]"
    echo "Runs all pin mode configurations for a given application."
    echo
    echo "Options:"
    echo "  --fast                   Enable fast mode (skips several pin modes)."
    echo "  --config <file>          Path to a configuration file. Settings here are"
    echo "                           overridden by command-line flags."
    echo "  --NO_REBOOT              Do not reboot the machine between trials."
    echo "  --TRACK_PIN              Track contiguity of memory allocated by Pin."
    echo "  --THP <0|1>                Enable/disable Transparent Huge Pages (default: 1)."
    echo "  --THP_SCAN <num>           Set pages_to_scan for khugepaged (default: 4096)."
    echo "  --THP_SLEEP <ms>           Set scan_sleep_millisecs for khugepaged (default: 10000)."
    echo "  --DIRTY <pages>            Set dirty_bytes in pages (default: 0)."
    echo "  --CPU <%>                  Set CPU usage limit (e.g., 50.0) (default: 0)."
    echo "  --TIME_DILATION <factor>   Set time dilation factor (default: 0)."
    echo "  --FRAGMENT <GB>            Generate N gigabytes of memory fragmentation (default: 0)."
    echo "  --NO_COMPACT             Disable memory compaction."
    echo "  --ZERO_COMPACT           Set compaction proactiveness to 0."
    echo "  --DIST                   Collect memory access distribution."
    echo "  --RANDOM_FREELIST        Use random free list."
    echo "  --LOOP_SLEEP <sec>         Sleep time for loop.sh (default: 5)."
    echo "  --PIN \"<args>\"             Pass extra arguments to the Pin tool."
}

# Parses a config file to set script variables.
parse_config_file() {
    local config_file="$1"
    if [ ! -f "${config_file}" ]; then
        echo "Error: Config file not found at '${config_file}'" >&2
        exit 1
    fi
    echo "--- Loading settings from config file: ${config_file} ---"
    while IFS=':' read -r key val; do
        # Skip empty lines or comment lines
        if [[ -z "$key" || "$key" == \#* ]]; then
            continue
        fi
        
        # Trim whitespace from the value
        val="$(echo "$val" | xargs)"

        # Assign value to the correct script variable
        case "$key" in
            "Reboot Between Trials")            [[ "$val" == "Disabled" ]] && NO_REBOOT=1 ;;
            "Transparent Huge Pages (THP)")     [[ "$val" == "Enabled" ]] && THP=1 || THP=0 ;;
            "THP Pages to Scan")                THP_SCAN=$val ;;
            "THP Scan Sleep (ms)")              THP_SLEEP=$val ;;
            "Memory Compaction")                [[ "$val" == "Disabled" ]] && NO_COMPACT=1 ;;
            "Compaction Proactiveness Zero")    [[ "$val" == "Yes" ]] && ZERO_COMPACT=1 ;;
            "Dirty Pages")                      DIRTY=$val ;;
            "Memory Fragmentation to Generate (GB)") FRAGMENT=$val ;;
            "Use Random Freelist")              [[ "$val" == "Yes" ]] && RANDOM_FREELIST=1 ;;
            "CPU Limit")                        CPU_LIMIT=${val%\%} ;; # Remove trailing %
            "Time Dilation Factor")             TIME_DILATION=$val ;;
            "Track Pin Memory Contiguity")      [[ "$val" == "Yes" ]] && TRACK_PIN=1 ;;
            "Collect Access Distribution")      [[ "$val" == "Yes" ]] && DIST=1 ;;
            "Extra Pin Arguments")              PIN_EXTRA="${val//\"/}" ;; # Remove quotes
            "Loop Script Sleep (sec)")          LOOP_SLEEP=$val ;;
        esac
    done < "${config_file}"
}

# ==========================================================================================================
# Main Script Logic
# ==========================================================================================================

# --- Default values for optional arguments ---
FAST_MODE=0; THP=1; THP_SCAN=4096; THP_SLEEP=10000; DIRTY=0; CPU_LIMIT=0; TIME_DILATION=0; PIN_EXTRA="";
LOOP_SLEEP=5; FRAGMENT=0; NO_REBOOT=0; NO_COMPACT=0; ZERO_COMPACT=0; DIST=0; TRACK_PIN=0; RANDOM_FREELIST=0;

# --- Parse Command-Line Arguments ---
if [ "$#" -lt 4 ]; then
    usage
    exit 1
fi

# Pre-scan for a config file to establish base settings.
# This allows command-line flags to override config file settings.
for ((i=1; i<=$#; i++)); do
    if [ "${!i}" == "--config" ]; then
        config_idx=$i
        config_file_idx=$((i+1))
        if [ -n "${!config_file_idx}" ]; then
            parse_config_file "${!config_file_idx}"
        else
            echo "Error: --config flag requires a file path." >&2; usage; exit 1
        fi
        break
    fi
done

# --- Parse Positional & Optional Arguments ---
TRIALS=$1
HOST=$2
APP=$3
OUTDIR=$4
shift 4 # Consume positional arguments

# Command-line flags will override config file settings
while [ "$#" -gt 0 ]; do
    case "$1" in
        --fast)             FAST_MODE=1; shift 1;;
        --config)           # Already processed, just shift past it
                            shift 2;;
        --NO_REBOOT)        NO_REBOOT=1; shift 1;;
        --NO_COMPACT)       NO_COMPACT=1; shift 1;;
        --ZERO_COMPACT)     ZERO_COMPACT=1; shift 1;;
        --DIST)             DIST=1; shift 1;;
        --TRACK_PIN)        TRACK_PIN=1; shift 1;;
        --RANDOM_FREELIST)  RANDOM_FREELIST=1; shift 1;;
        --THP)              THP="$2"; shift 2;;
        --THP_SCAN)         THP_SCAN="$2"; shift 2;;
        --THP_SLEEP)        THP_SLEEP="$2"; shift 2;;
        --DIRTY)            DIRTY="$2"; shift 2;;
        --CPU)              CPU_LIMIT="$2"; shift 2;;
        --TIME_DILATION)    TIME_DILATION="$2"; shift 2;;
        --FRAGMENT)         FRAGMENT="$2"; shift 2;;
        --LOOP_SLEEP)       LOOP_SLEEP="$2"; shift 2;;
        --PIN)              PIN_EXTRA="$2"; shift 2;;
        *)                  echo "Unknown option: $1" >&2; usage; exit 1;;
    esac
done

# --- Construct Argument Array for Child Script ---
ARG_ARRAY=()
# Add all boolean flags if they are set
if [ "$NO_REBOOT" == "1" ];       then ARG_ARRAY+=(--NO_REBOOT); fi
if [ "$NO_COMPACT" == "1" ];      then ARG_ARRAY+=(--NO_COMPACT); fi
if [ "$ZERO_COMPACT" == "1" ];    then ARG_ARRAY+=(--ZERO_COMPACT); fi
if [ "$DIST" == "1" ];            then ARG_ARRAY+=(--DIST); fi
if [ "$TRACK_PIN" == "1" ];       then ARG_ARRAY+=(--TRACK_PIN); fi
if [ "$RANDOM_FREELIST" == "1" ]; then ARG_ARRAY+=(--RANDOM_FREELIST); fi

# Add all flags that take a value
ARG_ARRAY+=(--THP "$THP")
ARG_ARRAY+=(--THP_SCAN "$THP_SCAN")
ARG_ARRAY+=(--THP_SLEEP "$THP_SLEEP")
ARG_ARRAY+=(--DIRTY "$DIRTY")
ARG_ARRAY+=(--CPU "$CPU_LIMIT")
ARG_ARRAY+=(--TIME_DILATION "$TIME_DILATION")
ARG_ARRAY+=(--FRAGMENT "$FRAGMENT")
ARG_ARRAY+=(--LOOP_SLEEP "$LOOP_SLEEP")
ARG_ARRAY+=(--PIN "$PIN_EXTRA")

echo "--- Running all configurations for application: ${APP} ---"
if [ "$FAST_MODE" == "1" ]; then echo "--- FAST MODE ENABLED ---"; fi
echo "Passing extra args: ${ARG_ARRAY[@]}"

# Use set -x to see the exact commands being executed
set -x

# --- Execute Trials for Each Pin Mode ---
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR native "${ARG_ARRAY[@]}"
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR empty "${ARG_ARRAY[@]}"
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR fields "${ARG_ARRAY[@]}"
./contiguity_trials.sh $TRIALS $HOST $APP $OUTDIR pitracer "${ARG_ARRAY[@]}"

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