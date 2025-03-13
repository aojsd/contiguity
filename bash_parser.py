# Parses extra command line arguments for the main bash script
import argparse
import sys

def main(args):
    parser = argparse.ArgumentParser(description="Parse extra arguments for the main bash script")
    parser.add_argument("--TRACK_PIN", action="store_true", help="Track contiguity of memory allocated by Pin", default=False)
    parser.add_argument("--THP", type=int, help="THP setting", default=1)
    parser.add_argument("--THP_SCAN", type=int, help="THP scan setting", default=4096)
    parser.add_argument("--THP_SLEEP", type=int, help="THP sleep setting", default=10000)
    parser.add_argument("--DIRTY", type=int, help="Dirty bytes setting (pages)", default=0)
    parser.add_argument("--CPU", type=int, help="CPU usage limit", default=0)
    parser.add_argument("--PIN", type=str, help="Extra PIN arguments", default="")
    parser.add_argument("--LOOP_SLEEP", type=int, help="Sleep time for loop.sh", default=5)
    parser.add_argument("--ZERO_COMPACT", action="store_true", help="Set compaction proactiveness to 0", default=False)
    parser.add_argument("--NO_COMPACT", action="store_true", help="Disable memory compaction", default=False)
    parser.add_argument("--DIST", action="store_true", help="Collect access distribution", default=False)
    parser.add_argument("--RANDOM_FREELIST", action="store_true", help="Use random free list", default=False)
    args = parser.parse_args(args)

    dirty_bytes = args.DIRTY << 12
    print(f"THP={args.THP}")
    print(f"THP_SCAN={args.THP_SCAN}")
    print(f"THP_SLEEP={args.THP_SLEEP}")
    print(f"DIRTY={dirty_bytes}")
    print(f"DIRTY_BG={dirty_bytes >> 1}")
    print(f"CPU_LIMIT={args.CPU}")
    print(f"PIN_EXTRA=\"{args.PIN}\"")
    print(f"LOOP_SLEEP={args.LOOP_SLEEP}")
    print(f"ZERO_COMPACT={int(args.ZERO_COMPACT)}")
    print(f"NO_COMPACT={int(args.NO_COMPACT)}")
    print(f"DIST={int(args.DIST)}")
    print(f"RANDOM_FREELIST={int(args.RANDOM_FREELIST)}")
    print(f"TRACK_PIN={int(args.TRACK_PIN)}")
    return

if __name__ == "__main__":
    main(sys.argv[1:])