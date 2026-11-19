# usbdfc — XIAO RP2350 magnetometer to USB CDC (PIO-USB host)

Firmware for the Seeed XIAO RP2350 that reads an MMC5983MA magnetometer
and streams one record every 20 ms (50 Hz) to a downstream USB CDC-ACM
device. Each record is built from a SET/RESET pair of low-noise (BW = 00)
measurements, so the sensor's offset is cancelled. The XIAO is the
**USB host**, on a PIO-USB port.

**Status:** the production firmware builds with the project's strict
warning set. It has not yet run on hardware. Not yet implemented: the native
USB diagnostic port (NU-1 to NU-6), the test-target firmware (VE-1) and
most of the Python tools (section 9.5). The requirements are in
[docs/REQUIREMENTS.md](docs/REQUIREMENTS.md). Section 8a lists every
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
CMakePresets.json         release / debug / diag / diag-single / diag-spacing5
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
src/diag.[ch], prof.[ch]  counters and timing instrumentation
tests/host/               host-side test of the record encoder
tools/rx_decode.py        record decoder and stream checker (V-1)
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

## Host test

```bash
gcc -std=c11 -Wall -Wextra -Werror -Wconversion -Itests/host/stubs -Isrc \
    -o test_record tests/host/test_record.c src/record.c
./test_record > frames.txt
python tools/rx_decode.py --hexfile frames.txt
```

## License

[0BSD](LICENSE): anyone may use, copy, modify and distribute this code, with
or without a fee, and nothing has to be kept or credited. Third-party
components keep their own licences, and their copyright notices must go with
any binary you distribute: the Pico SDK (BSD-3-Clause), TinyUSB (MIT) and
Pico-PIO-USB (MIT).
