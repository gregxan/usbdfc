# Verification round 2, session A: core 1, dual-role USB, mailbox, boot

You are reviewing firmware written by another model. Treat its comments,
commit messages and docs/REQUIREMENTS.md as claims to verify, not facts.
Verify only against the pinned sources in this repository.

**Scope:**
- areas 4, 5, 7 and 8 of `docs/VERIFY_PROMPT.md`;
- a re-check of the fixes for round-1 findings F1–F11
  (docs/REQUIREMENTS.md section 8b).

Session B covers areas 6, 9 and 10. Do not review those.

## Setup: read code, don't build

- **Commit.** Use `main` at `69e2796` or later. At `69e2796` the `release`,
  `debug` and `diag` presets and `test_target` build with no warnings
  outside the B-25 exceptions, and the host tests pass. **Do not install a
  toolchain or build** unless a finding needs compiled output. The only
  expected case is core 1 stack usage (area 8).
- **If you do need a build:** build `release` only, with
  arm-none-eabi-gcc 13.x, and read `build/release/**/*.su`.
- **Submodules.** Fetch only what this review reads. The SDK's other
  submodules (btstack, cyw43, lwip, mbedtls and so on) are large and not
  needed:
  ```bash
  git submodule update --init --depth 1 lib/pico-sdk lib/Pico-PIO-USB
  git -C lib/pico-sdk submodule update --init --depth 1 lib/tinyusb
  ```

## Files you may read

Read nothing outside this list unless a specific finding needs it. If one
does, say which file and why.

**This repository:**

| File | Functions |
|---|---|
| `src/usbh.c` | all |
| `src/usbh.h` | all |
| `src/mailbox.c` | `mb_publish`, `mb_take` |
| `src/mailbox.h` | all |
| `src/main.c` | `main`, `watchdog_boot_check`, `watchdog_poll`, `epoch_work`, `build_and_publish`, `vbus_init` |
| `src/tick.c` | `tick_isr`, `tick_init`, `tick_count` |
| `src/usbd_diag.c` | all |
| `src/status.c` | `status_poll`, `build_lines` |
| `src/config.h`, `src/tusb_config.h`, `CMakeLists.txt` | all |
| `docs/REQUIREMENTS.md` | sections 4, 4.1, 5, 5.0, 5.1, 5.3, 7, 8a, 8b, 8c |

**TinyUSB** (`lib/pico-sdk/lib/tinyusb/src/`):
- `host/usbh.c`, `host/usbh.h`, `host/usbh_pvt.h`, `host/hcd.h`
- `class/cdc/cdc_host.c`
- `tusb.c` (the `tu_edpt_stream_*` functions)
- `osal/osal_pico.h`
- `device/usbd.c`
- `portable/raspberrypi/pio_usb/hcd_pio_usb.c`
- `portable/raspberrypi/rp2040/rp2040_usb.c`, `rp2040_usb.h`,
  `dcd_rp2040.c`

**Pico-PIO-USB** (`lib/Pico-PIO-USB/src/`): `pio_usb.c`, `pio_usb_host.c`,
`pio_usb_ll.h`, `usb_definitions.h`

**Pico SDK** (`lib/pico-sdk/src/`):
- `rp2_common/hardware_timer/`
- `rp2_common/hardware_irq/irq.c`
- `rp2_common/pico_multicore/multicore.c`
- `rp2_common/hardware_watchdog/watchdog.c`
- `rp2_common/hardware_clocks/clocks.c`
- `rp2350/pico_platform/platform.c`
- `rp2_common/pico_crt0/rp2350/memmap_copy_to_ram.ld`
- `rp2_common/pico_bootrom/` (only `rom_reset_usb_boot_extra`)
- `rp2_common/pico_stdio_usb/reset_interface.c` (as the reference
  implementation for `usbd_diag.c`)

## Already verified: do not re-check

These were confirmed by round 1 or by the fact audit (REQUIREMENTS 8c).
Cite them if needed, but do not spend reading on them.

- **Callback and enumeration behaviour:**
  - `tuh_cdc_mount_cb` runs before `tuh_mount_cb`;
  - the control stage is IDLE in the mount callback;
  - `tuh_cdc_itf_get_info()` returns the communication interface;
  - DTR/RTS is sent before line coding, only with line-request support,
    and a STALL there does not stop the mount;
  - enumeration retries 3 times, 100 ms apart, with a shared counter.
- **Abort and halt clear:**
  - after `tuh_edpt_abort_xfer()` and `tuh_cdc_write_clear()` the stream
    is reusable;
  - the abort's busy-wait never runs from the core 1 thread;
  - `ep_num` is the full address, and `data_id` is the toggle;
  - a zero-length `tuh_cdc_read` re-arms IN when the FIFO is empty and
    the endpoint is free;
  - `hcd_edpt_clear_stall()` is a stub.
- **Link-time wrap.** In the linked release ELF, all producer call sites of
  `hcd_event_handler` go through `__wrap_hcd_event_handler`.
- **Pico-PIO-USB at `fe9133f`:**
  - with `skip_alarm_pool`, `timer_active` is still set;
  - `pio_usb_host_frame()` sends SOF, then runs endpoints in pool order;
  - a NAK is retried next frame;
  - an error is not retried;
  - the PIO dividers come from `clock_get_hz(clk_sys)`;
  - DMA is claimed with `dma_claim_mask(1 << tx_ch)`.
- **TinyUSB build mode.** The SDK builds TinyUSB with `OPT_OS_PICO`.

## What to check

### A1. Re-check the fixes for F1–F11 (`src/usbh.c`)
- **The changed functions:** `try_flush`, `send_frame`, `queue_record`,
  `drop_inflight`, `stream_poll`, `reset_stream`, `halt_state_reset`,
  `tuh_cdc_umount_cb`, `tuh_mount_cb`, `tuh_umount_cb`,
  `__wrap_hcd_event_handler`, `events_poll`, `parse_endpoints`.
- **Does each fix close its finding?** Does it open a new race, especially
  between the frame ISR setting `s_out_done` / `s_in_err_ev` and
  `tuh_task()` releasing the endpoint?
- **Missing-slot logic:** the `while (s_have_seq && now >= designated_sof_us(...))`
  loop, against the late-record and pending-record paths. Look for double
  counting, and for records that are never counted.
- **F5 resend.** Is the claim that a device discards the resend as a
  duplicate correct for the data-toggle handling in `usb_out_transaction`
  and `pio_usb_ll_transfer_continue`? What happens if the resend also
  fails? (A known residual: the next record may then be discarded.)
- **`try_flush`.** Its "FIFO empty means already sent" test uses
  `tuh_cdc_write_available() >= CFG_TUH_CDC_TX_BUFSIZE`. Is that sound for
  the `tu_edpt_stream` FIFO?

### A2. SOF generation (area 4)
- **Shared TIMER0.** It carries both the core 0 tick alarm and the core 1
  SOF alarm. Check for races on INTR, ARMED and INTE. Check
  `irq_set_exclusive_handler()` called from core 1: shared vector table,
  per-core NVIC.
- **C1-2's ±0.5 µs interval.** Find everything on core 1 that disables
  interrupts or busy-waits in thread context:
  - `osal_pico` critical sections;
  - `hcd_port_reset`;
  - `pio_usb_host_port_reset_start` / `_end`.

  Estimate the worst-case SOF interrupt latency, and whether SOF can stop
  for 3 ms or more while a device is attached.

### A3. Dual-role TinyUSB, diag builds (area 5)
- **Shared USB code.** Does `RP2040_USB_HOST_MODE=1`, set by
  `tinyusb_host_base`, change how `rp2040_usb.c` or `dcd_rp2040.c` behave
  for the native device in the same image?
- **Cross-core state.** Are any globals in `usbh.c`, `usbd.c`, `tusb.c` or
  `osal_pico` touched by both `tud_task()` (core 0) and `tuh_task()`
  (core 1) after initialisation?
- **`usbd_diag.c`:**
  - Do the BOS and MS OS 2.0 descriptors match
    `reset_interface.c`?
  - `bcdUSB` 0x0210;
  - the `rom_reset_usb_boot_extra()` arguments;
  - the 1200-baud path.
- **`status.c`:** does it ever block? Does any status line exceed
  `LINE_MAX` (240)?

### A4. Mailbox (area 7, `src/mailbox.c`)
- **Correctness.** Prove or disprove the triple buffer under the Armv8-M
  memory model with the DMBs as written. Can core 0 write the slot core 1
  is copying? Can core 1 return a torn record?
- **Liveness.** Can `mb_take` livelock?

### A5. Clocks, watchdog and boot (area 8, `src/main.c`)
- **Clock.** Does `set_sys_clock_khz(120000, true)` after runtime init leave
  `clk_peri` at 120 MHz? Does anything in the listed files use the
  compile-time `SYS_CLK_HZ` (150 MHz) instead of `clock_get_hz()`?
- **Watchdog:**
  - `watchdog_enable_caused_reboot()` semantics;
  - whether scratch 0–1 survive a watchdog reset and clear at power-on;
  - whether `rom_reset_usb_boot_extra` or `watchdog_reboot` write
    scratch 0–1;
  - the feed condition in `watchdog_poll` (SOF within 10 ms, heartbeat
    within 1 s) against the 1.5 s timeout.
- **Start-up order.** Core 1 claims DMA channel 11 before
  `mag_init()`'s `dma_claim_unused_channel()` (C1-8). Can anything break
  that order: the 1 s FIFO timeout path in `main`?
- **Chip stepping.** Does `rp2350_chip_version() >= 4` identify A4, from
  the decoding in `platform.c`?
- **Core 1 stack.** Is it in SCRATCH_X, and how big is it (`.stack1`,
  `PICO_CORE1_STACK_SIZE`)? Estimate the worst case: thread plus frame ISR
  through TinyUSB and Pico-PIO-USB.

## Rules
- **Cite `file:line`** for every claim, in this repo or a pinned dependency.
- **Classify each finding:** BUG, LIKELY FAIL, ASSUMPTION CONFIRMED,
  ASSUMPTION REFUTED, or CANNOT VERIFY WITHOUT HARDWARE (name the
  measurement).
- **Rate severity:** High, Medium or Low.
- **Do not modify code.** Correctness, timing and concurrency only; no
  style review.

## Report: keep it short (aim for under ~1,500 words)
1. One line on setup: what you fetched, and whether you built.
2. A findings table, most severe first: ID (A-n), class, severity,
   `file:line`, one-line summary.
3. Per BUG or LIKELY FAIL: at most 5 lines covering evidence, failure
   scenario and fix.
4. The F1–F11 re-check: one line per finding (closed, or not closed and
   why).
5. Bench measurements needed, as a list.

Omit items that are fine, except a single line listing the checks that
passed.
