# usbdfc: XIAO RP2350 magnetometer to USB CDC (PIO-USB host)

Firmware for the Seeed XIAO RP2350. It reads an MMC5983MA magnetometer and
sends one record every 20 ms (50 Hz) to a downstream USB CDC-ACM device.
Each record comes from a SET/RESET pair of low-noise (BW = 00)
measurements, so the sensor's bridge offset and its temperature drift
cancel. The XIAO is the **USB host**, on a PIO-USB port driven by its
second core.

**Status:** everything in the requirements is implemented:
- the production firmware;
- the diagnostic build, with a native-USB status and command port;
- the Pico 2 test-target firmware;
- the Python verification tools.

All of it builds with the project's strict warning set, and the host-side
tests pass (`tests/host/`). **Nothing has run on hardware yet.**

| Document | Contents |
|---|---|
| [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) | Requirements (Rev 3.0-draft). Section 8a lists every assumption made during implementation. Section 9 is the verification plan. |
| [docs/VERIFY_PROMPT.md](docs/VERIFY_PROMPT.md) | Instructions for an independent code review (round 1). Round 2: [VERIFY_SESSIONS.md](docs/VERIFY_SESSIONS.md) |
| [docs/PROVENANCE.md](docs/PROVENANCE.md) | Origin of the copied magnetometer code, and how upstream changes are tracked |
| [tests/host/README.md](tests/host/README.md) | Tests that need no hardware |

## How it works

- **Core 0:**
  - **Tick.** A 1 kHz tick on TIMER0 forms the schedule grid.
  - **Magnetometer.** Two DMA-driven I2C transactions per 20 ms cycle:
    - at tick 0: read R, then a SET pulse, then trigger S;
    - at tick 10: read S, then a RESET pulse, then trigger R.
  - **Records.** Work runs in 1 ms epochs at tick + 750 µs. At each cycle's
    tick 0, the epoch builds the previous cycle's 46-byte record and hands
    it to core 1 through a triple-buffer mailbox.
  - **Housekeeping.** Core 0 also drives the VBUS switch, counts
    overcurrent and feeds the watchdog.
- **Core 1:**
  - TinyUSB host with its CDC-ACM driver, on Pico-PIO-USB;
  - SOF generated from a second TIMER0 alarm at tick + 850 µs, so the bus
    stays phase-locked to the magnetometer;
  - each record COBS-framed and sent in one 48-byte bulk OUT.
- **Native USB-C port:**
  - production builds leave it unused;
  - in the diagnostic build it is a serial port carrying status lines,
    commands and the reset interface for flashing.

Record format: 46-byte little-endian payload, CRC-16/CCITT-FALSE, COBS
framing (docs/REQUIREMENTS.md section 5.2). `tools/rx_decode.py` decodes
it.

## Hardware

### Parts

- **Seeed XIAO RP2350 with an A4-stepping RP2350 (HW-10).** Earlier
  steppings have erratum E9, which can break USB detach detection. The
  firmware reports the stepping: the `chip=` field in the diagnostic status
  (4 = A4). On other steppings it sets bit 4 of every record's `flags`.
- **MMC5983MA breakout** (3.3 V, I2C address 0x30).
- **Regulated 5 V supply,** at least 250 mA (HW-11).
- **USB host port parts** (HW-12, HW-13):
  - a current-limited 5 V load-switch module, with its limit set to about
    150–250 mA, an active-high enable and an open-drain overcurrent flag;
  - two 22 Ω resistors and two 15 kΩ resistors;
  - a low-capacitance USB ESD array;
  - a 4-pin header (VBUS, D-, D+, GND);
  - a ~100 kΩ resistor (VBUS enable pull-down).

### XIAO wiring

| Function | XIAO pad | GPIO | Notes |
|---|---|---|---|
| USB host D+ | D6 | 0 | 22 Ω series resistor at the pad; 15 kΩ to GND |
| USB host D- | D7 | 1 | 22 Ω series resistor at the pad; 15 kΩ to GND |
| VBUS switch enable | D9 | 4 | External ~100 kΩ pull-down, so VBUS is off in reset |
| VBUS switch overcurrent flag | D10 | 3 | Open drain; internal pull-up |
| I2C1 SDA → MMC5983MA | D4 | 6 | Pull-up to 3.3 V |
| I2C1 SCL → MMC5983MA | D5 | 7 | Pull-up to 3.3 V |
| Test pin: epoch marker (diag build) | D8 | 2 | High while each epoch's work runs |
| Test pin: R-trigger marker (diag build) | D3 | 5 | Toggles at each R trigger (V-3) |
| 5 V supply | 5V pad | | See back-feed below |

**USB host port.**
- Keep the wiring from the XIAO pads to the header to 10 cm or less, with
  D+ and D- twisted together and run beside GND. Put the ESD array at the
  header.
- **The downstream device** is hard-wired to the header. If it has a USB-C
  port, use a USB-A-to-C cable cut to the header: the pull-up inside the
  cable makes the device see a host.
- **Self-powered devices.** The firmware still supplies VBUS, which such a
  device uses to detect attach.

**Magnetometer.**
- Fit SDA and SCL pull-ups to 3.3 V if the breakout has none: 4.7 kΩ at
  most, 2.7 kΩ recommended. Check that the rise time is ≤ 300 ns.
- Check the breakout's schematic for:
  - SPI_CS tied to VDDIO;
  - a 10 µF low-ESR capacitor on CAP;
  - 1 µF on VDD.
- Leave INT unconnected.
- Keep the 5 V wiring, the load switch, VBUS and the USB cable away from
  the sensor, and don't route current-carrying wires over or under it.

**Power and back-feed.**
- The 5 V supply feeds the XIAO's 5V pad and, through the load switch, the
  device's VBUS.
- Check on the Seeed schematic whether the XIAO's USB-C VBUS connects
  directly to the 5V pad. If it does, feed the external supply through a
  Schottky or ideal diode. Then plugging a PC into USB-C (diagnostic
  builds) can never back-feed the supply, or the supply the PC.

**Other.** Flash is set to the board's actual 2 MB; the SDK board file
defaults to 4 MB, and `CMakeLists.txt` overrides it. Before moving any pin,
read the pin notes at the top of `src/config.h`.

### Verification bench (optional)

- **Test target.** A Raspberry Pi Pico 2 running `test_target/` takes the
  place of the downstream device. It timestamps SOFs, packets and markers,
  and can inject faults on command.

| Test target (Pico 2) | Connects to |
|---|---|
| USB port | XIAO host header: VBUS, D-, D+, GND |
| GP0 (UART TX), GP1 (UART RX) | USB-serial adapter (3.3 V logic) RX / TX, 921600 baud |
| GP2 | XIAO D3 (R-trigger marker, diag build) |
| GP3 | Coil driver: transistor and series resistor into about 50 turns of 10 cm diameter (V-19) |
| GND | XIAO GND |

- **Logic analyzer.** For final timing evidence, use one with at least
  100 MS/s and USB full-speed and I2C decoding (VE-3). Until then, the
  tools use the test target's timestamps.

## Repository layout

```
CMakeLists.txt            firmware build definition
CMakePresets.json         release / debug / diag
cmake/host_tools.cmake    finds the Arm toolchain, picotool and pioasm
cmake/pico_sdk_import.cmake
cmake/third_party_warnings.cmake   per-file warning exceptions (B-25)
lib/pico-sdk/             Pico SDK 2.2.0 (includes TinyUSB 86ad6e5), submodule
lib/Pico-PIO-USB/         Pico-PIO-USB fe9133f, submodule
src/main.c                start-up order, core 0 epoch loop, VBUS, watchdog
src/mag.[ch]              MMC5983MA: DMA transactions, 20 ms SET/RESET cycle
src/tick.[ch]             1 kHz TIMER0 tick
src/record.[ch]           46-byte record, CRC-16, COBS framing
src/mailbox.[ch]          core 0 -> core 1 triple-buffer mailbox
src/usbh.[ch]             core 1: SOF alarm, TinyUSB CDC host, streaming
src/usbd_diag.[ch]        diag build: native-USB CDC + reset interface
src/status.[ch]           diag build: status lines and commands
src/diag.[ch], prof.[ch]  counters and timing instrumentation
src/config.h              pins and compile-time parameters
src/tusb_config.h         TinyUSB configuration
test_target/              Pico 2 test-target firmware (separate CMake project)
tests/host/               tests that need no hardware
tools/                    Python capture, timing, noise and control tools
.vscode/                  build and flash tasks, SWD debug configurations
```

## Setup on Windows (reference: VS Code extension)

1. Install **Git for Windows**, **Python 3.12** and **VS Code**.
2. **Turn on long paths, and clone into a short path with no spaces.** The
   nested submodules make paths long.
   ```bash
   git config --global core.longpaths true
   ```
   ```bash
   git clone --recurse-submodules https://github.com/gregxan/usbdfc C:\usbdfc
   ```
   If you already cloned without submodules, run this inside the clone.
   TinyUSB is a submodule of the SDK, so `--recursive` is required:
   ```bash
   git submodule update --init --recursive
   ```
3. **Install the toolchain and tools with the Raspberry Pi Pico extension.**
   The extension only downloads tools when it creates or imports a project,
   so let it create a throwaway project once:
   1. In VS Code, install the **Raspberry Pi Pico** extension
      (`raspberry-pi.raspberry-pi-pico`) and the **CMake Tools** extension.
   2. Open the Raspberry Pi Pico view (its icon in the left Activity Bar)
      and choose **New C/C++ Project**.
   3. Fill it in like this:
      - Name: anything, e.g. `pico-tools-bootstrap`.
      - Board: **Seeed XIAO RP2350**, or Pico 2 if it isn't listed.
      - Location: a scratch folder such as `C:\scratch`, not this repo.
      - SDK version: **2.2.0**.
      - Under advanced options, if shown: toolchain **14.2.Rel1** and
        picotool **2.2.0**.
   4. Click **Create** and wait until the downloads finish. This takes a
      few minutes the first time.
   5. Check that `%USERPROFILE%\.pico-sdk` now contains `sdk`, `toolchain`,
      `picotool`, `tools`, `openocd`, `cmake` and `ninja` folders.
   6. Close and delete the throwaway project. The tools stay installed.
   7. **Put the extension's CMake and Ninja on `PATH`.** The extension only
      uses them inside its own projects.
      1. List their folders in PowerShell:
         ```bash
         Get-ChildItem $env:USERPROFILE\.pico-sdk\cmake, $env:USERPROFILE\.pico-sdk\ninja
         ```
      2. Add the folders that **contain the `.exe` files**, not their parent
         `cmake` and `ninja` folders. For example:
         - `C:\Users\<you>\.pico-sdk\cmake\v3.31.5\bin` (contains
           `cmake.exe`);
         - `C:\Users\<you>\.pico-sdk\ninja\v1.12.1` (contains `ninja.exe`;
           there is no `bin` subfolder).

         Use the version folder names the listing showed. To add them:
         Start → "Edit environment variables for your account" → `Path` →
         Edit → New.
      3. Open a **new** terminal (existing ones keep the old `PATH`), fully
         restart VS Code, and check:
         ```bash
         cmake --version
         ```
         ```bash
         ninja --version
         ```

      If you already have CMake ≥ 3.21 and Ninja installed, skip this step.

   **What this project uses from the extension.** It doesn't use the SDK
   copy the extension downloads: it builds against its own pinned submodule
   in `lib/pico-sdk`. It needs only the extension's **toolchain and host
   tools**, which `cmake/host_tools.cmake` finds under
   `%USERPROFILE%\.pico-sdk` automatically.

   **Don't use the extension's *Import Project* on this repository.**
   Import rewrites `CMakeLists.txt` and `.vscode/` with the extension's own
   settings, which would replace this project's presets, tasks, launch
   configuration and tool discovery. If VS Code offers to import or
   configure the folder for the Pico extension, decline.

   Menu names vary slightly between extension versions. If anything
   differs, the goal is simply to end up with the tools in
   `%USERPROFILE%\.pico-sdk`.
4. **Install the Python tools' dependency:**
   ```bash
   py -m pip install -r tools\requirements.txt
   ```
5. **Open this repository's folder** in VS Code: *File → Open Folder…*, and
   choose the folder that contains `CMakeLists.txt` and `CMakePresets.json`
   (e.g. `C:\usbdfc`).
6. **Trust the folder.** When VS Code asks *"Do you trust the authors of
   the files in this folder?"*, choose **Yes, I trust the authors**. In
   *Restricted Mode*, shown in the title bar, the CMake Tools and Pico
   extensions are disabled, and none of the commands below exist. If you
   dismissed the prompt, use *Manage → Trust* from the Restricted Mode
   banner, or run **Workspaces: Manage Workspace Trust** from the Command
   Palette.
7. **Select a preset and build.** CMake Tools reads `CMakePresets.json`.
   Use either route:
   - **Command Palette** (**Ctrl+Shift+P**): run **CMake: Select Configure
     Preset** and pick **Release (production)**. Then run **CMake: Build**,
     or press **F7**.
   - **CMake side panel:** click the **CMake** icon (a triangle) in the left
     Activity Bar. Under **Project Status → Configure**, click the preset
     line ("No configure preset selected") and pick **Release
     (production)**. Then click **Build** in the same panel.

   The first configure takes a minute or two. The output is
   `build\release\firmware.uf2`. The *Terminal → Run Task…* menu has the
   same builds as tasks (`.vscode/tasks.json`).

   If the CMake commands don't appear:
   - check that **CMake Tools** (`ms-vscode.cmake-tools`) is installed and
     enabled;
   - check that the folder is trusted;
   - check that you opened the top folder, the one with `CMakeLists.txt`.

## Building from the command line (PowerShell or cmd)

The same commands work in either shell and in VS Code:

```bash
cmake --preset release
```
```bash
cmake --build --preset release
```

The output is `build\<preset>\firmware.uf2`, plus `.elf`, `.bin` and `.dis`.

| Preset | Contents | Use |
|---|---|---|
| `release` | Production: `-O3 -g`; native USB unused; test pins untouched | Units in service; timing measurements of the production image |
| `diag` | Release optimisation, plus: the native-USB status and command port, the D8/D3 test pins, and the V-14 watchdog stall hook | Bench work and verification (section 9), the V-18 and V-19 mode commands |
| `debug` | `CMAKE_BUILD_TYPE=Debug` (`-O0`) | Stepping in the debugger only. Not for timing. |

**Test target** (a separate CMake project, for a Pico 2):

```bash
cd test_target
cmake --preset release
cmake --build --preset release
```

The output is `test_target\build\release\test_target.uf2`.

## Manual setup (tools installed individually)

Install these versions. The toolchain, picotool and pioasm match what the
VS Code extension installs for SDK 2.2.0.

| Tool | Version | Notes |
|---|---|---|
| Pico SDK | **2.2.0** (submodule, commit `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`) | Already pinned in `lib/pico-sdk` |
| TinyUSB | `86ad6e5` | Pinned by the SDK, in `lib/pico-sdk/lib/tinyusb` |
| Pico-PIO-USB | `fe9133f` (2024-08-26) | Already pinned in `lib/Pico-PIO-USB` |
| Arm GNU Toolchain | **14.2.Rel1**, Windows-hosted, `arm-none-eabi` | developer.arm.com |
| CMake | **3.31.6** (≥ 3.21 required) | |
| Ninja | **1.12.1** | |
| picotool | **2.2.0**, prebuilt Windows binary | github.com/raspberrypi/pico-sdk-tools releases |
| pioasm | **2.2.0**, prebuilt Windows binary | github.com/raspberrypi/pico-sdk-tools releases |
| OpenOCD | **0.12.0+dev**, Raspberry Pi fork, prebuilt | github.com/raspberrypi/pico-sdk-tools releases |
| Python | **3.12.x**, plus `pip install -r tools/requirements.txt` (pyserial) | |
| Git for Windows | **2.47** or later | |

1. Put `cmake`, `ninja` and the toolchain's `bin` directory on `PATH`.
2. Point the build at the prebuilt host tools, so no MSVC or MinGW host
   compiler is needed. Either set environment variables:

   ```bash
   setx PICO_TOOLCHAIN_PATH C:\tools\arm-gnu-toolchain-14.2.rel1
   ```
   ```bash
   setx picotool_DIR C:\tools\picotool-2.2.0\picotool
   ```
   ```bash
   setx pioasm_DIR C:\tools\pico-sdk-tools-2.2.0\pioasm
   ```

   …or put the same three as `cacheVariables` in a `CMakeUserPresets.json`
   (git-ignored) that inherits from `release`. Each `*_DIR` is the directory
   that contains `picotoolConfig.cmake` / `pioasmConfig.cmake`.

`cmake/host_tools.cmake` looks for each tool in this order:
1. a CMake variable;
2. an environment variable;
3. the VS Code extension's folder.

It prints what it found at the start of the configure log.

## Flashing

**BOOTSEL drive (no driver needed):**
- **XIAO:** hold **B** (BOOT), tap **R** (RESET), release B.
- **Pico 2:** hold BOOTSEL while plugging it in.

Then copy the `.uf2` to the `RP2350` drive.

**picotool:**
- **XIAO in BOOTSEL** (any build):
  ```bash
  picotool load -x build\release\firmware.uf2
  ```
- **XIAO running a `diag` build:** no button needed; `-f` reboots it into
  BOOTSEL through its reset interface:
  ```bash
  picotool load -x -f build\diag\firmware.uf2
  ```
  Release builds have no USB device, so `-f` can't reach them: use the
  buttons.
- **Same in VS Code:** the tasks *Flash with picotool (board in BOOTSEL)*
  and *Flash diag build with picotool (no button)*.
- **picotool can't see a board in BOOTSEL?** Windows hasn't bound a driver
  to the RP2350 BOOTSEL vendor interface.
  1. Run **Zadig** and choose *Options → List All Devices*.
  2. Select **RP2350 Boot (Interface 1)**, pick **WinUSB** and click
     *Install Driver*.

  Don't replace the driver on the mass-storage interface (Interface 0). The
  diagnostic build's own reset interface needs no Zadig step: it carries a
  Microsoft OS 2.0 descriptor, so Windows binds WinUSB automatically.

**Debug probe:** run the VS Code task *Flash with OpenOCD / debug probe
(release)*.

## Debugging

1. Connect a Raspberry Pi Debug Probe (CMSIS-DAP) to the XIAO's SWD pads
   (SWDIO, SWCLK, GND).
2. Build the `debug` preset (or any preset), then in VS Code choose **Pico
   Debug (extension toolchain)** and press F5. The second configuration is
   for a manual setup with `openocd` and `arm-none-eabi-gdb` on `PATH`.

Useful globals to watch:

| Variable | Contents |
|---|---|
| `g_diag_ctr[]` | Core 0 counters (`diag_ctr_t` in `src/diag.h`): magnetometer, scheduling, overcurrent |
| `g_diag_max_cycles[]` | Interrupt-handler and critical-section maxima, in CPU cycles at **120 MHz** |
| `g_usbh` | Core 1 counters: device, transfers, records, SOF and heartbeat, timing |
| `g_prof[]` | Epoch and record-build time: last, maximum, average (cycles) |
| `g_boot_id`, `g_watchdog_resets`, `g_chip_version` | Boot identity, watchdog resets since power-on, chip stepping |
| `g_mag_single`, `g_mag_r_tick` | Run-time test modes; writable from the debugger |

**Expect these effects while debugging:**
- **Halting a core stops SOF,** so the downstream device suspends after
  3 ms. Do all timing measurements on free-running builds.
- **The watchdog and timers pause** while the debugger has a core halted,
  so stepping does not cause a watchdog reset.

## Diagnostic port (`diag` build)

The XIAO's USB-C port appears as a standard COM port, using Windows' built-in
driver.

**Status lines.** Once per second the port prints five status lines. The
values are totals since boot:
- `MAG`: magnetometer counters, fault flag;
- `SCHED`: scheduling counters, epoch timing, interrupt and
  critical-section maxima;
- `USBH`: attach, detach, enumeration, unsupported devices, transfer
  errors, STALLs, halt clears, VBUS overcurrent;
- `REC`: records sent, late, aborted, overwritten and missing; SOF timing,
  queue timing, publish margin;
- `SYS`: boot ID, watchdog resets, chip stepping, test modes, last record's
  reason and flags.

**Lines appear only while the port is open with DTR asserted.**
`tools\diag_ctl.py` asserts DTR. Some terminal programs don't by default.

```bash
py tools\diag_ctl.py --port COM9 --watch 10
```

**Commands** (one per line; each is answered `OK` or `ERR`):

| Command | Effect |
|---|---|
| `mode pair` / `mode single` | Pair mode (normal) or single-measurement mode (V-18) |
| `rtick <1..19>` | Tick of the R trigger; 10 is normal, 5 is the V-19 reference |
| `stall 1` / `stall 2` | Stall core 0 or core 1 to test the watchdog (V-14) |
| `status` | Report now |

**Rebooting into BOOTSEL.** Opening the port at 1200 baud, or `picotool
reboot -f`, reboots into BOOTSEL. VBUS is turned off first, so the
downstream device sees a clean detach.

## Verification tools

Python 3, on Windows. Install with `py -m pip install -r
tools\requirements.txt`.

| Tool | Purpose |
|---|---|
| `tools/rx_decode.py` | Capture from the test target: checks framing, CRC, sequence and boot ID against the V-1 criteria; writes CSV and a raw log; sends timed test-target commands (`--script`) |
| `tools/timing_report.py` | V-2 cadence and V-3 freshness from a raw log |
| `tools/target_ctl.py` | Select test-target modes (NAK, STALL, descriptor variants, coil) |
| `tools/diag_ctl.py` | The XIAO's diagnostic port: status, commands, V-18 mode alternation |
| `tools/noise_report.py` | RMS noise and Allan deviation, split by pair and single mode |

**Examples** (COM7 = test target's USB-serial adapter, COM9 = XIAO diag
port):

1. **V-1 soak and V-2/V-3 timing.** Five minutes, then the timing report.
   `--meas-ms` is the measurement time found in V-17.
   ```bash
   py tools\rx_decode.py --port COM7 --seconds 300 --log raw.txt --csv rec.csv
   ```
   ```bash
   py tools\timing_report.py raw.txt --meas-ms 8.0
   ```
2. **Fault injection during a capture.** The script file has one
   `<seconds> <command>` per line, e.g. `60 mode nak 50`:
   ```bash
   py tools\rx_decode.py --port COM7 --seconds 120 --script faults.txt --log raw.txt
   ```
3. **V-18 noise, pair against single, for 20 minutes.** Run both commands at
   the same time, in two terminals:
   ```bash
   py tools\diag_ctl.py --port COM9 --alternate 60 --minutes 20
   ```
   ```bash
   py tools\rx_decode.py --port COM7 --seconds 1200 --csv noise.csv
   ```
   Then:
   ```bash
   py tools\noise_report.py noise.csv
   ```

To try the tools without hardware, run the tests in
[tests/host/README.md](tests/host/README.md).

## Configuration

Compile-time parameters are in `src/config.h`. Each can be overridden with
`-D`. They include:
- pins;
- the cycle schedule (`MAG_S_TICK`, `MAG_R_TICK`);
- the epoch and SOF offsets (`EPOCH_OFFSET_US`, `SOF_OFFSET_US`);
- the staleness and saturation limits;
- the re-probe rate and not-ready fault threshold;
- the USB host timeouts;
- the watchdog timeout;
- interrupt priorities;
- the PIO-USB DMA channel (`USB_HOST_TX_DMA_CH`, which nothing else may
  claim).

The magnetometer bandwidth is `MMC_CTRL1_BW` in `src/mmc5983ma.h`: BW = 00
by default, with BW = 01 as the fallback if the measurement time proves too
long (M-23).

## License

[0BSD](LICENSE): anyone may use, copy, modify and distribute this code, with
or without a fee, and nothing has to be kept or credited. Third-party
components keep their own licences, and their copyright notices must go with
any binary you distribute: the Pico SDK (BSD-3-Clause), TinyUSB (MIT) and
Pico-PIO-USB (MIT).
