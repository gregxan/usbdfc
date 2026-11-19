#!/usr/bin/env python3
"""Send commands to the test target (VE-2) and show its replies.

  target_ctl.py --port COM7 "mode nak 50"
  target_ctl.py --port COM7 "coil 50" --listen 2

Commands (test_target/src/main.c):
  mode normal | nak <ms> | neverack | stall_out_once | stall_out_rep |
       stall_in | in_data | ep32 | vendor | nolinereq |
       stall_linecoding | enumhang
  coil 50 | coil 60 | coil off
  hello

Only one program can hold the COM port. To change modes during a capture,
use rx_decode.py --script instead.
"""
import argparse
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--listen", type=float, default=0.5, help="seconds to print E/H replies")
    ap.add_argument("commands", nargs="+")
    args = ap.parse_args()

    import serial  # pyserial

    with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
        for c in args.commands:
            ser.write((c.strip() + "\n").encode())
        end = time.monotonic() + args.listen
        buf = b""
        while time.monotonic() < end:
            buf += ser.read(65536)
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                line = raw.decode("ascii", "replace")
                if line[:1] in ("E", "H"):
                    print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
