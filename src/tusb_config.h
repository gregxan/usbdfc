/*
 * TinyUSB configuration (USBH-S1, NU-4). Production builds: host only.
 * Diagnostic builds (APP_USB_STATUS = 1) add the native-USB device
 * (section 5.3); not implemented yet, so the device stack stays off.
 */
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

/* ---------------------------------------------------------- Host (core 1) */

#define BOARD_TUH_RHPORT 1 /* root port 1 = PIO-USB */
#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 1
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED

#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB 0        /* USBH-1a: no hubs */
#define CFG_TUH_DEVICE_MAX 1
#define CFG_TUH_CDC 1
#define CFG_TUH_CDC_FTDI 0   /* USBH-2: CDC-ACM only */
#define CFG_TUH_CDC_CP210X 0
#define CFG_TUH_CDC_CH34X 0
#define CFG_TUH_HID 0
#define CFG_TUH_MSC 0
#define CFG_TUH_VENDOR 0

/* USBH-4: DTR = 1, RTS = 1, then 115200 8N1 */
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM 0x03
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM {115200, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8}

/* One 48-byte record per transfer (REC-1, USBH-T6). */
#define CFG_TUH_CDC_TX_BUFSIZE 64
#define CFG_TUH_CDC_TX_EPSIZE 64
#define CFG_TUH_CDC_RX_BUFSIZE 64
#define CFG_TUH_CDC_RX_EPSIZE 64

#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif
#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
#endif

/* ------------------------------------------------------ Device (core 0) */

#define CFG_TUD_ENABLED 0 /* NU-1: diagnostic builds only (not yet built) */

#endif /* TUSB_CONFIG_H_ */
