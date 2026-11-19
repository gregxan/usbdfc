# Verification round 2, session B: magnetometer, test target, tools

You are reviewing firmware and tools written by another model. Treat its
comments, commit messages and docs/REQUIREMENTS.md as claims to verify, not
facts. Verify only against the pinned sources in this repository.

**Scope:** areas 6, 9 and 10 of `docs/VERIFY_PROMPT.md`. Session A covers
areas 4, 5, 7 and 8 and the round-1 fixes. Do not review those.

## Setup: read code, don't build

- **Commit.** Use `main` at `69e2796` or later. At `69e2796` the firmware
  presets and `test_target` build with no warnings outside the B-25
  exceptions, and the host tests pass. **Do not install a toolchain or
  build the firmware.**
- **Python.** You may run the Python host tests in `tests/host/README.md`
  (Python 3 only, no hardware) if a tools finding needs evidence.
- **Submodules.** Fetch only what this review reads:
  ```bash
  git submodule update --init --depth 1 lib/pico-sdk
  git -C lib/pico-sdk submodule update --init --depth 1 lib/tinyusb
  ```
  Pico-PIO-USB is not needed in this session.

## Files you may read

Read nothing outside this list unless a specific finding needs it. If one
does, say which file and why.

**This repository:**

| File | Functions |
|---|---|
| `src/mag.c` | `mag_tick_isr`, `i2c_isr`, `sample_done`, `eval_half`, `sample_start`, `start_dma`, `hw_idle`, `controller_hung`, `issue_abort`, `publish_pair`, `mag_read_pair`, `mag_init`, `build_cmds` |
| `src/mag.h`, `src/mmc5983ma.h`, `src/config.h` | all |
| `src/record.c` | `rec_build`, `pair_reason` |
| `src/main.c` | `build_and_publish` only |
| `test_target/src/main.c`, `usb_descriptors.c`, `target.h`, `tusb_config.h` | all |
| `tools/target_log.py`, `rx_decode.py`, `timing_report.py`, `noise_report.py`, `diag_ctl.py`, `target_ctl.py` | all |
| `tests/host/make_synth_log.py` | all |
| `docs/REQUIREMENTS.md` | sections 6, 5.1, 5.2, 8a (H2, M3, M9), 9 |

**Pico SDK** (`lib/pico-sdk/src/`):
- `rp2_common/hardware_i2c/i2c.c` and its header
- `rp2_common/hardware_dma/include/hardware/dma.h`

**TinyUSB** (`lib/pico-sdk/lib/tinyusb/src/`):
- `device/usbd.c`, `device/usbd_pvt.h`, `device/dcd.h`
- `class/cdc/cdc_device.c`
- `class/vendor/vendor_device.c`
- `portable/raspberrypi/rp2040/dcd_rp2040.c`

## Already verified: do not re-check

- **M-23:** the MMC5983MA datasheet figures (BW = 00: 8 ms in the BW table,
  0.4 mG RMS, 50 Hz maximum output rate) and the open 8 ms versus 10 ms
  conflict. Note the conflict's effect; don't research it.
- **NAK mode:** the TinyUSB CDC device stops re-arming OUT when less than
  `CFG_TUD_CDC_EP_BUFSIZE` is free (`_prep_out_transaction`). So with a
  64-byte FIFO and 64-byte endpoint, an unread record causes NAKs.
- **Synthetic log test:** the Python tools pass it: 32-bit counter wrap,
  out-of-order lines, a late packet, SOF jitter, known noise
  (tests/host/README.md).
- **F2 timing:** the tick-0 transaction is estimated at 445–520 µs. The
  epoch is now at tick + 750 µs and SOF at tick + 850 µs (C1-5).

## What to check

### B1. Magnetometer driver (area 6, `src/mag.c`)
- **Pair assembly.** Check `sample_done` and `eval_half` for:
  - each abort or timeout at tick 0, and at tick 10;
  - each switch between pair and single mode, and each `g_mag_r_tick`
    change, applied at tick 0.
- **Reason codes.** They should follow M-9 (amended) and section 8a M3.
  The first record after a mode switch should be NO_PRIOR. Entering single
  mode should apply one SET first. Is polarity right in every transition?
- **Start-up race.** `s_ready` guards the tick, which starts before
  `mag_init()`. Is anything touched before init: the I2C IRQ, DMA channel
  0, PIO1?
- **Resources.** The bus-clear PIO1 divider, and the SM claim. Check
  `dma_claim_unused_channel()` against channel 11, which Pico-PIO-USB
  claims by number.
- **M-23 margin.** The S read at tick 10 is about 9.6–9.7 ms after the S
  trigger. State the exact firmware behaviour if a real measurement takes
  longer: NOT_READY halves, then the M-2b fault after the score threshold,
  then the re-probe loop.
- **EX-4 (≤ 5 µs per ISR) at 120 MHz.** Estimate the `i2c_isr` cost on the
  tick-0 path: pair struct copy, `memcpy`, `eval_half`,
  `count_saturation`, publish.
- **`rec_build`.** Check the reason precedence (SENSOR_FAULT first) and the
  stale and saturated flags.

### B2. Test target (area 9)
- **Timestamps.** Does `dcd_rp2040.c` deliver SOF, XFER_COMPLETE and
  BUS_RESET through `dcd_event_handler()` as external calls the
  `--wrap` catches? Does `tud_sof_cb_enable(true)` enable SOF events?
- **NAK mode.** How many packets are accepted after it starts?
- **STALL OUT.** After `usbd_edpt_stall()` while an OUT transfer is armed,
  and the host's CLEAR_FEATURE: does `usbd_edpt_clear_stall` clear `busy`?
  Does `tud_cdc_read(&dummy, 0)` in `data_poll` re-arm OUT? (The author
  flagged this as unvalidated.)
- **Modes:**
  - the `--wrap=cdcd_control_xfer_cb` STALL of SET_LINE_CODING;
  - the vendor variant configuring with `CFG_TUD_VENDOR=1`;
  - `enumhang` (stopping `tud_task()` after a bus reset);
  - the descriptor variants' lengths.
- **Event ring.** Check `ev_push` for interrupt safety, and whether the
  UART output rate can overflow.

### B3. Tools (area 10)
- **`target_log.Unwrapper`.** It treats a backwards offset under 2^31
  cycles as out of order. Can the target's output order, from
  `events_drain` and `data_poll`, ever exceed that? Can the 'O' and 'S'
  interleaving corrupt the unwrap?
- **`timing_report.py`:**
  - freshness pairs each packet with the latest marker edge; is that the
    right R trigger under every phase?
  - can the whole-frame slip classifier hide real cadence failures?
- **`rx_decode.py`:**
  - frames split across packets (`split_frames`, `Decoder.packet`);
  - the `--script` timing;
  - the CSV fields.
- **`noise_report.py`:** the overlapping Allan deviation formula,
  segmentation at sequence gaps, and the counts-to-mG factor.

## Rules
- **Cite `file:line`** for every claim, in this repo or a pinned dependency.
- **Classify each finding:** BUG, LIKELY FAIL, ASSUMPTION CONFIRMED,
  ASSUMPTION REFUTED, or CANNOT VERIFY WITHOUT HARDWARE (name the
  measurement).
- **Rate severity:** High, Medium or Low.
- **Do not modify code.** Correctness only; no style review.

## Report: keep it short (aim for under ~1,200 words)
1. One line on setup: what you fetched, and what you ran.
2. A findings table, most severe first: ID (B-n), class, severity,
   `file:line`, one-line summary.
3. Per BUG or LIKELY FAIL: at most 5 lines covering evidence, failure
   scenario and fix.
4. Bench measurements needed, as a list.

Omit items that are fine, except a single line listing the checks that
passed.
