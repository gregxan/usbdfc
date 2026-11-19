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

## 3. Removed from DFC Rev 2.16

- HW-2, HW-3, HW-6, HW-6a (UART pins).
- Section 4 (U-1 … U-18): all UART requirements.
- The UART parts of EX-5, EX-9, D-1, D-3 and D-3a.
- The UART parts of V-1 to V-4.
- The U-8 and U-15a dependency in M-6. The tick stays, for the magnetometer.

## 4. Execution model (amendments)

- **HW-8 (replaced; detail in section 4.1).** Core 0 runs the magnetometer, the application and the
  native USB device. Core 1 is dedicated to the PIO-USB host: the frame
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
- **USBH-7.** VBUS hardware: a 5 V load switch with a current limit and an
  overcurrent flag, back-feed protection, series resistors on D+/D-,
  15 kΩ pull-downs, and ESD protection.
- **USBH-7a.** Self-powered device effects, to be checked in the hardware
  design:
  - **Ground loop.** The USB ground joins the two supplies. This shall not
    degrade the MMC5983MA noise floor (measure with and without the device
    attached).
  - **Unpowered XIAO.** With the XIAO unpowered and the device powered, D+/D-
    shall not back-power the RP2350. A compliant device keeps its pull-up
    off without VBUS; a non-compliant one shall not damage the XIAO.
- **USBH-8.** RP2350-E9 risk: detach detection shall be verified on the
  actual chip stepping.
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
- **B-19 / B-20 (rewritten).** Native USB is a CDC device to a PC, for
  status and flashing. It may stay enabled in production.

### 5.0 Stack and resources

- **USBH-S1.** The host stack is TinyUSB's host core, with `cdc_host` (ACM
  only) and the `hcd_pio_usb` port driver, running on Pico-PIO-USB.
  - **Root ports.** TinyUSB runs in dual-role mode. Root port 0 is the
    native USB device, served by `tud_task()` on core 0. Root port 1 is the
    PIO-USB host (`CFG_TUH_RPI_PIO_USB = 1`), served by `tuh_task()` on
    core 1.
  - **Class options.** `CFG_TUH_CDC_FTDI`, `CFG_TUH_CDC_CP210X` and
    `CFG_TUH_CDC_CH34X` are 0, and `CFG_TUH_HUB` is 0.
  - **Write path.** Only core 1 calls `tuh_cdc_write()` and
    `tuh_cdc_write_flush()`, taking records from the IC-1 ring.
- **USBH-S1a.** *(Verification item.)* TinyUSB's shared core code is used
  from two cores at once: device on core 0, host on core 1. Its
  thread-safety across cores shall be reviewed and tested. If it is unsafe,
  the fallback is to drop the native USB device from production builds.
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
  - **Counted.** Late records and overwritten records are counted.
- **USBH-T7 (requirement on the downstream device).** The device shall accept
  one packet of up to 64 bytes per 20 ms period, continuously.
- **USBH-T8. Start and restart.**
  - **When the cadence applies.** USBH-T1 applies from the first record
    sent after the device mounts. That record carries a stream-start flag.
  - **Sequence numbers.** A record's sequence number is its cycle index
    since boot (M-20). It keeps counting across a detach and re-attach, so
    the device can see what it missed. A reboot restarts it from zero.
  - **Boot identity.** Each record carries a boot identifier: the
    watchdog-reset count (C1-7) plus a power-on count, *format TBD in the
    record-format decisions (F)*. A reboot is therefore never mistaken for
    a sequence wrap.

### 5.2 Record format

The record is binary, framed with COBS, and protected by a CRC-16. Its
fields:

- sequence number;
- trigger timestamp;
- X/Y/Z, 16-bit or 18-bit (*TBD*);
- reason code and stale flag.

## 6. Magnetometer (amendments)

The output is a SET/RESET pair of low-noise measurements every 20 ms. The XIAO
schedules the pair and computes H = (S − R) / 2. It does no averaging or
other filtering. Bandwidth limiting is left to the MMC5983MA's own BW = 00
filter.

- **M-3 (amended).** Internal Control 1: BW = 00, which is an 8 ms
  measurement. All other register settings are unchanged. Auto_SR_en stays
  off (M-3a), and so does continuous mode.
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
    cancels the bridge offset and its temperature drift.
  - **Offset (diagnostic):** O = (S + R) / 2 per axis, sent in the record.
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

## 7. Diagnostics (new counters)

- attach and detach events
- enumeration failures and enumeration timeouts (USBH-3, USBH-9)
- unsupported devices (USBH-9)
- bytes received on bulk IN and discarded (USBH-4a)
- transfer errors: CRC, timeout, babble, STALL
- NAK-full events
- dropped records
- ring overflows
- VBUS overcurrent
- core 1 frame-handler duration maximum, and SOF interval minimum and
  maximum (C1-2, C1-3)
- maximum time from mailbox publish to queued (C1-4)
- records overwritten in the mailbox (IC-1)
- late records, and missing slots (USBH-T5, USBH-T6)
- incomplete pairs (M-9)
- core 1 SOF counter and loop heartbeat (cumulative; C1-7)
- watchdog resets (kept across reset in watchdog scratch; C1-7)

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
4. Whether native USB also carries the sample stream to a PC.
5. Whether to extract the magnetometer driver into a module shared with DFC.
6. D-5: which host retries, STALL clears and re-enumerations count as normal
   operation. *(Partly closed: USBH-9 covers the TinyUSB and Pico-PIO-USB
   retries, and C1-7 adds the watchdog-reset exception.)*
7. *(Closed: IC-1 to IC-3, section 4.1.)* Core 1 execution model:
   - a triple-buffer mailbox, which core 1 polls in a tight loop;
   - single-writer counters;
   - SOF and frame-handler timing budgets;
   - liveness through a watchdog, fed by core 0 only while both cores make
     progress;
   - the start-up order.

## 9. Verification (to be written)

- **Bus capture.** Capture D+/D- for at least 10 minutes with a full-speed
  decoder and check every packet interval.
- **Re-run timing.** Run V-5 (ISR, critical-section and epoch timing) on core
  0 while core 1 is busy.
- **Fault tests.** Hot-plug loops, a device that stops reading, a device that
  STALLs, a VBUS short, and the E9 detach test.
- **Magnetometer.** Run V-5a and V-6, carried over from DFC.
