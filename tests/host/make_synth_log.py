#!/usr/bin/env python3
"""Generate a synthetic test-target log to exercise the Python tools.

  make_synth_log.py out.txt [--seconds 60] [--sigma-counts 4] [--late 1]
                    [--jitter-sof 1] [--seed 1]

What it models (test_target/src/main.c output):
  - SOF every 1000 us, DWT at 150 MHz, starting just before a 32-bit wrap;
  - one record packet per 20 ms at SOF + 30 us; "O" lines are printed
    before the "S" lines that preceded them, as on the target;
  - an R-trigger marker toggle 10.4 ms after each cycle's tick 0;
  - field H = (1000, -2000, 300) counts plus Gaussian noise, offset 5;
  - optionally one packet delayed by one frame (--late) and one SOF moved
    by 2 us (--jitter-sof), which the timing report must flag.
Records are built with an independent Python encoder (not src/record.c).
"""
import argparse
import random
import struct

HZ = 150_000_000


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs(data):
    out, block = bytearray(), bytearray()
    for b in data:
        if b == 0:
            out.append(len(block) + 1)
            out += block
            block = bytearray()
        else:
            block.append(b)
    out.append(len(block) + 1)
    out += block
    return bytes(out) + b"\x00"


def record(seq, t_us, h, off):
    d = [2 * round(v) for v in h]
    s = [2 * o for o in off]
    p = struct.pack("<BBBBIIQiiiiii", 1, 1, 1 | (4 if seq == 0 else 0), 0, 0xDEADBEEF, seq, t_us, *d, *s)
    return cobs(p + struct.pack("<H", crc16(p)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--sigma-counts", type=float, default=4.0)
    ap.add_argument("--late", type=int, default=1)
    ap.add_argument("--jitter-sof", type=int, default=1)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    c0 = 0xFFFFFFFF - 5 * HZ  # wraps after 5 s

    def cyc(t_us):
        return (c0 + int(round(t_us * HZ / 1e6))) & 0xFFFFFFFF

    frames = int(a.seconds * 1000)
    late_cycle = 1000 if a.late else -1
    lines = [f"H {HZ} normal 0"]
    pending_sof = []
    for f in range(frames):
        t = f * 1000.0
        if a.jitter_sof and f == 2500:
            t += 2.0
        pending_sof.append(f"S {cyc(t):08x}")
        k = f % 20
        if k == 10:
            lines.append(f"M {cyc(t + 400.0 - 600.0 + 10.0):08x} {(f // 20) & 1}")
        if k == 0 and f >= 20:
            seq = f // 20 - 1
            delay = 1000.0 if seq == late_cycle else 0.0
            h = [v + rnd.gauss(0.0, a.sigma_counts) for v in (1000.0, -2000.0, 300.0)]
            frame = record(seq, int(seq * 20000 + 400), h, [5, 5, 5])
            lines.append(f"O {cyc(t + 30.0 + delay):08x} {frame.hex()}")
            lines.extend(pending_sof)  # SOFs printed after the packet line
            pending_sof = []
    lines.extend(pending_sof)
    with open(a.out, "w") as fh:
        fh.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
