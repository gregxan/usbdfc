/* MMC5983MA register map (subset). Datasheet Rev A, 4/3/2019.
 * Adapted from DFC Rev 2.16 src/mmc5983ma.h (B-26). */
#ifndef MMC5983MA_H
#define MMC5983MA_H

#define MMC_REG_XOUT0 0x00u
#define MMC_REG_STATUS 0x08u
#define MMC_REG_CTRL0 0x09u
#define MMC_REG_CTRL1 0x0Au
#define MMC_REG_CTRL2 0x0Bu
#define MMC_REG_CTRL3 0x0Cu
#define MMC_REG_PRODUCT_ID 0x2Fu

#define MMC_PRODUCT_ID 0x30u

/* Internal Control 0 */
#define MMC_CTRL0_TM_M (1u << 0)
#define MMC_CTRL0_TM_T (1u << 1)
#define MMC_CTRL0_INT_DONE_EN (1u << 2)
#define MMC_CTRL0_SET (1u << 3)   /* 500 ns SET pulse, self-clearing   */
#define MMC_CTRL0_RESET (1u << 4) /* 500 ns RESET pulse, self-clearing */
#define MMC_CTRL0_AUTO_SR_EN (1u << 5)

/* Internal Control 1: BW[1:0] (p. 15). 0b00 = 8 ms, 100 Hz (M-3). */
#define MMC_CTRL1_BW_100HZ 0x00u
#define MMC_CTRL1_BW_200HZ 0x01u /* M-23 fallback: 4 ms */
#define MMC_CTRL1_SW_RST (1u << 7) /* software reset; 10 ms power-on time */

#ifndef MMC_CTRL1_BW
#define MMC_CTRL1_BW MMC_CTRL1_BW_100HZ
#endif

/* Status */
#define MMC_STATUS_MEAS_M_DONE (1u << 0)
#define MMC_STATUS_OTP_RD_DONE (1u << 4) /* factory OTP loaded */

/* Field bytes per sample: Xout0, Xout1, Yout0, Yout1, Zout0, Zout1, XYZout2 */
#define MMC_SAMPLE_BYTES 7u
/* Bytes read per transaction: 0x00..0x08 (field, Tout, Status). */
#define MMC_READ_BYTES 9u

/* 18-bit null-field output (p. 2). */
#define MMC_NULL_COUNTS 131072u
#define MMC_FULL_SCALE_COUNTS 262144u

#endif /* MMC5983MA_H */
