#!/bin/bash

# ===================================================================
#               SHARED CONFIGURATION AND ARG PARSING
# ===================================================================

# Prints the script's usage instructions.
# Place this function in shared_config.sh
usage() {
    local caller_script=$1
    local mode=$2 # "brief" or "full"

    # Always print the one-line usage structure
    if [[ "$caller_script" == "run_all" ]]; then
        echo "Usage: $0 <num_trials> <remote_host> <app> <output_dir> [OPTIONS]"
    else
        echo "Usage: $0 <num_trials> <remote_host> <app> <output_dir> <pin_mode> [OPTIONS]"
    fi

    # Only print the full options list if mode is "full"
    if [[ "$mode" == "full" ]]; then
        echo # Blank line
        echo "Options:"
        if [[ "$caller_script" == "run_all" ]]; then
            echo "  --fast                Enable fast mode (skips several pin modes)."
            echo "  --config <file>       Path to a configuration file."
        fi

        # Print all the common options
        echo "  --NO_REBOOT           Do not reboot the machine between trials."
        echo "  --TRACK_PIN           Track contiguity of memory allocated by Pin."
        echo "  --THP <0|1>           Enable/disable Transparent Huge Pages (default: 1)."
        echo "  --THP_SCAN <num>      Set pages_to_scan for khugepaged (default: 4096)."
        echo "  --THP_SLEEP <ms>      Set scan_sleep_millisecs for khugepaged (default: 10000)."
        echo "  --DIRTY <pages>       Set dirty_bytes in pages (default: 0)."
        echo "  --CPU <%>             Set CPU usage limit (e.g., 50.0) (default: 0)."
        echo "  --TIME_DILATION <f>   Set time dilation factor (default: 0)."
        echo "  --FRAGMENT <GB>       Generate N gigabytes of memory fragmentation (default: 0)."
        echo "  --NO_COMPACT          Disable memory compaction."
        echo "  --ZERO_COMPACT        Set compaction proactiveness to 0."
        echo "  --DIST                Collect memory access distribution."
        echo "  --RANDOM_FREELIST     Use random free list."
        echo "  --LOOP_SLEEP <sec>    Sleep time for loop.sh (default: 5)."
        echo "  --PIN \"<args>\"        Pass extra arguments to the Pin tool."
    else
        echo "Use -h or --help to see the full list of options."
    fi
}

# Define all default values for shared options in one place.
THP=1
THP_SCAN=4096
THP_SLEEP=10000
DIRTY=0
CPU_LIMIT=0
TIME_DILATION=0
PIN_EXTRA=""
LOOP_SLEEP=5
FRAGMENT=0
NO_REBOOT=0
NO_COMPACT=0
ZERO_COMPACT=0
DIST=0
TRACK_PIN=0
RANDOM_FREELIST=0

# Create a function to parse all shared options.
parse_trial_args() {
    # This loop processes arguments until it finds one it doesn't recognize.
    while :; do
        case "$1" in
            --NO_REBOOT)        NO_REBOOT=1; shift ;;
            --NO_COMPACT)       NO_COMPACT=1; shift ;;
            --ZERO_COMPACT)     ZERO_COMPACT=1; shift ;;
            --DIST)             DIST=1; shift ;;
            --TRACK_PIN)        TRACK_PIN=1; shift ;;
            --RANDOM_FREELIST)  RANDOM_FREELIST=1; shift ;;
            --THP)              THP="$2"; shift 2 ;;
            --THP_SCAN)         THP_SCAN="$2"; shift 2 ;;
            --THP_SLEEP)        THP_SLEEP="$2"; shift 2 ;;
            --DIRTY)            DIRTY="$2"; shift 2 ;;
            --CPU)              CPU_LIMIT="$2"; shift 2 ;;
            --TIME_DILATION)    TIME_DILATION="$2"; shift 2 ;;
            --FRAGMENT)         FRAGMENT="$2"; shift 2 ;;
            --LOOP_SLEEP)       LOOP_SLEEP="$2"; shift 2 ;;
            --PIN)              PIN_EXTRA="$2"; shift 2 ;;
            -h|--help)
                # Let the calling script handle usage instructions
                return 1
                ;;
            *)
                # Stop parsing if an unknown option is encountered.
                # This leaves it for the calling script to handle.
                break
                ;;
        esac
    done
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