# B-25 (extended): per-file exceptions to B-22/B-23 for third-party sources.
# Each relaxes as little as possible: warnings stay reported, they are only
# not fatal. Review whenever the SDK pin (B-9) or USBH-S2 changes.
# Recorded from the first build at Pico SDK 2.2.0 / TinyUSB 86ad6e5 /
# Pico-PIO-USB fe9133f.

set(_pio_usb ${PICO_PIO_USB_PATH}/src)

#  2. pio_usb.c: 16 -Wconversion / -Wsign-conversion warnings (integer
#     narrowing in pin and PIO set-up, and in the generated usb_rx.pio.h it
#     includes). Library code at a pinned commit; not modified (USBH-S2).
set_source_files_properties(${_pio_usb}/pio_usb.c
    PROPERTIES COMPILE_OPTIONS "-Wno-error=conversion;-Wno-error=sign-conversion")

#  3. pio_usb_host.c: 29 -Wconversion / -Wsign-conversion warnings, as above.
#     Also -Wstack-usage: enumerate_device() (1224 bytes, a 512-byte buffer)
#     belongs to Pico-PIO-USB's own standalone host stack, which is not used
#     with TinyUSB (PIO_USB_USE_TINYUSB) and is removed by --gc-sections.
set_source_files_properties(${_pio_usb}/pio_usb_host.c
    PROPERTIES COMPILE_OPTIONS "-Wno-error=conversion;-Wno-error=sign-conversion;-Wno-error=stack-usage=")

#  4. pio_usb_device.c: 12 -Wconversion / -Wsign-conversion warnings. The
#     PIO-USB device mode is not used (the SDK target compiles it anyway).
set_source_files_properties(${_pio_usb}/pio_usb_device.c
    PROPERTIES COMPILE_OPTIONS "-Wno-error=conversion;-Wno-error=sign-conversion")

#  5. pico_rand/rand.c (SDK 2.2.0, line 173): one -Wconversion warning, a
#     uint64_t stored into the 32-bit TRNG_CONFIG register. Used for
#     boot_id (REC-4).
set_source_files_properties(${PICO_SDK_PATH}/src/rp2_common/pico_rand/rand.c
    PROPERTIES COMPILE_OPTIONS "-Wno-error=conversion")
