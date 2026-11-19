# Task: independent code verification of usbdfc

> **Round 1 (areas 1–3) is complete** (findings F1–F11, fixed in
> `69e2796`; see docs/REQUIREMENTS.md section 8b). Round 2 is split into
> `docs/VERIFY_PROMPT_A.md` and `docs/VERIFY_PROMPT_B.md`. To run it, see
> `docs/VERIFY_SESSIONS.md`. This file remains the full reference list of
> areas.

You are reviewing firmware written by another model. Treat its comments,
commit messages and docs/REQUIREMENTS.md section 8a ("Implementation
assumptions") as claims to verify, not as facts. Verify each claim against
the actual source of the pinned dependencies in this repository.

## Project

- Repository: `gregxan/usbdfc`, branch `main`, commit `c301956` or later.
- Target: Seeed XIAO RP2350 (RP2350, Cortex-M33, A4 stepping assumed).
- Function: reads an MMC5983MA magnetometer over I2C1 by DMA as a
  SET/RESET pair, one record every 20 ms (BW = 00). It streams 48-byte
  COBS-framed records to a downstream USB CDC-ACM device.
- USB host: core 1 runs a PIO-USB host (TinyUSB host + `cdc_host` on
  Pico-PIO-USB). Core 0 runs the magnetometer, the 1 ms epochs and the
  watchdog.
- Diagnostic builds (`diag` preset) add a native-USB CDC status and command
  port on core 0, so TinyUSB runs dual-role.
- `test_target/` is Pico 2 firmware acting as the downstream device, with
  fault-injection modes.
- `tools/` holds the Python verification tools.

Pinned dependencies (read these, not upstream or memory):
- `lib/pico-sdk` = Pico SDK 2.2.0
- `lib/pico-sdk/lib/tinyusb` = TinyUSB `86ad6e5`
- `lib/Pico-PIO-USB` = `fe9133f`

Read first: `docs/REQUIREMENTS.md` (sections 4, 4.1, 5.x, 6, 7, 8a, 9) and
`docs/PROVENANCE.md`. The magnetometer code was adapted from a sibling
project (DFC Rev 2.16), which is not in this repo.

## Ground rules

- Every finding must cite `file:line`, in this repo or in the pinned
  dependency, showing the code that proves or refutes it.
- Classify each finding:
  - **BUG**: wrong now.
  - **LIKELY FAIL**: will probably fail a requirement or verification test
    on hardware.
  - **ASSUMPTION CONFIRMED** or **ASSUMPTION REFUTED**.
  - **CANNOT VERIFY WITHOUT HARDWARE**: say exactly what measurement would
    settle it.
- Rate severity: High (breaks streaming, data integrity or safety), Medium,
  Low.
- Build everything first and report the result:
  - `cmake --preset release|debug|diag` and the matching build;
  - `test_target` (cd into it, `cmake --preset release`);
  - the host tests in `tests/host/README.md`.

  The author built with arm-none-eabi-gcc 13.2. The project pins 14.2, so
  note any difference you can see.
- Do not modify code. Produce a report. You may include a proposed fix for
  each BUG or LIKELY FAIL.
- Do not re-review style. Focus on correctness, timing, concurrency and
  unverified claims.

## Highest-risk areas

### 1. Core 0 epoch races the magnetometer publish
Files: `src/main.c`, `src/mag.c`, `src/config.h`.

- **The concern.** The record for cycle N is built in the epoch at tick
  0 + `EPOCH_OFFSET_US` (500 µs). The pair it needs is published by the I2C
  STOP interrupt of the transaction that started at that same tick 0:
  read 9 bytes, then a SET pulse, then TM_M, at 400 kHz.
- **Check the transaction time.** Estimate it from the command list in
  `build_cmds()`: bytes, RESTARTs and bit times. If it can exceed ~500 µs,
  records will be built before their pair is published and come out as
  PAIR_INCOMPLETE.
- **Check the arithmetic against the requirements.** Raising the epoch
  offset pushes packets later and erodes the 3 ms freshness limit
  (USBH-T4). Check C1-5, USBH-T3 and USBH-T4 against that.

### 2. Enumeration flow and callback order in TinyUSB `86ad6e5`
File: `src/usbh.c`.

- **Callback order.** Does `tuh_cdc_mount_cb` really fire before
  `tuh_mount_cb` for a CDC-ACM device? If not, every CDC device is counted
  "unsupported".
- **Descriptor fetch inside the mount callback.** Can
  `tuh_descriptor_get_configuration()` be called from inside
  `tuh_cdc_mount_cb` (mid-enumeration) without failing because the control
  pipe is busy? If it fails, the device is marked ST_UNSUPPORTED and never
  streams.
- **Interface info.** Does `tuh_cdc_itf_get_info()` return the
  *communication* interface? `parse_endpoints()` depends on that.
- **Timeouts and failures.** Is an enumeration that never completes
  actually detected by the 2 s timeout logic? Are TinyUSB's internal
  enumeration failures ever counted as `enum_failures`, as section 7.2
  claims?

### 3. Transfer-result wrap and the streaming state machine
File: `src/usbh.c`.

- **Link-time wrap.** `-Wl,--wrap=hcd_event_handler`: confirm every call
  path that delivers XFER_COMPLETE from `hcd_pio_usb.c` is an external call,
  so the wrap sees it, and that none bypasses it.
- **Race with `tuh_task()`.** The ISR sets `s_out_done` before `tuh_task()`
  has released the endpoint (`busy` and `claimed` are cleared in
  `usbh.c`'s event processing). Can `queue_record()` run
  `tuh_cdc_write_flush()` while the endpoint is still busy? Trace what
  happens to the queued bytes, and whether `s_inflight` and `late`
  accounting stay correct.
- **Abort.** After `tuh_edpt_abort_xfer()` and `tuh_cdc_write_clear()`, is
  the TinyUSB tx stream in a state where the next write and flush start a
  transfer? Check `tu_edpt_stream_*` and `usbh_edpt_claim` / release.
- **Halt clear.** The `CLEAR_FEATURE` path, plus `reset_toggle()` writing
  `data_id` in `pio_usb_ep_pool`:
  - Is the `ep_num` / `dev_addr` match correct?
  - Is `data_id` the right field?
  - Can the frame ISR change it at the same time?
  - Does IN polling resume after `tuh_cdc_read(idx, &dummy, 0)`? Confirm a
    read of length 0 re-arms.
- **USBH-T5.** A record core 0 never publishes (for example a skipped epoch)
  is counted as "overwritten", not "missing". Report this as a spec
  mismatch if confirmed.

### 4. SOF generation outside Pico-PIO-USB's own timer
Files: `src/usbh.c`, Pico-PIO-USB.

- **`skip_alarm_pool = true`.** Confirm `pio_usb_host_init()` still sets
  `timer_active`, and that calling `pio_usb_host_frame()` from our TIMER0
  alarm interrupt (priority 0, core 1) is equivalent to the library's own
  timer path. Check whether anything else in the library or `hcd_pio_usb`
  assumes the alarm pool exists.
- **Shared TIMER0.** TIMER0 carries both the core 0 tick alarm and the
  core 1 SOF alarm. Check for shared-register races (INTR, ARMED, INTE via
  `hw_set_bits`). Confirm `irq_set_exclusive_handler()` from core 1 is
  correct under SDK 2.2.0, given the shared vector table and per-core NVIC.
- **C1-2's ±0.5 µs interval.** List everything on core 1 that disables
  interrupts or busy-waits in thread context:
  - TinyUSB with `OPT_OS_PICO`: critical sections in the osal queue;
  - `pio_usb_host_endpoint_abort_transfer()`'s `busy_wait_ms(1)`;
  - `hcd_port_reset`.

  Estimate the worst-case SOF interrupt latency. Also verify the ±0.5 µs
  figure against the USB 2.0 full-speed frame-interval specification; it
  came from memory.
- **SOF gaps.** Can anything stop SOF for 3 ms or more while a device is
  attached? Port reset, abort waits?

### 5. Dual-role TinyUSB (diag builds) and the native-USB device
Files: `src/usbd_diag.c`, `src/tusb_config.h`, `CMakeLists.txt`.

- **USB code shared between device and host.** `tinyusb_host_base` adds
  `RP2040_USB_HOST_MODE=1` and compiles `rp2040_usb.c`. Does that define
  change behaviour of the native *device* driver (`dcd_rp2040.c`) in the
  same image? Is `rp2040_usb.c` compiled once and correctly?
- **Cross-core safety.** Re-examine USBH-S1a: are any TinyUSB globals
  touched by both `tud_task()` (core 0) and `tuh_task()` (core 1) after
  initialisation (`tusb.c`, `usbh.c`, `usbd.c`, osal_pico)? The claim is
  that they are not.
- **Reset interface and descriptors.** Check the reset interface, the MS OS
  2.0 descriptor (copied in spirit from `pico_stdio_usb/reset_interface.c`),
  BOS with `bcdUSB` 0x0210, and `rom_reset_usb_boot_extra()` arguments.
  Will `picotool reboot -f` and the 1200-baud touch work?
- **Status writer.** `tud_cdc_connected()` gating means no output without
  DTR. Do the status lines fit `LINE_MAX` (240)? Does the code ever block?

### 6. Magnetometer driver adaptation (`src/mag.c`)
- **Pair assembly.** Check `sample_done()` and `eval_half()` for every
  abort or timeout at tick 0 or tick 10, and every run-time switch between
  pair and single mode (`g_mag_single`, `g_mag_r_tick`). Check that reason
  codes follow M-9 (amended) and section 8a M3. Check that the first record
  after a mode switch is NO_PRIOR.
- **Single mode.** Entering it applies a SET once. Is polarity right in
  every transition?
- **Start-up race.** `s_ready` guards the tick, which starts before
  `mag_init()` (C1-8). Is anything touched before init: the I2C IRQ, DMA
  channel 0, the PIO?
- **Resource allocation.**
  - The bus clear is on PIO1 with a computed divider.
  - Pico-PIO-USB uses PIO0 SMs 0–2 and claims DMA channel 11 by number
    (`dma_claim_mask` panics if it is taken).
  - Confirm nothing else claims channel 11 or PIO0 resources, and that the
    claim order in `main()` is guaranteed.
- **M-23: measurement time.** The S read at tick 10 happens about 9.6–9.7 ms
  after the S trigger. With the datasheet's conflicting 8 ms and 10 ms
  figures, every read may be NOT_READY. That trips M-2b after 10
  measurements and causes a re-probe loop. Flag this as CANNOT VERIFY, and
  state the exact behaviour if the real time exceeds the margin.
- **EX-4 (≤ 5 µs per ISR, core 0) at 120 MHz.** Estimate the I2C ISR cost
  on the tick-0 path: a 40-byte struct copy, `memcpy`s, `eval_half`,
  `count_saturation`, the publish. Also the tick ISR.

### 7. Inter-core mailbox (`src/mailbox.c`)
- **Correctness.** Prove or disprove the triple-buffer protocol under the
  Armv8-M memory model with the DMBs as written. Can core 0 ever write the
  slot core 1 is copying? Can core 1 return a torn record?
- **Liveness.** Can `mb_take()`'s retry loop livelock?

### 8. Clocks, watchdog and boot (`src/main.c`)
- **R-3.** Does `set_sys_clock_khz(120000, true)` after runtime init leave
  `clk_peri` at 120 MHz? Does any SDK code use the compile-time `SYS_CLK_HZ`
  (150 MHz) instead of `clock_get_hz()`, for example in I2C, PIO or
  Pico-PIO-USB divider set-up?
- **Watchdog.**
  - `watchdog_enable_caused_reboot()` semantics in SDK 2.2.0.
  - Do watchdog scratch 0–1 survive a watchdog reset and clear at power-on?
  - Do ROM or SDK reboots (`rom_reset_usb_boot_extra`, `watchdog_reboot`)
    touch scratch 0–1?
- **Chip stepping.** Does `rp2350_chip_version() >= 4` really identify A4?
  Check the SDK's decoding against the RP2350 datasheet's SYSINFO
  revision values.
- **Core 1 stack.** Is it in SCRATCH_X, and what is its default size in SDK
  2.2.0? Estimate the worst-case core 1 stack: thread plus the frame ISR,
  with TinyUSB and Pico-PIO-USB call depth, using the `.su` files.

### 9. Test target (`test_target/src/main.c`, `usb_descriptors.c`)
- **Timestamp source.** Does `dcd_rp2040.c` emit SOF and XFER_COMPLETE
  through `dcd_event_handler()` as external calls the wrap catches? Does
  `tud_sof_cb_enable(true)` enable SOF events?
- **NAK mode.** It relies on CDC device RX FIFO = 64 = endpoint size, so
  `_prep_out_transaction` stops re-arming. Confirm, and say how many
  packets are accepted after the mode starts.
- **STALL OUT.** After `usbd_edpt_stall()` while an OUT transfer is armed,
  and the host's CLEAR_FEATURE: does `usbd_edpt_clear_stall` clear `busy`?
  Does `tud_cdc_read(&dummy, 0)` re-arm OUT? (The author flagged this as
  unvalidated.)
- **Mode feasibility.** Check the `--wrap=cdcd_control_xfer_cb` STALL of
  SET_LINE_CODING, the vendor-class variant (does it configure with
  `CFG_TUD_VENDOR=1`?), and `enumhang`.

### 10. Python tools (`tools/`)
- **Timestamp unwrap.** `target_log.Unwrapper` treats a backwards offset of
  less than 2^31 cycles as out-of-order. Confirm the target's output order
  can never exceed that, and that the 'O' and 'S' line interleaving can't
  corrupt it.
- **Freshness.** `timing_report.py` pairs each packet with the latest
  marker edge. Is that the right R trigger for that record under every
  phase?
- **Slip classifier.** Can it hide real cadence failures?
- **Records split across packets.** Does `rx_decode.py` handle partial
  frames across packets?

## Also confirm or refute these factual claims

- The USB 2.0 full-speed SOF interval tolerance is 1.000 ms ± 0.5 µs.
- An idle bus suspends a device after 3 ms.
- TinyUSB's CDC host sends SET_CONTROL_LINE_STATE before SET_LINE_CODING,
  and only when the ACM descriptor advertises line-request support.
- TinyUSB retries each failed enumeration step 3 times, 100 ms apart.
- `hcd_edpt_clear_stall()` in `hcd_pio_usb` is a stub.
- The pid.codes VID/PID 0x1209/0x0001 is reserved for private testing.

## Report format

1. **Build and host-test results.**
2. **Findings table,** most severe first: ID, class, severity, file:line,
   one-line summary.
3. **Detail for each finding:** evidence with citations, failure scenario,
   proposed fix.
4. **Bench measurements:** a list of what must be measured on hardware
   to close each item marked CANNOT VERIFY WITHOUT HARDWARE.
