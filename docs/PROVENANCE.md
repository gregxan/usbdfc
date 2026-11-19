# Code provenance (B-26)

Code copied from the DFC firmware (`gregxan/dfc`) as a one-time copy, then
adapted for usbdfc. When DFC changes one of the source files below, review
the change, then either port it or record why it does not apply. Then
advance "Last DFC commit reviewed".

- **Baseline:** DFC Rev 2.16, commit `addc417` (2026-11-15)
- **Last DFC commit reviewed:** `addc417`

| usbdfc file | DFC source | Status |
|---|---|---|
| src/mag.c, src/mag.h | src/mag.c, src/mag.h | copied and adapted: transaction engine, aborts, bus clear, re-probe kept; schedule, pairs, reasons, BW, PIO1 changed (M-20 to M-23) |
| src/mmc5983ma.h | src/mmc5983ma.h | copied; BW = 00/01 constants added |
| src/i2c_bus_clear.pio | src/i2c_bus_clear.pio | copied unchanged (clock divider now set from clk_sys in mag.c) |
| src/tick.c, src/tick.h | src/tick.c, src/tick.h | copied; UART bookkeeping removed, 64-bit tick index |
| src/diag.c, src/diag.h | src/diag.c, src/diag.h | copied; counter and timing lists replaced (section 7.2), per-core DWT enable |
| src/prof.c, src/prof.h | src/prof.c, src/prof.h | copied; section list changed |
| src/config.h (parts) | src/config.h | magnetometer, priority and instrumentation parts copied and amended; UART parts dropped |

## Ported DFC changes

| DFC commit | usbdfc commit | Notes |
|---|---|---|
| | | |
