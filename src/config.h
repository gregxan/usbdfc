/*
 * Compile-time configuration. Every value can be overridden with -D on the
 * command line or in CMakeLists.txt (each is wrapped in #ifndef).
 *
 * Requirement IDs refer to docs/REQUIREMENTS.md (Revision 3.0-draft).
 * Adapted from DFC Rev 2.16 src/config.h (B-26, docs/PROVENANCE.md).
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* --------------------------------------------------------------- Clocks */

/* R-3: clk_sys = 120 MHz, set at the start of main(). clk_peri follows. */
#define SYS_CLOCK_KHZ 120000u

/* ======================================================================
 * Pin selection (XIAO RP2350). Front edge, from the Seeed pinout:
 *   D0 = GPIO26 (A0)   D4 = GPIO6         D8  = GPIO2
 *   D1 = GPIO27 (A1)   D5 = GPIO7         D9  = GPIO4
 *   D2 = GPIO28 (A2)   D6 = GPIO0         D10 = GPIO3
 *   D3 = GPIO5         D7 = GPIO1
 * Used by the board itself; do not assign: GPIO19, 22, 23, 25, 29.
 * ====================================================================== */

/* USBH-1: PIO-USB host. D- = D+ + 1 (PIO_USB_PINOUT_DPDM). */
#define USB_HOST_DP_PIN 0u /* D6; D- is GPIO1 = D7 */

/* HW-12: VBUS load switch. Enable has an external ~100 kOhm pull-down;
 * the overcurrent flag is open drain, active low, internal pull-up. */
#define VBUS_EN_PIN 4u /* D9  */
#define VBUS_OC_PIN 3u /* D10 */

/* HW-4, HW-5, M-1 */
#define MAG_SDA_PIN 6u /* D4 */
#define MAG_SCL_PIN 7u /* D5 */
#define MAG_I2C_ADDR 0x30u
#ifndef MAG_I2C_HZ
#define MAG_I2C_HZ 400000u
#endif

/* HW-9 (amended): test pins, driven only when APP_TEST_PINS = 1 (diag). */
#ifndef APP_TEST_PINS
#define APP_TEST_PINS 0
#endif
#define TEST_PIN_EPOCH 2u /* D8: high while the epoch work runs        */
#define TEST_PIN_RTRIG 5u /* D3: pulsed at the R-measurement trigger (V-3) */

/* --------------------------------------------------------- Magnetometer */

/* M-6: the schedule grid. */
#define TICK_PERIOD_US 1000u

/* M-20: one cycle = 20 ticks = 20 ms (50 Hz output). */
#define MAG_CYCLE_TICKS 20u
#define MAG_S_TICK 0u /* SET + trigger S at this tick of the cycle */
#ifndef MAG_R_TICK
#define MAG_R_TICK 10u /* RESET + trigger R; "rtick 5" for the V-19 reference */
#endif

/* Section 8a H2: start-up value of the run-time single-measurement mode
 * (g_mag_single, V-18). Switched by the diagnostic command "mode". */
#ifndef MAG_MODE_SINGLE
#define MAG_MODE_SINGLE 0
#endif

/* M-17 (amended): a record older than this when built is stale. */
#ifndef MAG_STALE_US
#define MAG_STALE_US 30000u
#endif

/* D-1: a raw axis within this many counts of either end of the 18-bit
 * range counts as saturated. */
#ifndef MAG_SAT_MARGIN
#define MAG_SAT_MARGIN 72u
#endif

/* M-2a / section 8a M9: while faulted, re-probe at tick 0 of every
 * MAG_REPROBE_CYCLES-th cycle (10 x 20 ms = 5 Hz). */
#ifndef MAG_REPROBE_CYCLES
#define MAG_REPROBE_CYCLES 10u
#endif

/* M-2b (amended): not-ready score per measurement. A never-ready sensor
 * faults after 10 measurements (100 ms). Thresholds TBD (M-2b). 0 disables. */
#ifndef MAG_NOT_READY_FAULT_SCORE
#define MAG_NOT_READY_FAULT_SCORE 20u
#endif
#define MAG_NOT_READY_STEP 2u

/* M-18: PIO bus clear. Section USBH-S5: on PIO1 (PIO0 is PIO-USB's). */
#ifndef MAG_BUS_CLEAR
#define MAG_BUS_CLEAR 1
#endif

/* --------------------------------------------------- Scheduling (C1-5) */

#ifndef EPOCH_OFFSET_US
#define EPOCH_OFFSET_US 500u /* EX-10: epoch at tick + 500 us */
#endif
#ifndef SOF_OFFSET_US
#define SOF_OFFSET_US 600u /* C1-5: SOF at tick + 600 us */
#endif

/* ------------------------------------------------------------- USB host */

/* USBH-S3: Pico-PIO-USB claims this DMA channel by number. Nothing else
 * may claim it; other DMA users claim only after core 1's tuh_init(). */
#ifndef USB_HOST_TX_DMA_CH
#define USB_HOST_TX_DMA_CH 11u
#endif

#define USBH_ENUM_TIMEOUT_US 2000000u   /* USBH-3   */
#define USBH_STUCK_RECORD_US 20000u     /* USBH-T6  */
#define USBH_HALT_CLEAR_MIN_US 1000000u /* section 7.1 */

/* --------------------------------------------------------- Watchdog (C1-7) */

#ifndef WATCHDOG_TIMEOUT_MS
#define WATCHDOG_TIMEOUT_MS 1500u
#endif
#define WATCHDOG_HEARTBEAT_WINDOW_US 1000000u

/* ---------------------------------------------------- Interrupt priority */

/* EX-5 (amended). RP2350 implements 4 priority bits; lower = higher. */
#define IRQ_PRIO_MAG 0x40u  /* I2C STOP detect (completion) + abort   */
#define IRQ_PRIO_TICK 0x80u /* 1 kHz timer                             */
#define IRQ_PRIO_USB 0xC0u  /* native USB, diagnostic builds only      */
#define IRQ_PRIO_SOF 0x00u  /* core 1's only interrupt (C1-1)          */

/* ------------------------------------------------------ Instrumentation */

#ifndef DIAG_TIMING
#define DIAG_TIMING 1
#endif

/* NU-1: native-USB diagnostic port (diagnostic presets only). */
#ifndef APP_USB_STATUS
#define APP_USB_STATUS 0
#endif

/* V-14 hook (diag builds): writing 1 or 2 to g_diag_stall_core stalls
 * core 0 or core 1 so the watchdog fires ("stall 1" / "stall 2"). */
#ifndef APP_STALL_HOOK
#define APP_STALL_HOOK 0
#endif

#endif /* APP_CONFIG_H */
