# usbdfc — XIAO RP2350 magnetometer to USB CDC (PIO-USB host)

Firmware for the Seeed XIAO RP2350 that samples an MMC5983MA magnetometer on
a fixed, jitter-bounded schedule and streams each sample to a downstream USB
CDC device. The XIAO is the **USB host**, on a PIO-USB port.

**Status: shell project.** It builds an empty main loop. The requirements are a
draft ([docs/REQUIREMENTS.md](docs/REQUIREMENTS.md)).

Derived from the DFC firmware (Rev 2.16, dual UART + MMC5983MA). The UART
requirements are dropped. The magnetometer and build requirements carry over.

## Planned architecture

- **Core 0:** a 1 kHz magnetometer tick, DMA-driven I2C transactions, and an
  epoch loop that produces one record per output period into an inter-core
  ring.
- **Core 1:** a dedicated PIO-USB host. SOF is phase-locked to the
  magnetometer tick grid, and each output period sends one bulk OUT record.
- **Native USB:** a CDC device to a PC, for diagnostics and flashing.

## Repository layout

```
CMakeLists.txt          build definition (inherited from DFC B-*)
CMakePresets.json       release / debug presets
cmake/                  toolchain and host-tool discovery, SDK import
lib/pico-sdk/           Pico SDK 2.2.0, git submodule
src/main.c              placeholder main loop
docs/REQUIREMENTS.md    draft requirements (Rev 3.0 baseline)
```

## Build (Windows, same setup as DFC)

```bash
git clone --recurse-submodules <repo-url> C:\usbdfc
cmake --preset release
cmake --build --preset release
```

The output is `build/release/firmware.uf2`. For toolchain installation, follow
the DFC README; the steps are identical.

## License

[0BSD](LICENSE): anyone may use, copy, modify and distribute this code, with
or without a fee, and nothing has to be kept or credited. Third-party
components keep their own licences, and their copyright notices must go with
any binary you distribute: the Pico SDK (BSD-3-Clause), and later Pico-PIO-USB
and TinyUSB (MIT).
