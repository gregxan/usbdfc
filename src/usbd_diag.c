#include "usbd_diag.h"

#if APP_USB_STATUS

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "pico/usb_reset_interface.h"

#include "device/usbd_pvt.h"
#include "tusb.h"

/* NU-6: the Pico SDK's default Raspberry Pi VID/PID for development.
 * Release gate: a dedicated PID from Raspberry Pi before distribution. */
#define USBD_VID 0x2E8Au
#define USBD_PID 0x0009u

#define ITF_CDC 0u /* two interfaces */
#define ITF_RESET 2u
#define ITF_COUNT 3u

#define EP_CDC_NOTIF 0x81u
#define EP_CDC_OUT 0x02u
#define EP_CDC_IN 0x82u

enum { STR_LANG, STR_MANUF, STR_PRODUCT, STR_SERIAL, STR_CDC, STR_RESET, STR_COUNT };

#define TUD_RPI_RESET_DESC_LEN 9u
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN)

/* NU-5: before any reboot, drop VBUS so the downstream device detaches
 * cleanly (HW-12). */
static void vbus_off(void) {
    gpio_put(VBUS_EN_PIN, false);
}

/* ------------------------------------------------------- descriptors -- */

static const tusb_desc_device_t s_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210, /* 2.1: BOS for the MS OS 2.0 descriptor */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t s_desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    /* Raspberry Pi reset interface: vendor class, no endpoints. */
    9, TUSB_DESC_INTERFACE, ITF_RESET, 0, 0, TUSB_CLASS_VENDOR_SPECIFIC, RESET_INTERFACE_SUBCLASS,
    RESET_INTERFACE_PROTOCOL, STR_RESET,
};

_Static_assert(sizeof(s_desc_config) == CONFIG_TOTAL_LEN, "configuration descriptor length");

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&s_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return s_desc_config;
}

/* MS OS 2.0: WinUSB on the reset interface, with the GUID the Pico SDK
 * uses, so picotool finds it as it does for SDK stdio builds. */
#define MS_OS_20_DESC_LEN 166u
#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define VENDOR_REQUEST_MICROSOFT 1u

static const uint8_t s_desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

const uint8_t *tud_descriptor_bos_cb(void) {
    return s_desc_bos;
}

static const uint8_t s_desc_ms_os_20[] = {
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000),
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_RESET, 0, U16_TO_U8S_LE(0x009C),
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0080), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY), U16_TO_U8S_LE(0x0001),
    U16_TO_U8S_LE(0x0028),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0, 'e', 0,
    'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0, 'D', 0, 0, 0,
    U16_TO_U8S_LE(0x004E),
    '{', 0, 'b', 0, 'c', 0, '7', 0, '3', 0, '9', 0, '8', 0, 'c', 0, '1', 0, '-', 0,
    '7', 0, '3', 0, 'c', 0, 'd', 0, '-', 0, '4', 0, 'c', 0, 'b', 0, '7', 0, '-', 0,
    '9', 0, '8', 0, 'b', 0, '8', 0, '-', 0, '9', 0, '1', 0, '3', 0, 'a', 0, '8', 0,
    'f', 0, 'c', 0, 'a', 0, '7', 0, 'b', 0, 'f', 0, '6', 0, '}', 0, 0, 0,
};

_Static_assert(sizeof(s_desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor length");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bRequest == VENDOR_REQUEST_MICROSOFT && request->wIndex == 7u) {
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_desc_ms_os_20, sizeof s_desc_ms_os_20);
    }
    return false;
}

/* NU-6: serial number = flash unique ID. */
static char s_serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
static const char *const s_strings[STR_COUNT] = {
    [STR_MANUF] = "usbdfc",
    [STR_PRODUCT] = "usbdfc diagnostic build",
    [STR_SERIAL] = s_serial,
    [STR_CDC] = "usbdfc status",
    [STR_RESET] = "Reset",
};
static uint16_t s_str_buf[33];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t n;
    if (index == STR_LANG) {
        s_str_buf[1] = 0x0409;
        n = 1;
    } else {
        if (index >= STR_COUNT || s_strings[index] == NULL) {
            return NULL;
        }
        const char *str = s_strings[index];
        n = strlen(str);
        if (n > 32u) {
            n = 32u;
        }
        for (size_t i = 0; i < n; i++) {
            s_str_buf[1u + i] = (uint16_t)(uint8_t)str[i];
        }
    }
    s_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * n + 2u));
    return s_str_buf;
}

/* --------------------------------------------- NU-5: reset interface -- */

static uint8_t s_reset_itf;

static void resetd_init(void) {}

static void resetd_reset(uint8_t rhport) {
    (void)rhport;
    s_reset_itf = 0xFFu;
}

static uint16_t resetd_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
    (void)rhport;
    TU_VERIFY(itf->bInterfaceClass == TUSB_CLASS_VENDOR_SPECIFIC &&
                  itf->bInterfaceSubClass == RESET_INTERFACE_SUBCLASS &&
                  itf->bInterfaceProtocol == RESET_INTERFACE_PROTOCOL,
              0);
    TU_VERIFY(max_len >= sizeof(tusb_desc_interface_t), 0);
    s_reset_itf = itf->bInterfaceNumber;
    return (uint16_t)sizeof(tusb_desc_interface_t);
}

static bool resetd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    (void)rhport;
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->wIndex != s_reset_itf) {
        return false;
    }
    if (request->bRequest == RESET_REQUEST_BOOTSEL) {
        vbus_off();
        rom_reset_usb_boot_extra(-1, (uint32_t)(request->wValue & 0x7fu), false); /* does not return */
    }
    if (request->bRequest == RESET_REQUEST_FLASH) {
        vbus_off();
        watchdog_reboot(0, 0, 100);
        return true;
    }
    return false;
}

static bool resetd_xfer_cb(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t n) {
    (void)rhport;
    (void)ep;
    (void)result;
    (void)n;
    return true;
}

static const usbd_class_driver_t s_resetd = {
#if CFG_TUSB_DEBUG >= 2
    .name = "RESET",
#endif
    .init = resetd_init,
    .reset = resetd_reset,
    .open = resetd_open,
    .control_xfer_cb = resetd_control_xfer_cb,
    .xfer_cb = resetd_xfer_cb,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &s_resetd;
}

/* NU-5: the 1200-baud touch reboots into BOOTSEL. */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding) {
    (void)itf;
    if (coding->bit_rate == 1200u) {
        vbus_off();
        rom_reset_usb_boot_extra(-1, 0u, false);
    }
}

/* ------------------------------------------------------------- API ---- */

void usbd_diag_init(void) {
    pico_get_unique_board_id_string(s_serial, sizeof s_serial);
    (void)tud_init(BOARD_TUD_RHPORT);
    irq_set_priority(USBCTRL_IRQ, IRQ_PRIO_USB); /* NU-3: lowest */
}

void usbd_diag_task(void) {
    tud_task_ext(0, false); /* non-blocking */
}

#endif /* APP_USB_STATUS */
