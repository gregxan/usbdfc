# usbdfc Requirements — DRAFT

**Revision 3.0-draft.** The baseline is DFC Rev 2.16. Nothing in this
document is final until the open decisions in section 8 are closed.

## 1. Scope

Firmware for a Seeed XIAO RP2350 that:

- Samples an MMC5983MA magnetometer on a fixed schedule.
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

- **HW-8 (replaced).** Core 0 runs the magnetometer, the application and the
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
- **IC-1 (new).** Core 0 and core 1 exchange data through a lock-free
  single-producer, single-consumer record ring. No hardware spinlocks are
  used in the data path. The depth is 2 and the policy is drop-oldest, with
  every drop counted.
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

- **USBH-T1.** One record per output period. A bulk OUT shall start no later
  than SOF + X µs. The interval from packet to packet shall be P ± 20 µs,
  measured on the bus. P is 1 ms or 2 ms (section 8).
- **USBH-T2.** SOF shall be generated from the magnetometer tick's timer grid,
  at a fixed offset, each one scheduled from the previous target time.
- **USBH-T3.** Core 0's epoch work shall finish before SOF minus a margin.
- **USBH-T4.** Freshness: the age from trigger to bus shall be at most
  1.5 ms. *TBD*.
- **USBH-T5.** Every slot carries a record. An invalid or missing sample is
  sent with its reason code and is never skipped.
- **USBH-T6.** Every dropped record is counted.
- **USBH-T7 (requirement on the downstream device).** The device shall accept
  one packet of up to 64 bytes in every output period, continuously.

### 5.2 Record format

The record is binary, framed with COBS, and protected by a CRC-16. Its
fields:

- sequence number;
- trigger timestamp;
- X/Y/Z, 16-bit or 18-bit (*TBD*);
- reason code and stale flag.

## 6. Magnetometer (amendments)

- **M-6 / M-6a.** Keep the 1 kHz tick. At a 2 ms output period, sample on
  even ticks and use the odd ticks for SET/RESET or a second measurement.
- **M-4 / M-4b.** At a 2 ms period, consider SET/RESET differencing on every
  sample: H = (S − R) / 2.
- **M-8, M-16a, M-17.** Re-derive the deadlines and the staleness limit for
  the chosen output period.

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
- core 1 frame-handler timing maximum

## 8. Open decisions

1. *(Closed: section 5, USBH-1a to USBH-9.)* The downstream device is a
   self-powered CDC-ACM device at full speed, attached directly. Bulk
   endpoints are 64 bytes. The host sets the line coding and DTR/RTS, polls
   IN and discards the data, and treats unsupported devices as counted and
   left alone. The specific product is still to be named.
2. The output period: 1 ms or 2 ms. A 2 ms period also needs the per-sample
   mode decided: single, averaged, or SET/RESET differenced.
3. *(Closed: R-3, USBH-1, USBH-S1 to USBH-S5.)* The stack is TinyUSB host
   with `cdc_host` on Pico-PIO-USB `fe9133f`. `clk_sys` is 120 MHz, and
   D+/D- are on GPIO0/1. Accepting TinyUSB amended USBH-3, USBH-4, USBH-4a
   and USBH-9 to match its behaviour.
4. Whether native USB also carries the sample stream to a PC.
5. Whether to extract the magnetometer driver into a module shared with DFC.
6. D-5: which host retries, STALL clears and re-enumerations count as normal
   operation.

## 9. Verification (to be written)

- **Bus capture.** Capture D+/D- for at least 10 minutes with a full-speed
  decoder and check every packet interval.
- **Re-run timing.** Run V-5 (ISR, critical-section and epoch timing) on core
  0 while core 1 is busy.
- **Fault tests.** Hot-plug loops, a device that stops reading, a device that
  STALLs, a VBUS short, and the E9 detach test.
- **Magnetometer.** Run V-5a and V-6, carried over from DFC.
