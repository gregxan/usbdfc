#!/usr/bin/env python3
"""Decode usbdfc records (REC-1 .. REC-6) and check the stream (V-1).

Each record is a 46-byte little-endian payload, COBS-encoded and followed
by a 0x00 delimiter (48 bytes on the wire). The CRC is CRC-16/CCITT-FALSE
over payload bytes 0..43.

Live capture from the test target (VE-1) on its UART:
  rx_decode.py --port COM7 [--seconds 300] [--log raw.txt] [--csv rec.csv]
               [--script cmds.txt]
  --log saves every target line for timing_report.py. --script sends test
  target commands at given times: one "<seconds> <command>" per line, e.g.
  "60 mode nak 50".
Offline:
  rx_decode.py --logfile raw.txt [--csv rec.csv]
  rx_decode.py --hexfile frames.txt     (host test of src/record.c)

Windows and Python 3 (V-7). --port needs pyserial (tools/requirements.txt).
"""
import argparse
import csv
import struct
import sys
import time
from dataclasses import dataclass, asdict

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import target_log  # noqa: E402

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
    t_rx: float  # test-target receive time, s (0 if unknown)
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


def parse_frame(frame: bytes, t_rx: float = 0.0) -> Record:
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
    return Record(t_rx, ver, typ, flags, REASONS.get(reason, f"?{reason}"), boot_id, seq, t_us,
                  d[0] / 2, d[1] / 2, d[2] / 2, s[0] / 2, s[1] / 2, s[2] / 2)


def split_frames(data: bytes):
    """Split packet bytes into frames (delimiter removed). Partial trailing
    bytes are returned as the second value."""
    frames = data.split(b"\x00")
    return [f for f in frames[:-1] if f], frames[-1]


class StreamChecker:
    """V-1 pass criteria over a sequence of decoded records."""

    def __init__(self):
        self.records = 0
        self.bad_frames = 0
        self.seq_gaps = 0
        self.invalid = 0
        self.starts = 0
        self.boot_ids = set()
        self.last_seq = None

    def frame_error(self):
        self.bad_frames += 1

    def add(self, r: Record):
        self.records += 1
        self.boot_ids.add(r.boot_id)
        if r.flags & FLAG_START:
            self.starts += 1
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
                f"invalid={self.invalid} stream_starts={self.starts} boot_ids={len(self.boot_ids)} "
                f"-> {'PASS' if self.passed() else 'FAIL'} (V-1 criteria)")


class Decoder:
    def __init__(self, csv_path=None):
        self.chk = StreamChecker()
        self.partial = b""
        self.f = None
        self.writer = None
        if csv_path:
            self.f = open(csv_path, "w", newline="")
            self.writer = csv.DictWriter(self.f, fieldnames=list(Record.__dataclass_fields__.keys()))
            self.writer.writeheader()

    def packet(self, data: bytes, t_rx: float):
        frames, self.partial = split_frames(self.partial + data)
        for fr in frames:
            try:
                r = parse_frame(fr, t_rx)
            except ValueError:
                self.chk.frame_error()
                continue
            self.chk.add(r)
            if self.writer:
                self.writer.writerow(asdict(r))

    def close(self):
        if self.f:
            self.f.close()


def run_events(events, dec: Decoder):
    for ev in events:
        if ev.kind == "O":
            dec.packet(ev.data, ev.t)
        elif ev.kind == "E":
            print(f"[{ev.t:10.6f}] event: {ev.text}")


def load_script(path):
    cmds = []
    if path:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith("#"):
                    t, cmd = line.split(" ", 1)
                    cmds.append((float(t), cmd))
    return sorted(cmds)


def run_port(args) -> int:
    import serial  # pyserial

    dec = Decoder(args.csv)
    log = open(args.log, "w") if args.log else None
    script = load_script(args.script)
    parser = target_log.LineParser()
    pending = b""
    t0 = time.monotonic()
    with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
        ser.write(b"hello\n")
        while time.monotonic() - t0 < args.seconds:
            el = time.monotonic() - t0
            while script and script[0][0] <= el:
                _, cmd = script.pop(0)
                ser.write((cmd + "\n").encode())
                print(f"[{el:8.3f}] sent: {cmd}")
            pending += ser.read(65536)
            *complete, pending = pending.split(b"\n")
            events = []
            for raw in complete:
                line = raw.decode("ascii", "replace")
                if log:
                    log.write(line + "\n")
                ev = parser.parse(line)
                if ev is not None:
                    events.append(ev)
            run_events(events, dec)
    if log:
        log.close()
    dec.close()
    print(dec.chk.summary())
    return 0 if dec.chk.passed() else 1


def run_logfile(args) -> int:
    dec = Decoder(args.csv)
    with open(args.logfile) as fh:
        run_events(target_log.parse_lines(fh), dec)
    dec.close()
    print(dec.chk.summary())
    return 0 if dec.chk.passed() else 1


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
                print(parse_frame(raw[:-1]))
            except ValueError as e:
                print(f"error: {e}")
                rc = 1
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--port")
    g.add_argument("--logfile")
    g.add_argument("--hexfile")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--csv")
    ap.add_argument("--log")
    ap.add_argument("--script")
    ap.add_argument("--seconds", type=float, default=300.0)  # V-1: 5 minutes
    args = ap.parse_args()
    if args.port:
        return run_port(args)
    if args.logfile:
        return run_logfile(args)
    return run_hexfile(args)


if __name__ == "__main__":
    sys.exit(main())
