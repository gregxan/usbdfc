#!/usr/bin/env python3
"""Decode usbdfc records (REC-1 .. REC-6) and check the stream (section 9.5).

Each record is a 46-byte little-endian payload, COBS-encoded and followed
by a 0x00 delimiter (48 bytes on the wire). The CRC is CRC-16/CCITT-FALSE
over payload bytes 0..43.

Usage:
  rx_decode.py --port COM7 [--baud 921600] [--csv out.csv] [--seconds 300]
      Read a byte stream of frames (e.g. forwarded by the test target) and
      check COBS framing, CRC, seq continuity and boot_id (V-1).
  rx_decode.py --hexfile frames.txt
      Decode one hex-encoded frame per line (host test of src/record.c).

Windows and Python 3 (V-7). --port needs pyserial (tools/requirements.txt).
"""
import argparse
import csv
import struct
import sys
import time
from dataclasses import dataclass, asdict

PAYLOAD_LEN = 46
FRAME_LEN = 48
COUNTS_PER_GAUSS = 16384

REASONS = {0: "OK", 1: "NO_PRIOR", 2: "NOT_READY", 3: "PAIR_INCOMPLETE", 4: "SENSOR_FAULT"}
FLAG_VALID, FLAG_STALE, FLAG_START, FLAG_SAT, FLAG_CHIP = 1, 2, 4, 8, 16


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_decode(enc: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(enc):
        code = enc[i]
        if code == 0:
            raise ValueError("zero byte inside COBS frame")
        i += 1
        end = i + code - 1
        if end > len(enc):
            raise ValueError("COBS code runs past the frame")
        out += enc[i:end]
        i = end
        if code < 0xFF and i < len(enc):
            out.append(0)
    return bytes(out)


@dataclass
class Record:
    version: int
    type: int
    flags: int
    reason: str
    boot_id: int
    seq: int
    t_us: int
    hx: float  # field H = d / 2, counts
    hy: float
    hz: float
    ox: float  # offset O = s / 2, counts
    oy: float
    oz: float

    @property
    def valid(self) -> bool:
        return bool(self.flags & FLAG_VALID)


def parse_frame(frame: bytes) -> Record:
    """frame: the COBS bytes without the trailing 0x00 delimiter."""
    payload = cobs_decode(frame)
    if len(payload) != PAYLOAD_LEN:
        raise ValueError(f"payload length {len(payload)}, expected {PAYLOAD_LEN}")
    crc = struct.unpack_from("<H", payload, 44)[0]
    if crc != crc16_ccitt_false(payload[:44]):
        raise ValueError("CRC mismatch")
    ver, typ, flags, reason, boot_id, seq, t_us = struct.unpack_from("<BBBBIIQ", payload, 0)
    d = struct.unpack_from("<iii", payload, 20)
    s = struct.unpack_from("<iii", payload, 32)
    return Record(ver, typ, flags, REASONS.get(reason, f"?{reason}"), boot_id, seq, t_us,
                  d[0] / 2, d[1] / 2, d[2] / 2, s[0] / 2, s[1] / 2, s[2] / 2)


class StreamChecker:
    """V-1 pass criteria over a sequence of decoded records."""

    def __init__(self):
        self.records = 0
        self.bad_frames = 0
        self.seq_gaps = 0
        self.invalid = 0
        self.boot_ids = set()
        self.last_seq = None

    def frame_error(self):
        self.bad_frames += 1

    def add(self, r: Record):
        self.records += 1
        self.boot_ids.add(r.boot_id)
        if not r.valid:
            self.invalid += 1
        if self.last_seq is not None and r.seq != (self.last_seq + 1) & 0xFFFFFFFF:
            self.seq_gaps += 1
        self.last_seq = r.seq

    def passed(self) -> bool:
        return (self.records > 0 and self.bad_frames == 0 and self.seq_gaps == 0
                and self.invalid == 0 and len(self.boot_ids) == 1)

    def summary(self) -> str:
        return (f"records={self.records} bad_frames={self.bad_frames} seq_gaps={self.seq_gaps} "
                f"invalid={self.invalid} boot_ids={len(self.boot_ids)} "
                f"-> {'PASS' if self.passed() else 'FAIL'}")


def split_frames(buf: bytearray):
    """Yield complete frames (without delimiter) from buf, consuming them."""
    while True:
        try:
            i = buf.index(0)
        except ValueError:
            return
        frame = bytes(buf[:i])
        del buf[:i + 1]
        if frame:
            yield frame


def run_port(args) -> int:
    import serial  # pyserial

    chk = StreamChecker()
    writer = None
    f = None
    if args.csv:
        f = open(args.csv, "w", newline="")
        writer = csv.DictWriter(f, fieldnames=list(Record.__dataclass_fields__.keys()))
        writer.writeheader()
    buf = bytearray()
    end = time.monotonic() + args.seconds
    with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
        while time.monotonic() < end:
            buf += ser.read(4096)
            for frame in split_frames(buf):
                try:
                    r = parse_frame(frame)
                except ValueError:
                    chk.frame_error()
                    continue
                chk.add(r)
                if writer:
                    writer.writerow(asdict(r))
    if f:
        f.close()
    print(chk.summary())
    return 0 if chk.passed() else 1


def run_hexfile(args) -> int:
    rc = 0
    with open(args.hexfile) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            raw = bytes.fromhex(line)
            if len(raw) != FRAME_LEN or raw[-1] != 0:
                print(f"bad frame length/delimiter: {line}")
                rc = 1
                continue
            try:
                r = parse_frame(raw[:-1])
            except ValueError as e:
                print(f"error: {e}")
                rc = 1
                continue
            print(r)
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--port")
    g.add_argument("--hexfile")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--csv")
    ap.add_argument("--seconds", type=float, default=300.0)  # V-1: 5 minutes
    args = ap.parse_args()
    return run_port(args) if args.port else run_hexfile(args)


if __name__ == "__main__":
    sys.exit(main())
