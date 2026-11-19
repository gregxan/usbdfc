/*
 * Adapted from DFC Rev 2.16 src/mag.c (B-26, docs/PROVENANCE.md). Kept
 * unchanged in substance: the DMA transaction engine (start_dma, M-7a DREQ
 * resync, RP2350-E5 handling), abort and timeout handling (M-15, M-16),
 * bus clear (M-18), re-probe and configuration (M-2a), the not-ready score
 * (M-2b). Changed: the schedule (M-20), pulses in every transaction, pair
 * assembly (M-22), reason codes (M-9 amended), BW = 00 (M-3), bus clear on
 * PIO1 (USBH-S5), removal of the periodic SET/RESET modes and the offset
 * calibration (M-4 / M-4b replaced).
 */
#include "mag.h"

#include <stddef.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/dma_debug.h"
#include "hardware/structs/m33.h"
#include "hardware/structs/sio.h"
#include "pico/time.h"

#include "config.h"
#include "diag.h"
#include "tick.h"

#if MAG_BUS_CLEAR
#include "i2c_bus_clear.pio.h"
#endif

#define MAG_I2C i2c1
#define MAG_I2C_IRQ I2C1_IRQ

#define CMD_READ I2C_IC_DATA_CMD_CMD_BITS
#define CMD_STOP I2C_IC_DATA_CMD_STOP_BITS
#define CMD_RESTART I2C_IC_DATA_CMD_RESTART_BITS

#define I2C_TX_FIFO_DEPTH 16u

/* IC_TX_ABRT_SOURCE abort-cause flags (bits 16:0). */
#define ABRT_REASON_BITS 0x0001ffffu
#define SLOT_NONE 2u

#if MAG_MODE_SINGLE
#define MAG_PAIR_MODE 0
#else
#define MAG_PAIR_MODE 1
#endif

_Static_assert(MAG_S_TICK < MAG_CYCLE_TICKS && MAG_R_TICK < MAG_CYCLE_TICKS && MAG_S_TICK != MAG_R_TICK,
               "schedule ticks must be distinct and inside the cycle");

typedef enum {
    MAG_FAULT,    /* product ID not confirmed: re-probe (M-2a)          */
    MAG_IDLE,     /* waiting for the next scheduled tick                */
    MAG_BUSY,     /* transaction in flight                              */
    MAG_ABORTING, /* tick requested an abort (M-16), TX_ABRT not seen   */
    MAG_ABORTED,  /* abort handled; waiting for DMA/I2C/bus to go idle  */
} mag_state_t;

/* Kind of the transaction in flight (or last aborted). */
typedef enum { TXN_S, TXN_R, TXN_PROBE, TXN_CONFIG } mag_txn_t;

/* M-11: double buffer of published pairs. */
static mag_pair_t s_pairs[2];
static volatile uint32_t s_latest = SLOT_NONE;

static volatile mag_state_t s_state = MAG_FAULT;
/* The tick starts before mag_init() (C1-8); it is ignored until init is done. */
static volatile bool s_ready;
static volatile bool s_fault = true;
static volatile mag_txn_t s_txn = TXN_S;

/* M-2a re-probe */
static volatile bool s_probe_asap;     /* M-2b: probe at the next tick 0 */
static uint32_t s_not_ready_score;     /* M-2b */
static volatile bool s_config_pending; /* probe matched: configure next tick */
static uint8_t s_probe_id;
static uint32_t s_probe_cmd[2];
#define CFG_CMD_LEN 10u
static uint32_t s_cfg_cmd[CFG_CMD_LEN];

/* Transaction in flight. */
static volatile uint64_t s_cur_cycle;
static uint8_t s_rx[MMC_READ_BYTES] __attribute__((aligned(4)));

/* The last completed trigger (M-9, M-10). */
static volatile bool s_trig_ok;
static volatile mag_txn_t s_trig_kind;
static volatile uint64_t s_trig_cycle;
static uint64_t s_trig_time_us;

/* Pair being assembled (I2C ISR only). */
static mag_pair_t s_build;
static bool s_build_have_s;

/* Command lists for TXN_S and TXN_R. */
static uint32_t s_cmd_s[I2C_TX_FIFO_DEPTH] __attribute__((aligned(4)));
static uint32_t s_cmd_r[I2C_TX_FIFO_DEPTH] __attribute__((aligned(4)));
static uint32_t s_cmd_s_len;
static uint32_t s_cmd_r_len;

static uint s_dma_cmd;
static uint s_dma_data;
static uint32_t s_dma_mask;

#if MAG_BUS_CLEAR
static PIO s_pio;
static uint s_sm;
static volatile bool s_bc_running;
#endif

bool mag_fault(void) {
    return s_fault;
}

/* ------------------------------------------------------------ helpers --- */

static inline i2c_hw_t *i2c_regs(void) {
    return i2c_get_hw(MAG_I2C);
}

static inline void i2c_irq_mask(void) {
    m33_hw->nvic_icer[MAG_I2C_IRQ / 32u] = 1u << (MAG_I2C_IRQ % 32u);
    __dsb();
    __isb();
}

static inline void i2c_irq_unmask(void) {
    m33_hw->nvic_iser[MAG_I2C_IRQ / 32u] = 1u << (MAG_I2C_IRQ % 32u);
}

static inline uint64_t nominal_tick_us(uint64_t cycle, uint32_t k) {
    return tick_time_us(cycle * MAG_CYCLE_TICKS + k);
}

/* M-7 (amended): read the previous measurement, then an optional pulse,
 * then the trigger. No persistent Internal Control 0 bits (M-5). */
static size_t build_cmds(uint32_t *c, uint32_t pulse) {
    size_t i = 0;
    c[i++] = MMC_REG_XOUT0; /* START + W(0x00) */
    for (unsigned k = 0; k < MMC_READ_BYTES; k++) {
        c[i++] = CMD_READ | (k == 0u ? CMD_RESTART : 0u); /* RESTART + R x9 */
    }
    if (pulse != 0u) {
        c[i++] = CMD_RESTART | MMC_REG_CTRL0;
        c[i++] = pulse;
    }
    c[i++] = CMD_RESTART | MMC_REG_CTRL0; /* RESTART + W(0x09)  */
    c[i++] = CMD_STOP | MMC_CTRL0_TM_M;    /* TM_M, STOP         */
    return i;
}

#if MAG_BUS_CLEAR
/* M-18. Called from the I2C ISR after an abort. Non-blocking. */
static void __not_in_flash_func(bus_clear_start)(void) {
    if (s_bc_running) {
        return;
    }
    s_bc_running = true;
    gpio_set_function(MAG_SCL_PIN, (gpio_function_t)pio_get_funcsel(s_pio));
    pio_sm_put(s_pio, s_sm, 0u);
    diag_inc(DIAG_MAG_BUS_CLEAR);
}

static bool __not_in_flash_func(bus_clear_done)(void) {
    if (!s_bc_running) {
        return true;
    }
    if (pio_sm_is_rx_fifo_empty(s_pio, s_sm)) {
        return false;
    }
    (void)pio_sm_get(s_pio, s_sm);
    gpio_set_function(MAG_SCL_PIN, GPIO_FUNC_I2C);
    s_bc_running = false;
    return true;
}
#endif

/* Both DMA channels and the I2C controller idle (M-16). Non-blocking. */
static bool __not_in_flash_func(hw_idle)(void) {
#if MAG_BUS_CLEAR
    /* First, and regardless of the controller's state (M-16, Rev 2.15). */
    if (!bus_clear_done()) {
        return false;
    }
#endif
    if (dma_hw->abort & s_dma_mask) {
        return false;
    }
    if ((dma_hw->ch[s_dma_cmd].ctrl_trig | dma_hw->ch[s_dma_data].ctrl_trig) & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
        return false;
    }
    i2c_hw_t *hw = i2c_regs();
    if (hw->enable & I2C_IC_ENABLE_ABORT_BITS) {
        return false;
    }
    uint32_t status = hw->status;
    if ((status & I2C_IC_STATUS_MST_ACTIVITY_BITS) || !(status & I2C_IC_STATUS_TFE_BITS)) {
        return false;
    }
    return true;
}

/* DMA finished and no abort pending, yet the controller still holds the
 * bus. A fresh abort is required (M-16). */
static bool __not_in_flash_func(controller_hung)(void) {
#if MAG_BUS_CLEAR
    if (s_bc_running) {
        return false;
    }
#endif
    if (dma_hw->abort & s_dma_mask) {
        return false;
    }
    if ((dma_hw->ch[s_dma_cmd].ctrl_trig | dma_hw->ch[s_dma_data].ctrl_trig) & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
        return false;
    }
    i2c_hw_t *hw = i2c_regs();
    return !(hw->enable & I2C_IC_ENABLE_ABORT_BITS) && (hw->status & I2C_IC_STATUS_MST_ACTIVITY_BITS);
}

/* RP2350-E5: clear EN before CHAN_ABORT; start_dma() sets EN again. */
static inline void __not_in_flash_func(mag_dma_abort)(void) {
    hw_clear_bits(&dma_hw->ch[s_dma_cmd].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[s_dma_data].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_hw->abort = s_dma_mask;
}

/* Arm the data channel (if any data is expected), then the command
 * channel. Tick ISR only, I2C IRQ masked, hardware idle. */
static void __not_in_flash_func(start_dma)(mag_txn_t txn, const uint32_t *cmd, uint32_t n_cmd, uint8_t *dst,
                                           uint32_t n_data) {
    i2c_hw_t *hw = i2c_regs();

    for (unsigned k = 0; k < I2C_TX_FIFO_DEPTH && hw->rxflr != 0u; k++) {
        (void)hw->data_cmd;
    }
    (void)hw->clr_tx_abrt;
    (void)hw->clr_stop_det;

    s_txn = txn;
    s_state = MAG_BUSY;
    /* M-7a: resynchronise both channels' DREQ credit counters (EX-9). */
    dma_debug_hw->ch[s_dma_data].dbg_ctdreq = 0u;
    dma_debug_hw->ch[s_dma_cmd].dbg_ctdreq = 0u;
    hw_set_bits(&dma_hw->ch[s_dma_data].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_set_bits(&dma_hw->ch[s_dma_cmd].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    if (n_data != 0u) {
        dma_hw->ch[s_dma_data].write_addr = (uint32_t)(uintptr_t)dst;
        dma_hw->ch[s_dma_data].al1_transfer_count_trig = n_data;
    }
    dma_hw->ch[s_dma_cmd].read_addr = (uint32_t)(uintptr_t)cmd;
    dma_hw->ch[s_dma_cmd].al1_transfer_count_trig = n_cmd;
}

/* Start a scheduled measurement transaction (M-20). */
static void __not_in_flash_func(sample_start)(mag_txn_t txn, uint64_t cycle) {
    s_cur_cycle = cycle;
    if (txn == TXN_S) {
        start_dma(TXN_S, s_cmd_s, s_cmd_s_len, s_rx, MMC_READ_BYTES);
    } else {
        start_dma(TXN_R, s_cmd_r, s_cmd_r_len, s_rx, MMC_READ_BYTES);
    }
}

static void __not_in_flash_func(probe_start)(void) {
    s_probe_id = 0u;
    start_dma(TXN_PROBE, s_probe_cmd, 2u, &s_probe_id, 1u);
}

static void __not_in_flash_func(config_start)(void) {
    start_dma(TXN_CONFIG, s_cfg_cmd, CFG_CMD_LEN, NULL, 0u);
}

/* M-16: abort without waiting. Returns true if a controller abort was
 * requested (a TX_ABRT interrupt will follow). */
static bool __not_in_flash_func(issue_abort)(void) {
    mag_dma_abort();
    i2c_hw_t *hw = i2c_regs();
    uint32_t status = hw->status;
    if ((status & I2C_IC_STATUS_MST_ACTIVITY_BITS) || !(status & I2C_IC_STATUS_TFE_BITS)) {
        hw_set_bits(&hw->enable, I2C_IC_ENABLE_ABORT_BITS);
        return true;
    }
    return false;
}

/* D-1: informational only (D-5). */
static void __not_in_flash_func(count_saturation)(const uint8_t *raw) {
    for (unsigned a = 0; a < 3u; a++) {
        uint32_t v = mag_raw18(raw, a);
        if (v <= MAG_SAT_MARGIN || v >= MMC_FULL_SCALE_COUNTS - 1u - MAG_SAT_MARGIN) {
            diag_inc(DIAG_MAG_SATURATED);
            return;
        }
    }
}

/* M-9 (amended): status of the half just read. expect_kind/expect_cycle
 * name the trigger that should have produced it. */
static mag_half_t __not_in_flash_func(eval_half)(mag_txn_t expect_kind, uint64_t expect_cycle) {
    if (!s_trig_ok || s_trig_kind != expect_kind || s_trig_cycle != expect_cycle) {
        return MAG_HALF_NO_PRIOR;
    }
    if ((s_rx[MMC_READ_BYTES - 1u] & MMC_STATUS_MEAS_M_DONE) == 0u) {
        diag_inc(DIAG_MAG_NOT_READY);
        s_not_ready_score += MAG_NOT_READY_STEP; /* M-2b */
        return MAG_HALF_NOT_READY;
    }
    if (s_not_ready_score != 0u) {
        s_not_ready_score--;
    }
    count_saturation(s_rx);
    return MAG_HALF_OK;
}

/* M-12: publish a completed pair with a single 32-bit write. */
static void __not_in_flash_func(publish_pair)(const mag_pair_t *p) {
    uint32_t slot = (s_latest == 0u) ? 1u : 0u; /* M-11 */
    s_pairs[slot] = *p;
    __dmb();
    s_latest = slot;
}

/* A scheduled transaction completed (STOP_DET, both DMA channels done). */
static void __not_in_flash_func(sample_done)(uint64_t now_us) {
    uint64_t c = s_cur_cycle;
    if (s_txn == TXN_S) {
#if MAG_PAIR_MODE
        /* Read R of cycle c-1: the pair for c-1 is complete. */
        if (c >= 1u) {
            mag_half_t r = eval_half(TXN_R, c - 1u);
            mag_pair_t p;
            if (s_build_have_s && s_build.cycle == c - 1u) {
                p = s_build;
            } else {
                memset(&p, 0, sizeof p);
                p.s_half = (uint8_t)MAG_HALF_MISSING;
                p.cycle = c - 1u;
                p.t_us = nominal_tick_us(c - 1u, MAG_S_TICK);
            }
            memcpy(p.r_raw, s_rx, MMC_READ_BYTES);
            p.r_half = (uint8_t)r;
            p.single = false;
            publish_pair(&p);
        }
#else
        /* Single mode: read S of cycle c-1. */
        if (c >= 1u) {
            mag_half_t h = eval_half(TXN_S, c - 1u);
            mag_pair_t p;
            memset(&p, 0, sizeof p);
            memcpy(p.s_raw, s_rx, MMC_READ_BYTES);
            p.s_half = (uint8_t)h;
            p.r_half = (uint8_t)MAG_HALF_OK;
            p.single = true;
            p.cycle = c - 1u;
            p.t_us = (h == MAG_HALF_NO_PRIOR) ? nominal_tick_us(c - 1u, MAG_S_TICK) : s_trig_time_us;
            publish_pair(&p);
        }
#endif
        /* This transaction's trigger (S of cycle c) just completed (M-10). */
        s_build_have_s = false;
        s_build.cycle = c;
        s_build.t_us = now_us;
        s_trig_kind = TXN_S;
    } else { /* TXN_R: read S of cycle c */
        mag_half_t s = eval_half(TXN_S, c);
        if (s_build.cycle != c) {
            /* This cycle's S transaction was lost: no S trigger time. */
            s_build.cycle = c;
            s_build.t_us = nominal_tick_us(c, MAG_S_TICK);
        }
        memcpy(s_build.s_raw, s_rx, MMC_READ_BYTES);
        s_build.s_half = (uint8_t)s;
        s_build_have_s = true;
        s_trig_kind = TXN_R;
#if APP_TEST_PINS
        sio_hw->gpio_togl = 1u << TEST_PIN_RTRIG; /* V-3: R trigger marker */
#endif
    }
    s_trig_ok = true;
    s_trig_cycle = c;
    s_trig_time_us = now_us;
    s_state = MAG_IDLE;
}

/* ------------------------------------------------------------- ISRs ---- */

/* I2C1: STOP_DET = transaction complete; TX_ABRT = abort (M-15). */
static void __not_in_flash_func(i2c_isr)(void) {
    uint32_t t0 = diag_cycles();
    /* M-10: read the timer first; STOP_DET follows the trigger's ACK by
     * about one bit time. */
    uint64_t now_us = time_us_64();
    i2c_hw_t *hw = i2c_regs();
    uint32_t st = hw->intr_stat;

    if (st & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) {
        mag_dma_abort();
        uint32_t src = hw->tx_abrt_source;
        (void)hw->clr_tx_abrt;
        (void)hw->clr_stop_det;
        bool user = (src & I2C_IC_TX_ABRT_SOURCE_ABRT_USER_ABRT_BITS) != 0u;
        if (src & ABRT_REASON_BITS & ~I2C_IC_TX_ABRT_SOURCE_ABRT_USER_ABRT_BITS) {
            diag_inc(s_txn == TXN_PROBE ? DIAG_MAG_PRODUCT_ID_FAULT : DIAG_MAG_I2C_ERROR);
        }
        s_trig_ok = false;
        if (s_state == MAG_BUSY || s_state == MAG_ABORTING) {
            s_state = MAG_ABORTED;
        }
#if MAG_BUS_CLEAR
        if (user || (src & I2C_IC_TX_ABRT_SOURCE_ARB_LOST_BITS)) {
            bus_clear_start();
        }
#else
        (void)user;
#endif
    } else if (st & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {
        (void)hw->clr_stop_det;
        if (s_state == MAG_BUSY) {
            bool data_done = (dma_hw->ch[s_dma_data].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) == 0u;
            bool cmd_done = (dma_hw->ch[s_dma_cmd].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) == 0u;
            if (!(data_done && cmd_done)) {
                /* Unexpected STOP mid-transaction (M-15). */
                mag_dma_abort();
                diag_inc(s_txn == TXN_PROBE ? DIAG_MAG_PRODUCT_ID_FAULT : DIAG_MAG_I2C_ERROR);
                s_trig_ok = false;
                s_state = MAG_ABORTED;
            } else if (s_txn == TXN_S || s_txn == TXN_R) {
                sample_done(now_us);
            } else if (s_txn == TXN_PROBE) {
                if (s_probe_id == MMC_PRODUCT_ID) {
                    s_config_pending = true; /* configure on the next tick */
                    s_state = MAG_IDLE;
                } else {
                    diag_inc(DIAG_MAG_PRODUCT_ID_FAULT);
                    s_state = MAG_FAULT;
                }
            } else { /* TXN_CONFIG: ended with RESET -> SET, nothing triggered */
                s_trig_ok = false;
                s_build_have_s = false;
                s_fault = false;
                s_not_ready_score = 0u;
                diag_inc(DIAG_MAG_RECOVERED);
                s_state = MAG_IDLE;
            }
        }
    }
    diag_max(DIAG_T_ISR_I2C, diag_cycles() - t0);
}

/* M-6, M-16, M-20. Called from the tick ISR (lower priority than the I2C
 * ISR, which is masked here so the state cannot change underneath). */
void __not_in_flash_func(mag_tick_isr)(uint64_t tick) {
    if (!s_ready) {
        return;
    }
    i2c_irq_mask();
    uint64_t cycle = tick / MAG_CYCLE_TICKS;
    uint32_t k = (uint32_t)(tick % MAG_CYCLE_TICKS);
    bool s_tick = (k == MAG_S_TICK);
    bool scheduled = s_tick || (MAG_PAIR_MODE && k == MAG_R_TICK);
    mag_txn_t next = s_tick ? TXN_S : TXN_R;
    bool sampling = (s_txn == TXN_S || s_txn == TXN_R);

    switch (s_state) {
    case MAG_FAULT:
        /* M-2a / section 8a M9: re-probe at tick 0 of every
         * MAG_REPROBE_CYCLES-th cycle, or the next tick 0 after M-2b. */
        if (s_tick && (s_probe_asap || cycle % MAG_REPROBE_CYCLES == 0u) && hw_idle()) {
            s_probe_asap = false;
            probe_start();
        }
        break;
    case MAG_IDLE:
        if (s_config_pending) {
            s_config_pending = false;
            config_start();
        } else if (MAG_NOT_READY_FAULT_SCORE != 0u && s_not_ready_score >= MAG_NOT_READY_FAULT_SCORE) {
            /* M-2b: a sensor that answers but keeps reading unfinished. */
            diag_inc(DIAG_MAG_RUNTIME_FAULT);
            s_not_ready_score = 0u;
            s_trig_ok = false;
            s_fault = true;
            s_probe_asap = true;
            s_state = MAG_FAULT;
        } else if (scheduled) {
            sample_start(next, cycle);
        }
        break;
    case MAG_BUSY:
        if (scheduled && sampling) {
            diag_inc(DIAG_MAG_MISSED_TICK);
        }
        diag_inc(DIAG_MAG_TIMEOUT);
        s_trig_ok = false;
        s_state = issue_abort() ? MAG_ABORTING : MAG_ABORTED;
        break;
    case MAG_ABORTING:
    case MAG_ABORTED:
        if (hw_idle()) {
            s_trig_ok = false;
            if (sampling) {
                s_state = MAG_IDLE;
                if (scheduled) {
                    sample_start(next, cycle); /* M-9: this half will be NO_PRIOR */
                }
            } else {
                s_config_pending = false;
                s_state = MAG_FAULT;
            }
        } else {
            if (scheduled && sampling) {
                diag_inc(DIAG_MAG_MISSED_TICK);
            }
            if (controller_hung()) {
                diag_inc(DIAG_MAG_TIMEOUT);
                s_state = issue_abort() ? MAG_ABORTING : MAG_ABORTED;
            }
        }
        break;
    default:
        break;
    }
    i2c_irq_unmask();
}

/* ---------------------------------------------------------- main loop -- */

bool __not_in_flash_func(mag_read_pair)(mag_pair_t *p) {
    uint32_t t0 = diag_cycles();
    cs_t cs = cs_enter();
    uint32_t l = s_latest;
    if (l < SLOT_NONE) {
        *p = s_pairs[l];
    }
    cs_exit(cs);
    diag_max(DIAG_T_API_MAG, diag_cycles() - t0);
    return l < SLOT_NONE;
}

/* --------------------------------------------------------------- init -- */

static bool reg_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_write_timeout_us(MAG_I2C, MAG_I2C_ADDR, b, 2, false, 5000) == 2;
}

static bool reg_read(uint8_t reg, uint8_t *val) {
    if (i2c_write_timeout_us(MAG_I2C, MAG_I2C_ADDR, &reg, 1, true, 5000) != 1) {
        return false;
    }
    return i2c_read_timeout_us(MAG_I2C, MAG_I2C_ADDR, val, 1, false, 5000) == 1;
}

#if MAG_BUS_CLEAR
static void od_low(uint pin) {
    gpio_put(pin, false);
    gpio_set_dir(pin, GPIO_OUT);
}

static void od_release(uint pin) {
    gpio_set_dir(pin, GPIO_IN);
}

/* M-2: one bus clear before the first I2C access (blocking, start-up). */
static void startup_bus_clear(void) {
    gpio_init(MAG_SDA_PIN);
    gpio_init(MAG_SCL_PIN);
    gpio_pull_up(MAG_SDA_PIN);
    gpio_pull_up(MAG_SCL_PIN);
    od_release(MAG_SDA_PIN);
    od_release(MAG_SCL_PIN);
    busy_wait_us(10);
    for (unsigned i = 0; i < 100u && !gpio_get(MAG_SCL_PIN); i++) {
        busy_wait_us(10);
    }
    for (unsigned i = 0; i < 9u; i++) {
        od_low(MAG_SCL_PIN);
        busy_wait_us(5);
        od_release(MAG_SCL_PIN);
        busy_wait_us(5);
    }
    od_low(MAG_SDA_PIN);
    busy_wait_us(5);
    od_release(MAG_SDA_PIN);
    busy_wait_us(5);
    diag_inc(DIAG_MAG_BUS_CLEAR);
}
#endif

bool mag_init(void) {
    sleep_ms(10); /* MMC5983MA power-on time */

#if MAG_BUS_CLEAR
    startup_bus_clear();
#endif

    i2c_init(MAG_I2C, MAG_I2C_HZ); /* M-1, at the 120 MHz clk_peri (R-3) */
    gpio_set_function(MAG_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MAG_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(MAG_SDA_PIN);
    gpio_pull_up(MAG_SCL_PIN);

    /* M-2: software reset (an MCU reset does not reset the sensor). */
    (void)reg_write(MMC_REG_CTRL1, MMC_CTRL1_SW_RST);
    sleep_ms(10);

    uint8_t id = 0;
    uint8_t st = 0;
    bool ok = reg_read(MMC_REG_PRODUCT_ID, &id) && id == MMC_PRODUCT_ID;
    ok = ok && reg_read(MMC_REG_STATUS, &st) && (st & MMC_STATUS_OTP_RD_DONE) != 0u;
    /* M-3 (amended): BW = 00; continuous mode off. */
    ok = ok && reg_write(MMC_REG_CTRL1, MMC_CTRL1_BW);
    ok = ok && reg_write(MMC_REG_CTRL2, 0u);
    ok = ok && reg_write(MMC_REG_CTRL3, 0u);
    /* M-4 start-up: one RESET -> SET conditioning, ending in SET. */
    ok = ok && reg_write(MMC_REG_CTRL0, (uint8_t)MMC_CTRL0_RESET);
    sleep_ms(1);
    ok = ok && reg_write(MMC_REG_CTRL0, (uint8_t)MMC_CTRL0_SET);
    sleep_ms(1);
    if (!ok) {
        diag_inc(DIAG_MAG_PRODUCT_ID_FAULT);
    }

    s_probe_cmd[0] = MMC_REG_PRODUCT_ID;                /* START + W(0x2F)  */
    s_probe_cmd[1] = CMD_RESTART | CMD_READ | CMD_STOP; /* RESTART R x1 STOP */
    s_cfg_cmd[0] = MMC_REG_CTRL1;
    s_cfg_cmd[1] = MMC_CTRL1_BW;
    s_cfg_cmd[2] = CMD_RESTART | MMC_REG_CTRL2;
    s_cfg_cmd[3] = 0u;
    s_cfg_cmd[4] = CMD_RESTART | MMC_REG_CTRL3;
    s_cfg_cmd[5] = 0u;
    s_cfg_cmd[6] = CMD_RESTART | MMC_REG_CTRL0;
    s_cfg_cmd[7] = MMC_CTRL0_RESET;
    s_cfg_cmd[8] = CMD_RESTART | MMC_REG_CTRL0;
    s_cfg_cmd[9] = CMD_STOP | MMC_CTRL0_SET;
#if MAG_PAIR_MODE
    s_cmd_s_len = (uint32_t)build_cmds(s_cmd_s, MMC_CTRL0_SET);
    s_cmd_r_len = (uint32_t)build_cmds(s_cmd_r, MMC_CTRL0_RESET);
#else
    s_cmd_s_len = (uint32_t)build_cmds(s_cmd_s, 0u); /* single: no pulses */
    s_cmd_r_len = s_cmd_s_len;
    memcpy(s_cmd_r, s_cmd_s, sizeof s_cmd_r);
#endif

    i2c_hw_t *hw = i2c_regs();
    /* The blocking calls above left IC_TAR = 0x30, which DMA reuses. */
    hw->dma_tdlr = 8u;
    hw->dma_rdlr = 0u;
    hw->dma_cr = I2C_IC_DMA_CR_TDMAE_BITS | I2C_IC_DMA_CR_RDMAE_BITS;

    /* USBH-S3: these claims run after core 1 claimed USB_HOST_TX_DMA_CH. */
    s_dma_cmd = (uint)dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_dma_cmd);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, i2c_get_dreq(MAG_I2C, true));
    dma_channel_configure(s_dma_cmd, &c, &hw->data_cmd, s_cmd_s, 0, false);

    s_dma_data = (uint)dma_claim_unused_channel(true);
    c = dma_channel_get_default_config(s_dma_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, i2c_get_dreq(MAG_I2C, false));
    dma_channel_configure(s_dma_data, &c, s_rx, &hw->data_cmd, 0, false);

    s_dma_mask = (1u << s_dma_cmd) | (1u << s_dma_data);

#if MAG_BUS_CLEAR
    /* USBH-S5: PIO1 (PIO0 belongs to PIO-USB). 160 ns per PIO cycle. */
    s_pio = pio1;
    s_sm = (uint)pio_claim_unused_sm(s_pio, true);
    uint offset = (uint)pio_add_program(s_pio, &i2c_bus_clear_program);
    pio_sm_config pc = i2c_bus_clear_program_get_default_config(offset);
    sm_config_set_set_pins(&pc, MAG_SCL_PIN, 1);
    sm_config_set_in_pins(&pc, MAG_SCL_PIN);
    uint32_t div256 = (uint32_t)(((uint64_t)clock_get_hz(clk_sys) * 256u) / 6250000u);
    sm_config_set_clkdiv_int_frac8(&pc, div256 >> 8, (uint8_t)(div256 & 0xffu));
    pio_sm_init(s_pio, s_sm, offset, &pc);
    pio_sm_set_pins_with_mask(s_pio, s_sm, 0u, 1u << MAG_SCL_PIN);
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 0u, 1u << MAG_SCL_PIN);
    pio_sm_set_enabled(s_pio, s_sm, true);
    s_bc_running = false;
#endif

    (void)hw->clr_intr;
    hw->intr_mask = I2C_IC_INTR_MASK_M_STOP_DET_BITS | I2C_IC_INTR_MASK_M_TX_ABRT_BITS;
    irq_set_exclusive_handler(MAG_I2C_IRQ, i2c_isr);
    irq_set_priority(MAG_I2C_IRQ, IRQ_PRIO_MAG);
    irq_set_enabled(MAG_I2C_IRQ, true);

    s_trig_ok = false; /* M-9: the first half read has no prior trigger */
    s_build_have_s = false;
    s_build.cycle = UINT64_MAX;
    s_txn = TXN_S;
    s_config_pending = false;
    s_probe_asap = !ok;
    s_fault = !ok;
    s_state = ok ? MAG_IDLE : MAG_FAULT;
    __dmb();
    s_ready = true;
    return ok;
}
