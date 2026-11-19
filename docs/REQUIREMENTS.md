# usbdfc Requirements — DRAFT

**Revision 3.0-draft.** The baseline is DFC Rev 2.16. Nothing in this
document is final until the open decisions in section 8 are closed.

## 1. Scope

Firmware for a Seeed XIAO RP2350 that:

- Samples an MMC5983MA magnetometer on a fixed schedule and outputs one
  offset-free reading every 20 ms (50 Hz), made from a SET/RESET pair of
  low-noise (BW = 00) measurements.
- Streams every output period's record to a downstream USB CDC device. The
  XIAO is the USB host, on a PIO-USB port.
- Keeps output records evenly spaced: one per period, within a small
  tolerance.
- Builds, flashes, and debugs from a Windows host.

## 2. Carried over from DFC Rev 2.16

- **Magnetometer (M-1 … M-18).** Kept, with the amendments in section 6.
- **Build and toolchain (B-1 … B-25).** Kept. B-19 and B-20 are rewritten
  (section 5).
- **Execution rules for core 0 (EX-2 … EX-8, EX-10).** Kept, with the
  amendments in section 4.
- **Diagnostics (D-1 … D-6).** Kept, minus the UART items, plus the USB
  host counters.

### 2.1 Code taken from DFC

- **B-26. Copy once, with a recorded baseline.**
  - **Source.** usbdfc takes its magnetometer and support code from DFC Rev
    2.16 (commit `addc417`) as a one-time copy. It is not shared code: the
    copy is then adapted freely.
  - **Files:**
    - `mag.c`, `mag.h`, `mmc5983ma.h`, `i2c_bus_clear.pio`;
    - `tick.c` and `tick.h`, with the UART bookkeeping (U-8) and the call
      into `link` removed;
    - `diag.c`, `diag.h`, `prof.c`, `prof.h`;
    - the relevant parts of `config.h`.

    The DFC verification procedures for the magnetometer (V-5a, V-6) are
    the starting point for usbdfc's.
  - **Provenance file.** `docs/PROVENANCE.md` lists each copied file with
    its DFC source path and the baseline commit. It also records the last
    DFC commit that was reviewed.
  - **Porting.** When DFC changes one of the baseline files, the change is
    reviewed and either ported (noting the DFC commit) or recorded as not
    applicable, with the reason. The review commit in `docs/PROVENANCE.md`
    is then advanced.
  - **Licence.** Copied code is licensed 0BSD in usbdfc (LICENSE). Both
    repositories have the same owner.

## 3. Removed from DFC Rev 2.16

- HW-2, HW-3, HW-6, HW-6a (UART pins).
- Section 4 (U-1 … U-18): all UART requirements.
- The UART parts of EX-5, EX-9, D-1, D-3 and D-3a.
- The UART parts of V-1 to V-4.
- The U-8 and U-15a dependency in M-6. The tick stays, for the magnetometer.

## 4. Execution model (amendments)

- **HW-8 (replaced; detail in section 4.1).** Core 0 runs the magnetometer and the application,
  and, in diagnostic builds only, the native USB device (section 5.3). Core 1 is dedicated to the PIO-USB host: the frame
  handler (SOF alarm interrupt) and the TinyUSB host task. Core 1 handles no
  interrupts other than its SOF alarm, and its stack and hot data live in
  SCRATCH_X/Y.
- **EX-1 (amended).** The core 0 main loop is the only consumer of
  magnetometer data. It is also the only producer into the inter-core ring.
- **EX-5 (amended).** The priority order is: magnetometer completion and
  abort, then the tick, then native USB.
- **EX-10 (amended).** The epoch runs at a fixed phase after the tick (about
  tick + 500 µs), after the M-8 publish deadline. *TBD by measurement.*
- **IC-1 (new).** Core 0 hands records to core 1 through a triple-buffer
  mailbox:
  - **Slots.** Three record slots. Core 0 always writes into a slot that is
    neither the published newest nor the one core 1 is reading, then
    publishes it as the newest. Core 1 always takes the newest.
  - **Never blocks.** Neither side ever waits for the other, and the
    freshest record always wins. A record overwritten before core 1 took it
    shows up as a sequence-number gap and is counted by core 1 (records
    overwritten).
  - **Memory access.** Plain aligned 32-bit loads and stores with memory
    barriers (DMB). No hardware spinlocks are used. Cross-core atomic
    read-modify-write (exclusive load/store) is used only if verified on the
    RP2350.
- **IC-2 (new).** Core 1 notices new records by polling, in a tight loop:
  check the mailbox, queue any new record with `tuh_cdc_write()` and
  `tuh_cdc_write_flush()`, call `tuh_task()`, increment its loop
  heartbeat, and repeat.
- **IC-3 (new; amends D-2 for core 1).** Every counter has exactly one
  writing core. Core 1's counters are cumulative and are never cleared. Core
  0 reads them as aligned 32-bit words and reports differences between
  snapshots. Core 0's own counters keep D-2's atomic read-and-clear.

### 4.1 Core 1 (USB host core)

- **C1-1. What runs.** Core 1 runs only:
  - the SOF alarm interrupt, which calls `pio_usb_host_frame()` (USBH-S4).
    This is the only interrupt enabled in core 1's NVIC;
  - the IC-2 thread loop.

  All of it runs from RAM (B-18).
- **C1-2. SOF timing.** While the host port is active:
  - **Interval.** The interval from one SOF to the next shall be
    1000 µs ± 0.5 µs, the USB 2.0 full-speed frame-interval tolerance,
    measured on the bus.
  - **No gaps.** SOF shall never stop for 3 ms or more while a device is
    attached. That is how long an idle bus takes to send a device into
    suspend.
- **C1-3. Frame handler budget.** The whole frame handler (SOF, all
  transactions, and event handling) shall take at most 300 µs per frame.
  *(TBD by measurement.)* Its maximum is recorded (section 7).
- **C1-4. Thread deadlines.**
  - A record published by core 0 shall be queued with TinyUSB within
    50 µs, and before SOF minus the USBH-T3 margin. *(TBD by
    measurement.)*
  - The thread may block only inside TinyUSB's own enumeration delays
    (USBH-3). It never blocks while streaming.
- **C1-5. Phase.** Epochs run every 1 ms at tick + 500 µs. SOF runs every
  1 ms at tick + 600 µs. The record for each cycle is published in the epoch
  of cycle tick 0 (M-20) and goes out on that tick's SOF. Both offsets are
  provisional until measured.
- **C1-6. Memory.** Core 1's stack is in SCRATCH_X, the SDK default for core 1.
  Its size is set from `-fstack-usage` (B-23). Pico-PIO-USB's and TinyUSB's
  host buffers go in SCRATCH_X if they fit, otherwise in main SRAM.
- **C1-7. Liveness and watchdog (amends D-5).** Core 0 feeds the hardware
  watchdog once per epoch. It feeds it only if all three are true:
  - core 0's own epoch has run;
  - core 1's SOF counter has advanced since the last check;
  - core 1's loop heartbeat has advanced within the last 1 s. This allows
    for TinyUSB's blocking enumeration delays.

  Further rules:
  - **Timeout.** The watchdog timeout is 1.5 s. *(TBD.)* It is enabled only
    after start-up (C1-8) has finished, so the blocking magnetometer
    start-up (M-2) cannot trip it.
  - **D-5 exception.** A watchdog reset is the single automatic recovery
    action permitted. It is classed as recovery, not normal operation.
  - **Persistent count.** On every boot, firmware checks whether the
    watchdog caused the reboot. If it did, a watchdog-reset counter, kept in
    the watchdog scratch registers, is incremented. The counter survives the
    reset, is cleared at power-on, and is reported. A reset loop is visible
    through it.
  - **VBUS during reset.** The VBUS switch enable shall default to off
    (pulled down) while the chip is in reset. The downstream device then
    sees a clean detach and re-attach.
  - **Expected gap.** About 1 s after a watchdog reset: the magnetometer
    restarts (M-2) and the device re-enumerates (USBH-3).
  - **Debugging.** The watchdog and timers keep the RP2350 default of
    pausing while a debugger halts a core.
- **C1-8. Start-up order.**
  1. Core 0 sets `clk_sys` to 120 MHz (R-3), handles the watchdog-reset
     check (C1-7), and starts the magnetometer tick, so the timer grid
     exists. If native USB is enabled, it also runs `tud_init()`.
  2. Core 0 launches core 1. Core 1 calls `tuh_configure()` and
     `tuh_init(1)`, claiming the fixed USB DMA channel (USBH-S3), then
     starts the SOF alarm on the tick grid plus the C1-5 offset.
  3. Core 0 initialises the magnetometer. Its DMA claims come after step 2.
  4. Core 0 enables the watchdog and enters the main loop.
- **C1-9. Debugging.** Timing verification shall use free-running builds
  only. Halting a core under SWD stops SOF, so the device suspends. That is
  expected while debugging.
- **R-3 (replaced).** `clk_sys` shall be 120 MHz, set at build time. This is
  within the RP2350's 150 MHz rating and is the clock most widely used with
  Pico-PIO-USB. Its PIO dividers are fractional: 2.5 for TX and 1.25 for RX.
  The bus capture in section 9 shall confirm that this causes no receive or
  transmit errors. `clk_peri` follows `clk_sys`. The I2C rate (M-1), the
  M-8 transaction times and the EX-4, EX-7 and EX-8 limits shall be
  re-measured at 120 MHz, because code runs 20 % slower than DFC's 150 MHz.

## 5. USB

- **USBH-1.** The host port is PIO-USB with D+ on GPIO0 (pad D6) and D- on
  GPIO1 (pad D7): Pico-PIO-USB's `PIO_USB_PINOUT_DPDM`, where D- = D+ + 1.
- **USBH-1a.** The port shall support only a full-speed device attached
  directly. A low-speed device, or a hub, shall be treated as unsupported
  (USBH-9).
- **USBH-2.** The only supported class is CDC-ACM: a communication interface
  (class 0x02, subclass 0x02) paired with a data interface (class 0x0A)
  that has one bulk IN and one bulk OUT endpoint. Any communication-interface
  protocol code is accepted. On a composite device, the first CDC-ACM
  function is used, located through its Interface Association Descriptor or
  its Union functional descriptor. Vendor USB-serial bridges (FTDI, CP210x,
  CH34x) are not supported.
- **USBH-3.** Enumeration is performed by the TinyUSB host stack in the core
  1 host task (USBH-S1). It may block that task, using TinyUSB's own delays:
  a 450 ms connect debounce, a 50 ms reset and 2 ms address recovery. It
  shall never block core 0 or delay the core 1 frame handler.
  - **Time limit.** With a device that answers every request, enumeration
    shall complete within 1 s of attach. *(To be confirmed by measurement.)*
  - **Timeout.** TinyUSB has no control-transfer timeout, so the
    application shall add one. If a device that has attached is not ready
    to stream within 2 s, the enumeration-failure counter is incremented
    and USBH-9 applies.
- **USBH-4.** After SET_CONFIGURATION, the host shall send, in TinyUSB's
  order:
  1. SET_CONTROL_LINE_STATE with DTR = 1 and RTS = 1
     (`CFG_TUH_CDC_LINE_CONTROL_ON_ENUM`).
  2. SET_LINE_CODING: 115,200 baud, 8 data bits, no parity, 1 stop bit
     (`CFG_TUH_CDC_LINE_CODING_ON_ENUM`). Native-USB CDC devices ignore the
     value; it is sent for devices that expect it.

  Both are sent only if the device's ACM functional descriptor advertises
  line-request support, as TinyUSB does and as the CDC specification
  intends. Streaming starts once the device is mounted. A failure or STALL
  on either request does not stop the mount: TinyUSB's ACM driver proceeds
  regardless. The application cannot see the result of these two
  requests, so no counter is kept for them.
- **USBH-4a.** The host keeps a bulk IN transfer queued at all times
  (TinyUSB's CDC host does), so the IN endpoint is polled once per frame.
  This keeps the device's transmit buffer from filling. Bytes received are
  counted and discarded. A NAK is the expected, normal response. The CDC
  notification (interrupt IN) endpoint is opened but not polled.
  - **Order within a frame.** Pico-PIO-USB runs endpoints in the order they
    were opened, which is the device's descriptor order. If the device lists
    bulk IN before bulk OUT, the IN poll comes first in each frame. Its cost
    (a NAK is a few µs) shall be included in the USBH-T1 measurement.
- **USBH-5.** Max packet size:
  - **Bulk endpoints.** Both bulk endpoints shall report a maximum packet
    size of 64 bytes. A device reporting less is unsupported (USBH-9). One
    record fits in one packet. TinyUSB does not check this, so the
    application shall check it from the configuration descriptor at mount.
  - **EP0.** Any EP0 maximum packet size allowed at full speed (8, 16, 32
    or 64 bytes) shall be accepted for control transfers.
- **USBH-6.** The downstream device is self-powered. The host shall still
  supply VBUS, since a self-powered device detects attach from VBUS and
  shall not connect its D+ pull-up without it. The current limit is set for
  a low-power port: the switch shall supply at least 100 mA. *(Part TBD.)*
- **USBH-7.** VBUS and line hardware: see section 5.4 (HW-11 to HW-14).
- **USBH-7a.** Self-powered device effects, to be checked in the hardware
  design:
  - **Ground loop.** The USB ground joins the two supplies. This shall not
    degrade the MMC5983MA noise floor (measure with and without the device
    attached).
  - **Unpowered XIAO.** With the XIAO unpowered and the device powered, D+/D-
    shall not back-power the RP2350. A compliant device keeps its pull-up
    off without VBUS; a non-compliant one shall not damage the XIAO.
- **USBH-8.** RP2350-E9: only A4-stepping parts are supported (HW-10).
  Detach detection shall still be verified on the actual boards.
- **USBH-9.** Unsupported or failed devices.
  - **Unsupported device** (not CDC-ACM, low speed, a hub, or bulk
    endpoints under 64 bytes). TinyUSB sends SET_CONFIGURATION before any
    class driver is matched, so such a device ends up configured. The
    application shall never send it data. It shall increment the
    unsupported-device counter and wait for detach.
  - **Failed enumeration.** TinyUSB's built-in retry is normal operation
    under D-5: each failed enumeration step is repeated up to 3 times,
    100 ms apart, with no new bus reset. After the last attempt fails, or
    after the USBH-3 timeout, the enumeration-failure counter is
    incremented and the device is left as it is.
  - **No further action** in either case: no VBUS cycling, no bus reset, no
    re-enumeration until detach. A new attach starts a new enumeration.
    That is a new event, not a retry, so it is normal operation under D-5.
  - **Transfer errors while streaming.** Pico-PIO-USB repeats a failed
    transaction up to 3 times, on later frames, before reporting an error.
    This is also normal operation under D-5. Each error reported is
    counted.
- **B-19 / B-20 (rewritten).** The native USB port is a CDC device to a PC
  in diagnostic builds only. Production builds leave the native USB
  controller unused (section 5.3).

### 5.0 Stack and resources

- **USBH-S1.** The host stack is TinyUSB's host core, with `cdc_host` (ACM
  only) and the `hcd_pio_usb` port driver, running on Pico-PIO-USB.
  - **Root ports.**
    - **Production builds:** host only. `CFG_TUD_ENABLED = 0`, so no device
      stack is linked. Root port 1 is the PIO-USB host
      (`CFG_TUH_RPI_PIO_USB = 1`), served by `tuh_task()` on core 1.
    - **Diagnostic builds:** dual-role. Root port 0 is the native USB device,
      served by `tud_task()` on core 0 (section 5.3), and root port 1 is the
      host, as in production.
  - **Class options.** `CFG_TUH_CDC_FTDI`, `CFG_TUH_CDC_CP210X` and
    `CFG_TUH_CDC_CH34X` are 0, and `CFG_TUH_HUB` is 0.
  - **Write path.** Only core 1 calls `tuh_cdc_write()` and
    `tuh_cdc_write_flush()`, taking records from the IC-1 ring.
- **USBH-S1a. Dual-role safety (diagnostic builds only).** TinyUSB has no
  cross-core locking. In a bare-metal build its only lock masks that
  stack's own interrupt on the calling core, and the PIO-USB host's lock is
  empty. Its device and host halves keep separate state. Dual-role use is
  safe only under these rules:
  1. All device-side calls, `tud_init()`, `tud_task()` and the native USB
     interrupt stay on core 0.
  2. All host-side calls, `tuh_init()`, `tuh_task()` and the SOF interrupt
     stay on core 1.
  3. `tud_init()` completes before core 1 calls `tuh_init()` (C1-8).
  4. Only the PIO-USB host driver (`hcd_pio_usb`) is linked, not the SDK's
     native host driver.
  5. `tusb_time_millis_api()` only reads the timer, and
     `tusb_time_delay_ms_api()` waits on the timer. Neither uses the SDK
     alarm pool.
  6. Data crosses cores only through IC-1 and the single-writer counters
     (IC-3).

  **Verification.** Review TinyUSB `86ad6e5`'s shared files (`tusb.c`,
  `usbh.c`, `usbd.c`) for globals touched after initialisation by both
  halves. Then soak-test a diagnostic build with status output and host
  streaming running together, for the V-1 duration.

  **Fallback.** If a diagnostic build fails, it uses SWD for status instead.
  Production builds are unaffected.
- **USBH-S2.** Versions (B-9):
  - **TinyUSB:** the copy bundled with Pico SDK 2.2.0, commit `86ad6e5`.
  - **Pico-PIO-USB:** a git submodule at `lib/Pico-PIO-USB`, pinned to
    commit `fe9133f`. That is the commit TinyUSB `86ad6e5` lists in its
    dependencies (`tools/get_deps.py`).
  - **Build integration.** The SDK provides no CMake target for
    Pico-PIO-USB, so the project's CMakeLists.txt builds its sources and
    TinyUSB's `hcd_pio_usb.c` itself.
- **USBH-S3.** Pico-PIO-USB configuration, passed through
  `tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, …)`:
  - `pin_dp = 0` and `pinout = PIO_USB_PINOUT_DPDM` (USBH-1);
  - TX, RX and EOP state machines on PIO0. They use all 32 of its
    instruction slots;
  - `tx_ch`: a fixed DMA channel, proposed channel 11, defined in config.h.
    Pico-PIO-USB claims it by number, so no other code may claim it, and
    all other DMA users shall claim channels only after the USB host init
    has run;
  - `skip_alarm_pool = true`, so the library starts no timer of its own.
- **USBH-S4.** SOF (USBH-T2): the application calls `pio_usb_host_frame()`
  from a core-1-only interrupt on a second alarm of TIMER0. TIMER0 is the
  timer that runs the magnetometer tick, so SOF and tick count the same
  time base and stay phase-locked. The alarm is scheduled from the previous
  target time.
- **USBH-S5.** The M-18 I2C bus-clear state machine moves from PIO0 to PIO1.
  PIO2 stays free.
- **B-25 (extended).** Every TinyUSB and Pico-PIO-USB source compiled into
  the image is held to B-22 … B-24. Exceptions are listed file by file, with
  a written justification. The list is to be filled in after the first
  build and reviewed whenever USBH-S2 changes.

### 5.1 Output timing

The output period is 20 ms (50 Hz): one record per magnetometer cycle (M-20).
USB frames are 1 ms, so each record has one designated frame: the frame
whose SOF follows the record's publication.

- **USBH-T1. Cadence.** Each record's bulk OUT shall start within X µs of
  its designated frame's SOF. *(X TBD by measurement, expected under
  50 µs.)* The interval between consecutive records' packets shall be
  20,000 µs ± 10 µs, measured on the bus. A record delayed by a NAK or an
  error (USBH-T6) is exempt from this, but is counted as late.
- **USBH-T2. SOF lock.** SOF shall be generated from the magnetometer tick's
  timer grid, at a fixed offset (C1-5), each one scheduled from the previous
  target time.
- **USBH-T3. Publish margin.** Core 0 shall publish each record at least
  50 µs before its designated SOF. *(TBD by measurement.)*
- **USBH-T4. Freshness.** The time from the end of the record's second
  (RESET) measurement to the start of its packet on the bus shall be at
  most 3 ms. The record represents the field over the two 8 ms measurement
  windows in M-20.
- **USBH-T5. What is sent.** Every record core 0 publishes is sent. Invalid
  records are sent with their reason code (M-9). If core 0 has published
  nothing new by the designated frame, core 1 sends nothing for that
  period. The device sees a gap in the sequence numbers, and core 1 counts
  it as a missing slot.
- **USBH-T6. Late records: newest wins.**
  - **One at a time.** Core 1 queues at most one record with TinyUSB at a
    time.
  - **A NAK or error delays it.** Pico-PIO-USB retries the record on the
    next frame, automatically (USBH-9).
  - **Newer records wait.** Until the delayed record is sent, newer records
    stay in the mailbox (IC-1), and only the newest survives.
  - **Stuck records are aborted.** If a record has not been accepted 20 ms
    (one period) after it was queued, core 1 aborts the transfer, counts the
    record as aborted, and queues the newest record from the mailbox. A
    device that recovers therefore receives fresh data, never a stale
    record. *(Verify that `hcd_pio_usb` supports TinyUSB's transfer abort.)*
  - **Counted.** Late, aborted and overwritten records are counted.
- **USBH-T7 (requirement on the downstream device).** The device shall accept
  one packet of up to 64 bytes per 20 ms period, continuously.
- **USBH-T8. Start and restart.**
  - **When the cadence applies.** USBH-T1 applies from the first record
    sent after the device mounts. That record carries a stream-start flag.
  - **Sequence numbers.** A record's sequence number is its cycle index
    since boot (M-20). It keeps counting across a detach and re-attach, so
    the device can see what it missed. A reboot restarts it from zero.
  - **Boot identity.** Each record carries a random boot identifier
    (REC-4), so a reboot is never mistaken for a sequence wrap.

### 5.2 Record format

- **REC-1. Encoding.** Each record is a binary frame:
  - the 46-byte payload below, COBS-encoded;
  - followed by a single 0x00 delimiter.

  The encoded frame is 48 bytes, so it fits one 64-byte packet (USBH-5).
  Each record goes out in exactly one bulk OUT transfer. The stream carries
  data records only: no status or heartbeat records.
- **REC-2. Payload.** All multi-byte fields are little-endian.

  | Offset | Size | Field | Contents |
  |---|---|---|---|
  | 0 | 1 | `version` | 1 |
  | 1 | 1 | `type` | 1 = magnetometer data. Other values are reserved. |
  | 2 | 1 | `flags` | bit 0 valid; bit 1 stale (M-17); bit 2 stream start (USBH-T8); bit 3 saturated (any raw S or R axis within 72 counts of either end of the 18-bit range, D-1); bit 4 unsupported chip stepping (HW-10). Other bits are 0. |
  | 3 | 1 | `reason` | 0 OK, 1 NO_PRIOR, 2 NOT_READY, 3 PAIR_INCOMPLETE (M-9) |
  | 4 | 4 | `boot_id` | u32 random value drawn at boot from the RP2350 hardware random-number generator (REC-4) |
  | 8 | 4 | `seq` | u32 cycle index since boot (M-20) |
  | 12 | 8 | `t_us` | u64 µs since boot, at the S trigger (M-10, M-22) |
  | 20 | 12 | `d_x`, `d_y`, `d_z` | i32 S − R per axis, in raw 18-bit counts |
  | 32 | 12 | `s_x`, `s_y`, `s_z` | i32 S + R − 262144 per axis, in raw 18-bit counts |
  | 44 | 2 | `crc` | CRC-16 over bytes 0 to 43 (REC-3) |

  **Units.** Raw counts at 16384 counts per gauss (datasheet p. 2). No
  scaling or calibration is applied on the XIAO. The receiver computes
  field H = d / 2 and offset O = s / 2, in counts. The firmware sends the
  sum and difference rather than the halved values, so no half-count is
  rounded away (amends M-22).
- **REC-3. CRC.** CRC-16/CCITT-FALSE:
  - polynomial 0x1021, initial value 0xFFFF;
  - not reflected, final XOR 0x0000;
  - check value 0x29B1 for the ASCII string "123456789";
  - computed over the payload before COBS encoding.

  The receiver discards any frame whose CRC or length is wrong.
- **REC-4. Boot identity (amends USBH-T8).** `boot_id` is a fresh random
  32-bit value on every boot, whether power-on, watchdog or other reset. A
  change in `boot_id` marks a reboot, whatever `seq` does. No flash writes
  are needed. The watchdog-reset count (C1-7) stays a diagnostic counter and
  is not in the record.
- **REC-5. Temperature.** Not reported. The SET/RESET pair cancels the
  temperature-driven offset drift (M-22). The datasheet gives no
  temperature-measurement time, and none is scheduled (M-23).
- **REC-6. Compatibility.** A receiver shall ignore frames with an unknown
  `version` or `type`. A new field or meaning requires a new `version`.

### 5.3 Native USB (diagnostic builds only)

- **NU-1. Builds.** The native USB device exists only in diagnostic presets
  (B-7). Production presets have no device stack, no `USBCTRL_IRQ` and no
  `tud_task()`. They are flashed from BOOTSEL entered with the BOOT button,
  which the ROM's own USB handles without any firmware.
- **NU-2. Content.**
  - **What it sends.** Human-readable status lines, one set per second:
    - every counter in section 7, including the watchdog-reset count;
    - the timing maxima (C1-2 to C1-4, D-4, D-6);
    - the fault flags;
    - the latest record's validity and reason.
  - **Never blocks.** A line is written only when the CDC buffer has room
    for all of it, as DFC's status reporter does.
  - **No data mirror.** The binary data records are not mirrored to this
    port.
- **NU-3. Timing on core 0.**
  - **Interrupt.** `USBCTRL_IRQ` has the lowest priority (EX-5).
  - **Task.** `tud_task()` and the status writer run once per epoch, after
    the application work. Their time is excluded from the epoch measurement
    and recorded as a separate maximum (D-4).
  - **Bounds.** The native USB interrupt is bounded by EX-4.
- **NU-4. Implementation.** `pico_stdio_usb` is not used, because it
  supplies its own TinyUSB configuration and descriptors, which cannot also
  enable the host. The project provides:
  - its own `tusb_config.h` (USBH-S1);
  - device descriptors;
  - the CDC status writer, which calls `tud_cdc_write()` and uses no
    `printf`.
- **NU-5. Reboot into BOOTSEL without the button.**
  - **Interfaces.** The device is composite: the CDC interface (with an
    Interface Association Descriptor) plus the vendor reset interface
    defined by the Pico SDK (`pico/usb_reset_interface.h`).
  - **1200-baud touch.** Setting the line coding to 1,200 baud reboots into
    BOOTSEL.
  - **picotool.** Its reset requests (`picotool reboot -f`, `load -f`) work.
  - **Before rebooting.** Firmware turns VBUS off (USBH-7), so the
    downstream device sees a clean detach.
- **NU-6. Identity.**
  - **Development.** Diagnostic builds use the Pico SDK's default Raspberry
    Pi VID/PID.
  - **Strings.** The serial-number string is the RP2350 flash unique ID.
    The product string identifies a usbdfc diagnostic build.
  - **Release gate.** Before any units are distributed, a dedicated product
    ID shall be obtained from Raspberry Pi, which offers them for RP2040 and
    RP2350 products. NU-6 shall then be updated.

### 5.4 Host-port hardware and power

- **HW-10. Chip stepping.** Only XIAO RP2350 boards with an A4-stepping
  RP2350 are supported. A4 fixes erratum E9, where GPIO inputs can latch
  near 2 V and the 15 kΩ host pull-downs might not pull a detached line low.
  - **Check at boot.** Firmware reads the chip revision from SYSINFO. The
    register value for A4 is to be confirmed from the RP2350 datasheet.
  - **On any other stepping,** firmware:
    - sets `flags` bit 4 in every record (REC-2);
    - reports the stepping in diagnostic status (NU-2);
    - otherwise runs normally, with no automatic action (D-5).
- **HW-11. Power.**
  - **Source.** In production, an external regulated 5 V supply feeds the
    XIAO's 5V pad. The same rail supplies VBUS through the load switch
    (HW-12).
  - **Rating.** The supply shall provide at least 250 mA. This covers the
    XIAO, the MMC5983MA (about 6.4 mA typical plus SET/RESET pulses, M-23)
    and the ≥ 100 mA VBUS allowance (USBH-6).
  - **Back-feed.** Diagnostic builds may also have a PC on USB-C. The
    connection between the XIAO's USB-C VBUS and its 5V pad shall be
    checked on the Seeed schematic. If they are connected directly, the
    external supply shall feed the 5V pad through a Schottky or ideal
    diode, so it never drives the PC's VBUS and the PC never drives the
    supply. The diode's drop lowers the VBUS voltage the device sees. That
    is acceptable here, because the device is self-powered and uses VBUS
    only to detect attach (USBH-6).
- **HW-12. VBUS switch.** A current-limited 5 V load-switch module. Its
  limit shall be set between 100 mA and 500 mA. *(Part TBD.)*
  - **Enable.** On GPIO4 (pad D9), with an external pull-down of about
    100 kΩ. VBUS is then off while the RP2350 is in reset or unpowered
    (C1-7). An internal pull-down alone is not relied on.
  - **Overcurrent flag.** The switch's open-drain flag goes to GPIO3 (pad
    D10), with a pull-up.
  - **Firmware control.** Firmware turns VBUS on in C1-8, after `tuh_init()`.
    It turns it off before a BOOTSEL reboot (NU-5). It never cycles VBUS
    otherwise (USBH-9).
  - **Overcurrent handling.** Core 0 polls the flag once per epoch and
    counts each assertion. The switch limits the current itself, so the
    firmware takes no action (D-5).
- **HW-13. Line components** (hand-wired modules):
  - a 22 Ω series resistor in each of D+ (GPIO0) and D- (GPIO1), at the
    XIAO pads;
  - a 15 kΩ pull-down from each of D+ and D- to GND (USB host
    termination);
  - a low-capacitance ESD array on D+, D- and VBUS at the device
    connection;
  - wiring from the XIAO to the device connection kept short (no more than
    10 cm), with D+ and D- twisted together and run beside GND.
- **HW-14. Device connection.**
  - **Header.** The downstream device is hard-wired. A 4-pin header carries
    VBUS, D-, D+ and GND, with no host-side connector.
  - **USB-C devices.** If the device has a USB-C receptacle, it shall be
    connected with a USB-A-to-C cable cut to the header. The pull-up inside
    that cable makes the device see a host.
- **HW-15. Magnetic layout.** Following the datasheet's hardware design
  considerations (p. 12):
  - the 5 V supply wiring, the load switch, VBUS and the USB cable shall be
    routed away from the MMC5983MA;
  - no current-carrying wire shall pass under or over the sensor;
  - magnetised parts (connector shells, ferrite, battery) shall be kept
    clear of it.

  The ground-loop check (USBH-7a) also applies.
- **HW-9 (amended). Test pins.** Kept on D8 (GPIO2, the epoch marker) and D3
  (GPIO5). D9 and D10 now carry the VBUS enable and the overcurrent flag
  (HW-12).

## 6. Magnetometer (amendments)

The output is a SET/RESET pair of low-noise measurements every 20 ms. The XIAO
schedules the pair and computes H = (S − R) / 2. It does no averaging or
other filtering. Bandwidth limiting is left to the MMC5983MA's own BW = 00
filter.

- **M-3 (amended).** Internal Control 1: BW = 00. All other register
  settings are unchanged. Auto_SR_en stays off (M-3a), and so does
  continuous mode. Datasheet figures for BW = 00 (MMC5983MA Rev A,
  4/3/2019, pp. 2 and 15):
  - 8 ms measurement time and 100 Hz bandwidth. The BW bits set the length
    of the decimation filter;
  - 0.4 mG RMS noise;
  - a maximum output rate of 50 Hz.
- **M-20 (new). The 20 ms cycle.** The 1 kHz tick (M-6, M-6a) is kept as
  the schedule grid. Every 20 ticks form one cycle (k = 0 … 19). Two
  transactions run per cycle, both of DFC's read-then-trigger form (M-7)
  with a pulse:
  - **Tick 0:** read the previous cycle's RESET measurement, then SET
    pulse, then trigger the SET measurement (S).
  - **Tick 10:** read S, then RESET pulse, then trigger the RESET
    measurement (R).

  Other ticks start no transaction. They still run the M-16 timeout check.
- **M-21 (new). Spacing.**
  - **10 ms apart.** The S and R triggers are 10 ms apart, give or take the
    tick jitter. RESET inverts the sensor's response to the whole field.
    The two windows are half a 50 Hz period apart, so a 50 Hz field (and
    its odd harmonics) cancels in S − R.
  - **Margin.** Each measurement has 10 ms before it is read, against the
    8 ms it takes.
  - **60 Hz is not cancelled.** It aliases to 10 Hz. On a simple
    flat-window estimate, about 20 % of it reaches the output after the
    window and the pair.
  - **No other filtering.** The XIAO applies none (scope decision).
- **M-22 (new). The record.** Core 0 computes each record from the S and R
  of one cycle:
  - **Field:** H = (S − R) / 2 per axis, from the raw 18-bit counts. This
    cancels the bridge offset and its temperature drift. The record carries
    S − R exactly; the receiver halves it (REC-2).
  - **Offset (diagnostic):** O = (S + R) / 2 per axis. The record carries
    S + R − 262144 exactly; the receiver halves it (REC-2).
  - **Timestamp:** the S trigger time (M-10's definition). The record covers
    the time from the S trigger to the R trigger plus 8 ms.
  - **Sequence number:** the cycle index.
  - **Valid:** true only if both S and R are valid.
- **M-9 (amended). Reason codes.**
  - **`AFTER_RESET` removed.** An R measurement is inverted by design and
    is never marked invalid for that.
  - **`PAIR_INCOMPLETE` added.** One half of the pair was lost to an abort
    or timeout.
  - **Order.** `NO_PRIOR`, then `NOT_READY` (checked on both halves), then
    `PAIR_INCOMPLETE`.
- **M-4 / M-4b (replaced by M-20 to M-22).**
  - **Start-up.** Keep the single RESET → SET conditioning at start-up.
  - **Removed.** The periodic SET/RESET strategy (`g_mag_sr_mode`) and the
    periodic offset calibration, together with their counters (D-1).
- **M-23 (new). Datasheet limits and bench checks.**
  - **Rate limit.** The schedule (M-20) runs at the datasheet's limit for
    BW = 00. That is 100 measurements per second, one every 10 ms. It
    matches the continuous-mode maximum at BW = 00 (100 Hz, p. 16). Each
    pair gives one record at 50 Hz, the rated maximum output rate.
  - **Inconsistency.** The datasheet gives 8 ms per measurement in the BW
    table but says "10ms per measurement" in its power example (p. 12).
  - **Check 1, measurement time.** The real BW = 00 measurement time shall
    be measured, by polling Meas_M_Done after a trigger. The schedule needs
    it to be at most 9.5 ms, so that S is ready before the tick-10 read.
    If it is longer, the fallback is BW = 01: a 4 ms measurement, 0.6 mG.
    The spacing and the 50 Hz cancellation stay the same.
  - **Check 2, SET/RESET repeatability.** The datasheet gives SET/RESET
    repeatability as ±1 mG (p. 2), larger than the 0.4 mG noise. If that
    variation is random from pulse to pulse, pulsing before every
    measurement adds it to every record. Noise shall be measured in the
    pair mode, and in a reference mode for comparison: one SET at start-up,
    then single measurements at 50 Hz. Compare their RMS and Allan
    deviation before committing to M-20.
  - **Check 3, supply current.** Current scales with measurement rate. It is
    450 µA at 7 measurements per second (p. 2), which gives about 6.4 mA
    typical at 100 per second. Also add 100 SET or RESET pulses per second,
    drawn from CAP. The 3.3 V rail (USBH-7a) and the CAP capacitor (HW-5a)
    shall be checked under this load.
  - **Temperature.** Temperature (TM_T) and field (TM_M) measurements cannot
    run at the same time (p. 14). The 2 ms gaps in M-20 leave no confirmed
    slot for TM_T. Whether to report temperature is a record-format
    decision (F).
- **M-8 (amended).** Every transaction contains a pulse, so each shall
  complete within 450 µs of its tick. This shall be re-measured at 120 MHz
  (R-3).
- **M-16a (amended).** After a single hung transaction, valid records shall
  resume within 2 cycles (40 ms).
- **M-17 (amended).** A record is stale once it is more than 30 ms old
  (1.5 cycles).
- **M-2b (amended).** The not-ready fault score now counts per measurement:
  2 per cycle, instead of DFC's 1,000 per second. The default thresholds
  shall be re-derived. *(TBD.)*
- **Saturation (D-1).** The check applies to each raw S and R measurement.

## 7. Diagnostics and recovery

### 7.1 Recovery policy (D-5 replaced)

No flag or counter triggers an automatic action, except the actions listed
below.

**Normal operation.** These are not recovery:

- **Magnetometer (from DFC):**
  - aborting a failed or timed-out transaction and resuming the schedule
    (M-15, M-16);
  - the I2C bus clear (M-18);
  - re-probing at 5 Hz while the fault flag is set (M-2a);
  - setting the fault flag on persistent not-ready measurements (M-2b).
- **Enumeration retries.** TinyUSB repeats each failed enumeration step up
  to 3 times, 100 ms apart (USBH-9).
- **Transaction retries.** Pico-PIO-USB repeats an errored transaction up to
  3 times, and a NAK'd one on every frame (USBH-9).
- **New attach.** Enumeration on a new attach (USBH-9).
- **Stuck records.** Aborting a record that is still not accepted after
  20 ms, and moving on to the newest one (USBH-T6).
- **Halted endpoints.**
  - **Clear the halt.** When the device STALLs its bulk OUT or bulk IN
    endpoint while streaming, the host sends CLEAR_FEATURE
    (ENDPOINT_HALT), resets that endpoint's data toggle, and carries on
    with the newest record.
  - **Rate limit.** At most one halt clear per endpoint per second. A STALL
    that occurs within a second of the last clear is counted and left until
    the next second.
  - *(Verify how TinyUSB `86ad6e5` and `hcd_pio_usb` reset the toggle.)*

**Recovery.** The only automatic recovery action is the watchdog reset
(C1-7).
- **Every reset is counted** in watchdog scratch, which survives the reset.
- **No limit.** There is no cap on watchdog resets in a row: the firmware
  keeps resetting while the fault persists, and recovers by itself once it
  clears.
- **Visible.** A reset loop shows as changing `boot_id` values in the stream
  (REC-4), and as the watchdog-reset count in diagnostic status.

**Prohibited.** Everything else:
- VBUS cycling;
- host-initiated bus resets other than at a new attach;
- re-enumerating a device that is still attached;
- any software reset.

**Counted, with no action:**
- overcurrent (HW-12);
- unsupported devices (USBH-9);
- persistent transfer errors;
- an unsupported chip stepping (HW-10).

### 7.2 Counters (D-1 replaced)

**Core 0 counters.** Cleared only by the main loop, with an atomic
read-and-clear (D-2). The status reporter keeps totals since boot.

- **Magnetometer:**
  - I2C errors, missed ticks, timeouts;
  - product-ID faults, bus clears, recoveries, run-time faults
    (M-2a, M-2b);
  - not-ready measurements, saturated measurements, incomplete pairs
    (M-9).
- **Scheduling:** tick overruns (M-6a), epoch overruns (EX-8).
- **Hardware:** VBUS overcurrent assertions (HW-12).

**Core 1 counters.** Cumulative and never cleared; core 0 reports the
change between readings (IC-3).

- **Device:**
  - attaches, detaches;
  - enumeration failures, enumeration timeouts;
  - unsupported devices.
- **Transfers:**
  - transfer errors;
  - STALLs on bulk OUT and on bulk IN;
  - halt clears, and halt clears deferred by the rate limit.
- **Records:**
  - sent, late, aborted;
  - overwritten in the mailbox (IC-1);
  - missing slots (USBH-T5).
- **Other:**
  - bytes received on bulk IN and discarded (USBH-4a);
  - the SOF counter and loop heartbeat (C1-7).

**Timing maxima.** Reset only at boot.

- **Core 1:** frame-handler duration (C1-3); SOF interval minimum and
  maximum (C1-2); time from mailbox publish to queued (C1-4).
- **Core 0:** epoch work time and start latency (D-4); interrupt-handler
  and critical-section maxima (V-5); profiled sections (D-6). In diagnostic
  builds only, the status reporter and `tud_task()` time (NU-3).

**Persistent and static values:**
- the watchdog-reset count, kept in watchdog scratch and cleared at
  power-on (C1-7);
- the chip stepping (HW-10);
- the current `boot_id` (REC-4).

**Removed from DFC:** all UART counters, offset calibrations (M-4b) and
TX DMA completions (D-3a).

### 7.3 Status format (diagnostic builds, NU-2)

- **Rate.** Once per second, as a set of text lines.
- **Lines.** Each line starts with a line type and the report's sequence
  number, followed by `name=value` pairs for every counter, maximum and
  value in section 7.2.
- **Values.** Counter values are totals since boot.
- **Never blocks.** A line is written only when it fits whole in the CDC
  buffer.

## 8. Open decisions

1. *(Closed: section 5, USBH-1a to USBH-9.)* The downstream device is a
   self-powered CDC-ACM device at full speed, attached directly. Bulk
   endpoints are 64 bytes. The host sets the line coding and DTR/RTS, polls
   IN and discards the data, and treats unsupported devices as counted and
   left alone. The specific product is still to be named.
2. *(Closed: sections 5.1 and 6.)* Output timing and magnetometer mode:
   - 50 Hz output (20 ms cycle);
   - BW = 00;
   - a SET/RESET pair sequenced by the XIAO, with the measurements 10 ms
     apart and H = (S − R) / 2;
   - no XIAO averaging or filtering;
   - late records go out on the next frame, newest wins;
   - a missing record is not replaced;
   - bus tolerance ±10 µs.
3. *(Closed: R-3, USBH-1, USBH-S1 to USBH-S5.)* The stack is TinyUSB host
   with `cdc_host` on Pico-PIO-USB `fe9133f`. `clk_sys` is 120 MHz, and
   D+/D- are on GPIO0/1. Accepting TinyUSB amended USBH-3, USBH-4, USBH-4a
   and USBH-9 to match its behaviour.
4. *(Closed: section 5.3.)* Native USB:
   - diagnostic builds only;
   - status text only, with no data mirror;
   - BOOTSEL reboot by the 1200-baud touch and picotool's reset interface;
   - SDK default VID/PID for now, with a Raspberry Pi PID required before
     distribution.
5. *(Closed: B-26.)* The magnetometer driver is copied once from DFC
   `addc417`, adapted, and tracked by hand through `docs/PROVENANCE.md`.
   It is not a shared module.
6. *(Closed: section 7.)* Recovery and counters:
   - halted endpoints are cleared, at most once per second;
   - a stuck record is aborted after 20 ms;
   - watchdog resets are unlimited and counted;
   - the counter list and status format are consolidated.
7. *(Closed: IC-1 to IC-3, section 4.1.)* Core 1 execution model:
   - a triple-buffer mailbox, which core 1 polls in a tight loop;
   - single-writer counters;
   - SOF and frame-handler timing budgets;
   - liveness through a watchdog, fed by core 0 only while both cores make
     progress;
   - the start-up order.
8. *(Closed: section 5.2.)* Record format:
   - a 46-byte binary payload, COBS-framed into 48 bytes, one packet each;
   - raw counts, sent as S − R and S + R, little-endian;
   - CRC-16/CCITT-FALSE;
   - a random boot ID;
   - no temperature, and no status records.

9. *(Closed: section 5.4.)* Hardware:
   - A4 stepping only;
   - an external 5 V supply on the 5V pad, with diode back-feed protection
     if VBUS and the 5V pad are connected directly;
   - hand-wired modules: a current-limited VBUS switch (enable on D9 with
     an external pull-down, overcurrent flag on D10), 22 Ω series
     resistors, 15 kΩ pull-downs and an ESD array;
   - the device hard-wired through a 4-pin header.

## 9. Verification (to be written)

- **Bus capture.** Capture D+/D- for at least 10 minutes with a full-speed
  decoder and check every packet interval.
- **Re-run timing.** Run V-5 (ISR, critical-section and epoch timing) on core
  0 while core 1 is busy.
- **Fault tests.** Hot-plug loops, a device that stops reading, a device that
  STALLs, a VBUS short, and the E9 detach test.
- **Magnetometer.** Run V-5a and V-6, carried over from DFC.
