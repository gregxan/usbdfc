# usbdfc — XIAO RP2350 magnetometer to USB CDC (PIO-USB host)

Firmware for the Seeed XIAO RP2350 that reads an MMC5983MA magnetometer
and streams one record every 20 ms (50 Hz) to a downstream USB CDC-ACM
device. Each record is built from a SET/RESET pair of low-noise (BW = 00)
measurements, so the sensor's offset is cancelled. The XIAO is the
**USB host**, on a PIO-USB port.

**Status:** everything in the requirements is implemented:
- the production firmware;
- the diagnostic build, with a native-USB status and command port;
- the Pico 2 test-target firmware;
- the Python verification tools.

All of it builds with the project's strict warning set. Host-side tests pass
on the PC (`tests/host/`). Nothing has run on hardware yet. The requirements
are in [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md); section 8a lists every
assumption made during implementation.

Derived from the DFC firmware (Rev 2.16, dual UART + MMC5983MA). The
magnetometer code is a one-time copy ([docs/PROVENANCE.md](docs/PROVENANCE.md)).

## Architecture

- **Core 0:**
  - the 1 kHz tick on TIMER0;
  - the DMA-driven I2C transactions: tick 0 reads R, then SET and trigger
    S; tick 10 reads S, then RESET and trigger R;
  - 1 ms epochs at tick + 500 µs. The epoch at each cycle's tick 0 builds
    the previous cycle's record and publishes it to a triple-buffer
    mailbox;
  - the VBUS switch and the watchdog.
- **Core 1:**
  - TinyUSB host with the CDC-ACM driver, on Pico-PIO-USB;
  - SOF from a second TIMER0 alarm at tick + 600 µs, phase-locked to the
    magnetometer;
  - one 48-byte bulk OUT per record.

## Wiring

| Function | XIAO pad | GPIO |
|---|---|---|
| USB host D+ / D- (22 Ω series, 15 kΩ pull-down each) | D6 / D7 | 0 / 1 |
| VBUS switch enable (external ~100 kΩ pull-down) | D9 | 4 |
| VBUS switch overcurrent flag (open drain) | D10 | 3 |
| I2C1 SDA / SCL → MMC5983MA (3.3 V, addr 0x30) | D4 / D5 | 6 / 7 |
| Test pins (diag presets): epoch marker / R-trigger marker | D8 / D3 | 2 / 5 |

Power the board from an external 5 V supply on the 5V pad (HW-11). Use an
A4-stepping RP2350 (HW-10).

## Repository layout

```
CMakeLists.txt            build definition
CMakePresets.json         release / debug / diag (status port, markers, stall hook)
cmake/                    toolchain discovery, SDK import, B-25 exceptions
lib/pico-sdk/             Pico SDK 2.2.0 (with TinyUSB 86ad6e5), submodule
lib/Pico-PIO-USB/         Pico-PIO-USB fe9133f, submodule
src/main.c                start-up order, core 0 epoch loop, VBUS, watchdog
src/mag.[ch]              MMC5983MA: DMA transactions, 20 ms pair cycle
src/tick.[ch]             1 kHz TIMER0 tick (the schedule grid)
src/record.[ch]           46-byte record, CRC-16, COBS framing
src/mailbox.[ch]          core 0 -> core 1 triple-buffer mailbox
src/usbh.[ch]             core 1: SOF alarm, TinyUSB CDC host, streaming
src/tusb_config.h         TinyUSB configuration
src/usbd_diag.[ch]        diag builds: native-USB CDC + reset interface
src/status.[ch]           diag builds: status lines and commands
src/diag.[ch], prof.[ch]  counters and timing instrumentation
test_target/              Pico 2 test-target firmware (VE-1, VE-2)
tests/host/               no-hardware tests (record encoder, tools)
tools/                    capture, timing, noise and control tools (9.5)
docs/                     requirements, provenance
```

## Build (Windows, same setup as DFC)

```bash
git clone --recurse-submodules https://github.com/gregxan/usbdfc C:\usbdfc
cmake --preset release
cmake --build --preset release
```

The output is `build/release/firmware.uf2`. For toolchain installation,
follow the DFC README; the steps are the same. Initialise the submodules
recursively: TinyUSB is a submodule of the SDK.

Diagnostic build: `cmake --preset diag && cmake --build --preset diag`.
Its native USB-C port is a serial port carrying status lines once per
second. It also accepts commands (`tools/diag_ctl.py`), the 1200-baud
touch and `picotool reboot -f`.

Test target (Pico 2):

```bash
cd test_target
cmake --preset release
cmake --build --preset release
```

Then flash `test_target/build/release/test_target.uf2`.

## Bench wiring for verification

- **Test target USB:** XIAO host header (VBUS, D-, D+, GND) to the
  Pico 2's USB port.
- **Test target UART:** Pico 2 GP0 (TX) / GP1 (RX) to a USB-serial
  adapter, 921600 baud.
- **Marker:** XIAO D3 to Pico 2 GP2, diag build only.
- **Coil (V-19):** Pico 2 GP3, through a transistor and series resistor.

Then, for example, the V-1 soak and the V-2/V-3 timing checks:

```bash
python tools/rx_decode.py --port COM7 --seconds 300 --log raw.txt --csv rec.csv
python tools/timing_report.py raw.txt --meas-ms 8.0
```

## Host tests

See [tests/host/README.md](tests/host/README.md).

## License

[0BSD](LICENSE): anyone may use, copy, modify and distribute this code, with
or without a fee, and nothing has to be kept or credited. Third-party
components keep their own licences, and their copyright notices must go with
any binary you distribute: the Pico SDK (BSD-3-Clause), TinyUSB (MIT) and
Pico-PIO-USB (MIT).
