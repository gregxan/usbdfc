# Host tests (no hardware)

Run from the repository root.

1. **Record encoder** (`src/record.c`): CRC check value, COBS framing,
   field layout, decoded by `tools/rx_decode.py`.

   ```bash
   gcc -std=c11 -Wall -Wextra -Werror -Wconversion -Itests/host/stubs -Isrc \
       -o test_record tests/host/test_record.c src/record.c
   ./test_record > frames.txt
   python tools/rx_decode.py --hexfile frames.txt
   ```

2. **Python tools on a synthetic test-target log**: a 32-bit counter wrap,
   out-of-order lines, one late packet, one jittered SOF, and known noise
   (4 counts = 0.244 mG).

   ```bash
   python tests/host/make_synth_log.py log.txt --seconds 60
   python tools/rx_decode.py --logfile log.txt --csv rec.csv   # PASS, 2999 records
   python tools/timing_report.py log.txt     # flags the SOF jitter, 1 late record
   python tools/noise_report.py rec.csv      # RMS ~0.24 mG, ADEV ~ 1/sqrt(tau)
   python tests/host/make_synth_log.py clean.txt --late 0 --jitter-sof 0
   python tools/timing_report.py clean.txt   # V-2 PASS, V-3 PASS
   ```
