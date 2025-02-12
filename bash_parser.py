# Parses extra command line arguments for the main bash script
import argparse
import sys

def main(args):
    parser = argparse.ArgumentParser(description="Parse extra arguments for the main bash script")
    parser.add_argument("--THP", type=int, help="THP setting", default=1)
    parser.add_argument("--DIRTY", type=int, help="Dirty bytes setting", default=0)
    parser.add_argument("--PIN", type=str, help="Extra PIN arguments", default="")
    args = parser.parse_args(args)
    # print(args.THP, args.dirty_bytes, f"\"{args.PIN}\"")
    print(f"THP={args.THP}")
    print(f"DIRTY={args.DIRTY}")
    print(f"DIRTY_BG={args.DIRTY >> 1}")
    print(f"PIN_EXTRA=\"{args.PIN}\"")
    return

if __name__ == "__main__":
    main(sys.argv[1:])