/* Test target shared definitions (VE-1, VE-2). */
#ifndef TARGET_H
#define TARGET_H

#include <stdint.h>

/* Descriptor variants (usb_descriptors.c). */
enum { DESC_CDC = 0, DESC_CDC_EP32 = 1, DESC_CDC_NOLINEREQ = 2, DESC_VENDOR = 3 };
extern volatile uint32_t g_desc_variant;

/* Pico 2 pins. */
#define PIN_UART_TX 0u   /* to the PC's USB-serial adapter RX   */
#define PIN_UART_RX 1u   /* from the adapter's TX               */
#define PIN_MARKER 2u    /* from the XIAO's D3 (R-trigger marker, V-3) */
#define PIN_COIL 3u      /* coil driver transistor (VE-4)        */
#define UART_BAUD 921600u

#endif
