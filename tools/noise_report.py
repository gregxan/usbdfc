#!/usr/bin/env python3
"""RMS noise and Allan deviation of the field (V-18, V-20).

  noise_report.py rec.csv [more.csv ...] [--label NAME ...]

Input: CSV files from rx_decode.py --csv. Only valid records are used.
Records are grouped by file and by record type (1 = pair mode,
2 = single mode, section 8a H2), so one V-18 capture that alternates modes
gives both groups. The field is converted at 16384 counts/G
(1 count = 0.061 mG).

Allan deviation is the overlapping estimator at tau = m x 20 ms. It
assumes evenly spaced samples; segments are split at seq gaps, and each
is used separately.
"""
import argparse
import csv
import math
import sys
from collections import defaultdict

MG_PER_COUNT = 1000.0 / 16384.0
TAU0 = 0.02
MS = (1, 2, 5, 10, 20, 50, 100, 200, 500, 1000)


def segments(rows):
    seg = []
    last = None
    for r in rows:
        if last is not None and r["seq"] != last + 1 and seg:
            yield seg
            seg = []
        seg.append(r)
        last = r["seq"]
    if seg:
        yield seg


def adev(series_list, m):
    """Overlapping Allan deviation over several contiguous series."""
    acc = 0.0
    n = 0
    for y in series_list:
        if len(y) < 2 * m + 1:
            continue
        x = [0.0]
        for v in y:
            x.append(x[-1] + v * TAU0)
        for i in range(len(x) - 2 * m):
            d = x[i + 2 * m] - 2 * x[i + m] + x[i]
            acc += d * d
            n += 1
    if n == 0:
        return None
    tau = m * TAU0
    return math.sqrt(acc / (2.0 * tau * tau * n))


def stdev(v):
    if len(v) < 2:
        return float("nan")
    mu = sum(v) / len(v)
    return math.sqrt(sum((a - mu) ** 2 for a in v) / (len(v) - 1))


def report(name, rows):
    segs = list(segments(rows))
    print(f"\n{name}: {len(rows)} valid records in {len(segs)} contiguous segment(s)")
    header = "  axis   mean (mG)   RMS (mG)   " + " ".join(f"{m * TAU0 * 1000:>7.0f}ms" for m in MS)
    print(header)
    for axis in ("hx", "hy", "hz"):
        allv = [r[axis] * MG_PER_COUNT for r in rows]
        series = [[r[axis] * MG_PER_COUNT for r in s] for s in segs]
        ad = [adev(series, m) for m in MS]
        cells = " ".join(f"{a:9.4f}" if a is not None else "        -" for a in ad)
        print(f"  {axis:4} {sum(allv) / len(allv):11.3f} {stdev(allv):10.4f}   {cells}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--label", nargs="*")
    args = ap.parse_args()
    labels = args.label or args.csv
    groups = defaultdict(list)
    for path, label in zip(args.csv, labels):
        with open(path, newline="") as fh:
            for row in csv.DictReader(fh):
                if (int(row["flags"]) & 1) == 0:
                    continue
                typ = int(row["type"])
                r = {"seq": int(row["seq"]), "hx": float(row["hx"]), "hy": float(row["hy"]),
                     "hz": float(row["hz"])}
                groups[(label, "pair" if typ == 1 else "single")].append(r)
    if not groups:
        print("no valid records")
        return 1
    for (label, mode), rows in groups.items():
        rows.sort(key=lambda r: r["seq"])
        report(f"{label} [{mode} mode]", rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
