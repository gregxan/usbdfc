/*
 * Test target USB descriptors (VE-2). The variant is chosen before each
 * (re)connect: normal CDC-ACM, CDC-ACM with 32-byte bulk endpoints,
 * CDC-ACM without line-request support, or a vendor-class interface.
 *
 * VID/PID: 0x1209/0x0001, the pid.codes test identifier reserved for
 * private testing. Not for distribution.
 */
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#include "target.h"

#define EP_NOTIF 0x81u
#define EP_OUT 0x02u
#define EP_IN 0x82u

enum { STR_LANG, STR_MANUF, STR_PRODUCT, STR_SERIAL, STR_ITF, STR_COUNT };

volatile uint32_t g_desc_variant = DESC_CDC;

static const tusb_desc_device_t s_dev = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x1209,
    .idProduct = 0x0001,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&s_dev;
}

#define CDC_TOTAL (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define VENDOR_TOTAL (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)
/* Self-powered on the bench (USBH-6), 100 mA allowance. */
#define CFG_ATTR TUSB_DESC_CONFIG_ATT_SELF_POWERED

static const uint8_t s_cfg_cdc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CDC_TOTAL, CFG_ATTR, 100),
    TUD_CDC_DESCRIPTOR(0, STR_ITF, EP_NOTIF, 8, EP_OUT, EP_IN, 64),
};

static const uint8_t s_cfg_cdc32[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CDC_TOTAL, CFG_ATTR, 100),
    TUD_CDC_DESCRIPTOR(0, STR_ITF, EP_NOTIF, 8, EP_OUT, EP_IN, 32),
};

/* TUD_CDC_DESCRIPTOR with ACM bmCapabilities = 0: no line requests (V-11). */
static const uint8_t s_cfg_cdc_nolr[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CDC_TOTAL, CFG_ATTR, 100),
    8, TUSB_DESC_INTERFACE_ASSOCIATION, 0, 2, TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL,
    CDC_COMM_PROTOCOL_NONE, 0,
    9, TUSB_DESC_INTERFACE, 0, 0, 1, TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL,
    CDC_COMM_PROTOCOL_NONE, STR_ITF,
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER, U16_TO_U8S_LE(0x0120),
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_CALL_MANAGEMENT, 0, 1,
    4, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT, 0,
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION, 0, 1,
    7, TUSB_DESC_ENDPOINT, EP_NOTIF, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(8), 16,
    9, TUSB_DESC_INTERFACE, 1, 0, 2, TUSB_CLASS_CDC_DATA, 0, 0, 0,
    7, TUSB_DESC_ENDPOINT, EP_OUT, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
    7, TUSB_DESC_ENDPOINT, EP_IN, TUSB_XFER_BULK, U16_TO_U8S_LE(64), 0,
};

static const uint8_t s_cfg_vendor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, VENDOR_TOTAL, CFG_ATTR, 100),
    TUD_VENDOR_DESCRIPTOR(0, STR_ITF, EP_OUT, EP_IN, 64),
};

_Static_assert(sizeof s_cfg_cdc == CDC_TOTAL && sizeof s_cfg_cdc32 == CDC_TOTAL &&
                   sizeof s_cfg_cdc_nolr == CDC_TOTAL && sizeof s_cfg_vendor == VENDOR_TOTAL,
               "configuration descriptor lengths");

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    switch (g_desc_variant) {
    case DESC_CDC_EP32:
        return s_cfg_cdc32;
    case DESC_CDC_NOLINEREQ:
        return s_cfg_cdc_nolr;
    case DESC_VENDOR:
        return s_cfg_vendor;
    default:
        return s_cfg_cdc;
    }
}

static char s_serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
static const char *const s_str[STR_COUNT] = {
    [STR_MANUF] = "usbdfc",
    [STR_PRODUCT] = "usbdfc test target",
    [STR_SERIAL] = s_serial,
    [STR_ITF] = "usbdfc test target",
};
static uint16_t s_buf[33];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t n;
    if (index == STR_LANG) {
        s_buf[1] = 0x0409;
        n = 1;
    } else {
        if (index >= STR_COUNT || s_str[index] == NULL) {
            return NULL;
        }
        if (index == STR_SERIAL && s_serial[0] == '\0') {
            pico_get_unique_board_id_string(s_serial, sizeof s_serial);
        }
        const char *str = s_str[index];
        n = strlen(str);
        if (n > 32u) {
            n = 32u;
        }
        for (size_t i = 0; i < n; i++) {
            s_buf[1u + i] = (uint16_t)(uint8_t)str[i];
        }
    }
    s_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * n + 2u));
    return s_buf;
}
