#!/usr/bin/env python3
"""V-2 (cadence) and V-3 (freshness) from a test-target log (rx_decode.py --log).

  timing_report.py raw.txt [--meas-ms 8.0] [--period-us 20000] [--tol-us 10]

V-2: every interval between record packets is period +/- tol (USBH-T1);
     every SOF interval is 1000 +/- 0.5 us (C1-2); no SOF gap >= 3 ms.
V-3: for every packet, (packet time) - (latest R-trigger marker) - meas-ms
     is at most 3 ms (USBH-T4). meas-ms is the BW = 00 measurement time
     measured in V-17.

Interim method (VE-3, no logic analyzer): times are the test target's DWT
timestamps taken in its USB and GPIO interrupts. They include that
interrupt's entry latency, so state the uncertainty with the result:
about +/-0.2 us for SOF and markers, and a few us for packet completion,
which the target sees only after the packet's handshake.
"""
import argparse
import statistics
import sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import target_log  # noqa: E402


def intervals(ts):
    return [(b - a) * 1e6 for a, b in zip(ts, ts[1:])]


def describe(name, iv, lo, hi, show=5):
    if not iv:
        print(f"{name}: no data")
        return False
    bad = [(i, v) for i, v in enumerate(iv) if v < lo or v > hi]
    print(f"{name}: n={len(iv)} min={min(iv):.3f} us max={max(iv):.3f} us "
          f"mean={statistics.fmean(iv):.3f} us stdev={statistics.pstdev(iv):.3f} us "
          f"outside [{lo:.1f}, {hi:.1f}]: {len(bad)}")
    for i, v in bad[:show]:
        print(f"    interval {i}: {v:.3f} us")
    return not bad


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--meas-ms", type=float, default=8.0)
    ap.add_argument("--period-us", type=float, default=20000.0)
    ap.add_argument("--tol-us", type=float, default=10.0)
    ap.add_argument("--sof-tol-us", type=float, default=0.5)
    ap.add_argument("--fresh-ms", type=float, default=3.0)
    args = ap.parse_args()

    sof, pkt, mark = [], [], []
    with open(args.log) as fh:
        for ev in target_log.parse_lines(fh):
            if ev.kind == "S":
                sof.append(ev.t)
            elif ev.kind == "O":
                pkt.append(ev.t)
            elif ev.kind == "M":
                mark.append(ev.t)
    sof.sort()
    pkt.sort()
    mark.sort()

    print("V-2 cadence")
    ok_sof = describe("  SOF interval", intervals(sof), 1000 - args.sof_tol_us, 1000 + args.sof_tol_us)
    gaps = [v for v in intervals(sof) if v >= 3000]
    print(f"  SOF gaps >= 3 ms: {len(gaps)}")
    # USBH-T1: a record delayed by whole frames (NAK, error) is exempt but
    # counted as late. Such a delay shows as an interval about k frames
    # long, followed by one about k frames short.
    piv = intervals(pkt)
    slips = 0
    keep = []
    i = 0
    while i < len(piv):
        dev = piv[i] - args.period_us
        k = round(dev / 1000.0)
        if k >= 1 and abs(dev - 1000.0 * k) <= args.tol_us:
            slips += 1
            if i + 1 < len(piv) and abs(piv[i + 1] - args.period_us + 1000.0 * k) <= args.tol_us:
                i += 1  # the following short interval belongs to the same slip
            i += 1
            continue
        keep.append(piv[i])
        i += 1
    ok_pkt = describe("  packet interval (frame slips excluded)", keep, args.period_us - args.tol_us,
                      args.period_us + args.tol_us)
    print(f"  late records (whole-frame slips, exempt but counted): {slips}")
    v2 = ok_sof and ok_pkt and not gaps and len(pkt) > 1
    print(f"  V-2: {'PASS' if v2 else 'FAIL'}")

    print("V-3 freshness")
    fresh = []
    j = 0
    for t in pkt:
        while j + 1 < len(mark) and mark[j + 1] <= t:
            j += 1
        if mark and mark[j] <= t:
            fresh.append((t - mark[j]) * 1e3 - args.meas_ms)
    if fresh:
        over = [f for f in fresh if f > args.fresh_ms]
        print(f"  end of R measurement -> packet: n={len(fresh)} min={min(fresh):.3f} ms "
              f"max={max(fresh):.3f} ms mean={statistics.fmean(fresh):.3f} ms "
              f"over {args.fresh_ms} ms: {len(over)}")
        v3 = not over
    else:
        print("  no marker edges in the log (diag build, D3 wired to the target?)")
        v3 = False
    print(f"  V-3: {'PASS' if v3 else 'FAIL'}")
    print("Interim method: target DWT timestamps; see this script's header for the uncertainty.")
    return 0 if (v2 and v3) else 1


if __name__ == "__main__":
    sys.exit(main())
