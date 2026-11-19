/*
 * Native-USB diagnostic device (section 5.3, NU-1 .. NU-6). Diagnostic
 * builds only (APP_USB_STATUS = 1). Core 0 only (USBH-S1a rule 1).
 *
 * Composite device: CDC-ACM (status text out, commands in) plus the
 * Raspberry Pi reset interface (picotool), with an MS OS 2.0 descriptor so
 * Windows binds WinUSB to the reset interface.
 */
#ifndef USBD_DIAG_H
#define USBD_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#if APP_USB_STATUS

/* tud_init(0) and the interrupt priority (NU-3). Call before core 1 starts
 * (C1-8, USBH-S1a rule 3). */
void usbd_diag_init(void);

/* Run the device task once (non-blocking). Core 0 epoch only. */
void usbd_diag_task(void);

#else

static inline void usbd_diag_init(void) {}
static inline void usbd_diag_task(void) {}

#endif

#endif /* USBD_DIAG_H */
