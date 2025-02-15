# Parses extra command line arguments for the main bash script
import argparse
import sys

def main(args):
    parser = argparse.ArgumentParser(description="Parse extra arguments for the main bash script")
    parser.add_argument("--THP", type=int, help="THP setting", default=1)
    parser.add_argument("--DIRTY", type=int, help="Dirty bytes setting (pages)", default=0)
    parser.add_argument("--PIN", type=str, help="Extra PIN arguments", default="")
    args = parser.parse_args(args)

    dirty_bytes = args.DIRTY << 12
    print(f"THP={args.THP}")
    print(f"DIRTY={dirty_bytes}")
    print(f"DIRTY_BG={dirty_bytes >> 1}")
    print(f"PIN_EXTRA=\"{args.PIN}\"")
    return

if __name__ == "__main__":
    main(sys.argv[1:])