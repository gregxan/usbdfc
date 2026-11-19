# Code provenance (B-26)

Code copied from the DFC firmware (`gregxan/dfc`) as a one-time copy, then
adapted for usbdfc. When DFC changes one of the source files below, review
the change, then either port it or record why it does not apply. Then
advance "Last DFC commit reviewed".

- **Baseline:** DFC Rev 2.16, commit `addc417` (2026-11-15)
- **Last DFC commit reviewed:** `addc417`

| usbdfc file | DFC source | Status |
|---|---|---|
| src/mag.c, src/mag.h | src/mag.c, src/mag.h | not yet copied |
| src/mmc5983ma.h | src/mmc5983ma.h | not yet copied |
| src/i2c_bus_clear.pio | src/i2c_bus_clear.pio | not yet copied |
| src/tick.c, src/tick.h | src/tick.c, src/tick.h | not yet copied (remove U-8 and the `link` call) |
| src/diag.c, src/diag.h | src/diag.c, src/diag.h | not yet copied |
| src/prof.c, src/prof.h | src/prof.c, src/prof.h | not yet copied |
| src/config.h (parts) | src/config.h | not yet copied |

## Ported DFC changes

| DFC commit | usbdfc commit | Notes |
|---|---|---|
| | | |
