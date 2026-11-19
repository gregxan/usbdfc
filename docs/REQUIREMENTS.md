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

- **HW-8 (replaced).** Core 0 runs the magnetometer and the application. Core
  1 is dedicated to the PIO-USB host. Core 1 handles no interrupts other
  than its frame timer, and its stack and hot data live in SCRATCH_X/Y.
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
- **R-3 (replaced).** `clk_sys` shall be a multiple of 12 MHz that the pinned
  Pico-PIO-USB version supports. The candidate is 120 MHz. The I2C, tick,
  M-8 and EX-4 timings shall be re-verified at the chosen clock.

## 5. USB

- **USBH-1.** The host port is PIO-USB on a consecutive GPIO pair. The
  candidates are GPIO0/1 (D6/D7) and GPIO20/21. *(Pin choice TBD.)*
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
- **USBH-3.** Enumeration is a non-blocking state machine timed by the tick.
  It shall complete within 500 ms of attach. *(Limit TBD by measurement.)*
- **USBH-4.** After SET_CONFIGURATION, the host shall send:
  1. SET_LINE_CODING: 115,200 baud, 8 data bits, no parity, 1 stop bit.
     Native-USB CDC devices ignore the value; it is sent for devices that
     expect it.
  2. SET_CONTROL_LINE_STATE with DTR = 1 and RTS = 1.

  Streaming starts once both have completed. A STALL on either request is
  counted and tolerated, and streaming starts anyway: both requests are
  optional in CDC-ACM.
- **USBH-4a.** The host shall poll the bulk IN endpoint once per output
  period, after that period's data OUT and within the remaining frame
  budget. This keeps the device's transmit buffer from filling. Bytes
  received are counted and discarded. A NAK is the expected, normal
  response. The CDC notification (interrupt IN) endpoint is not polled.
- **USBH-5.** Max packet size:
  - **Bulk endpoints.** Both bulk endpoints shall report a maximum packet
    size of 64 bytes. A device reporting less is unsupported (USBH-9). One
    record fits in one packet.
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
- **USBH-9.** On an unsupported device, or a failed enumeration (timeout,
  STALL or error on a standard request), the host shall:
  1. increment the matching counter (unsupported device, or enumeration
     failure);
  2. leave the device unconfigured, with no retries, VBUS cycling or bus
     reset;
  3. wait for detach.

  A new attach starts a new enumeration. This is a new event, not a retry,
  so it is normal operation under D-5.
- **B-19 / B-20 (rewritten).** Native USB is a CDC device to a PC, for
  status and flashing. It may stay enabled in production.

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
- enumeration failures
- unsupported devices (USBH-9)
- STALLs on SET_LINE_CODING or SET_CONTROL_LINE_STATE (USBH-4)
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
3. The Pico-PIO-USB version and `clk_sys`.
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
