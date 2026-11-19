#!/usr/bin/env python3
"""Talk to the XIAO's diagnostic USB port (diag preset, NU-2).

  diag_ctl.py --port COM9 --watch 10            print status lines for 10 s
  diag_ctl.py --port COM9 "mode single"          send commands
  diag_ctl.py --port COM9 --alternate 60 --minutes 20 --log status.txt
        V-18: switch between "mode pair" and "mode single" every 60 s.
        Capture the records at the same time with rx_decode.py --csv; the
        record type (1 = pair, 2 = single) tells noise_report.py which
        mode each record came from.

Commands: mode pair | mode single | rtick <1..19> | stall <0|1|2> | status
"""
import argparse
import sys
import time


def pump(ser, buf, log, show=True):
    buf += ser.read(65536)
    *lines, buf = buf.split(b"\n")
    for raw in lines:
        line = raw.decode("ascii", "replace").rstrip("\r")
        if show:
            print(line)
        if log:
            log.write(line + "\n")
    return buf


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--watch", type=float, default=0.0)
    ap.add_argument("--alternate", type=float, help="seconds per mode (V-18)")
    ap.add_argument("--minutes", type=float, default=20.0)
    ap.add_argument("--log")
    ap.add_argument("commands", nargs="*")
    args = ap.parse_args()

    import serial  # pyserial

    log = open(args.log, "w") if args.log else None
    buf = b""
    with serial.Serial(args.port, 115200, timeout=0.05) as ser:
        ser.dtr = True  # the status reporter writes only while the port is open
        for c in args.commands:
            ser.write((c + "\r\n").encode())
        if args.alternate:
            end = time.monotonic() + 60.0 * args.minutes
            mode = "pair"
            next_switch = 0.0
            while time.monotonic() < end:
                now = time.monotonic()
                if now >= next_switch:
                    ser.write(f"mode {mode}\r\n".encode())
                    print(f"--- mode {mode}")
                    if log:
                        log.write(f"--- {now:.3f} mode {mode}\n")
                    mode = "single" if mode == "pair" else "pair"
                    next_switch = now + args.alternate
                buf = pump(ser, buf, log, show=False)
            ser.write(b"mode pair\r\n")
        else:
            end = time.monotonic() + max(args.watch, 0.5)
            while time.monotonic() < end:
                buf = pump(ser, buf, log)
    if log:
        log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
