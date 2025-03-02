# Parses extra command line arguments for the main bash script
import argparse
import sys

def main(args):
    parser = argparse.ArgumentParser(description="Parse extra arguments for the main bash script")
    parser.add_argument("--THP", type=int, help="THP setting", default=1)
    parser.add_argument("--THP_SCAN", type=int, help="THP scan setting", default=4096)
    parser.add_argument("--THP_SLEEP", type=int, help="THP sleep setting", default=10000)
    parser.add_argument("--DIRTY", type=int, help="Dirty bytes setting (pages)", default=0)
    parser.add_argument("--CPU", type=int, help="CPU usage limit", default=0)
    parser.add_argument("--PIN", type=str, help="Extra PIN arguments", default="")
    parser.add_argument("--LOOP_SLEEP", type=int, help="Sleep time for loop.sh", default=5)
    parser.add_argument("--DIST", action="store_true", help="Collect access distribution", default=False)
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
    print(f"DIST={int(args.DIST)}")
    return

if __name__ == "__main__":
    main(sys.argv[1:])