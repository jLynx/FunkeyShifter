#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "tusb_config.h"

// Commented out temporarily for validation testing with other pid/vid
// #define FUNKEY_USB_VID 0x1209
// #define FUNKEY_USB_PID 0x0001
// #define FUNKEY_USB_VID 0x0E4B
// #define FUNKEY_USB_PID 0x301A
#define FUNKEY_USB_VID 0x0E4C
#define FUNKEY_USB_PID 0x7288
#define FUNKEY_BCD_DEVICE 0x0100

#define FUNKEY_REPORT_LEN 8
#define FUNKEY_PORTAL_PACKET_LEN 7
#define FUNKEY_PORTAL_DESC_LEN (9 + 7)

#define FUNKEY_ITF_PORTAL 0
#define FUNKEY_ITF_TOTAL 1

#define FUNKEY_EP_PORTAL_IN 0x81

#define FUNKEY_REQ_INIT 0x00
#define FUNKEY_REQ_GET_REPORT 0x01
#define FUNKEY_REQ_SET_REPORT 0x02
#define FUNKEY_REQ_REMOVE 0x03
#define FUNKEY_REQ_SET_RAW_PACKET 0x04

#define TUD_FUNKEY_PORTAL_DESCRIPTOR(_itfnum, _stridx, _epin, _epsize, _interval) \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 1, TUSB_CLASS_VENDOR_SPECIFIC, 0x00, 0x00, _stridx, \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(_epsize), _interval

static const char *TAG = "funkey_shifter";

static portMUX_TYPE s_report_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_current_report[FUNKEY_REPORT_LEN] = {0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00};
static volatile bool s_report_dirty = true;

static uint8_t s_pending_control_report[FUNKEY_REPORT_LEN];
static bool s_pending_control_set = false;
static uint8_t s_pending_raw_packet[FUNKEY_PORTAL_PACKET_LEN];
static bool s_pending_raw_set = false;

static uint8_t s_portal_ep_in = 0;
static uint8_t s_portal_report_xfer[FUNKEY_PORTAL_PACKET_LEN];

static const uint8_t s_raw_no_figure[FUNKEY_PORTAL_PACKET_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
static const uint16_t s_portal_baseline_adc = 177;
static const uint16_t s_portal_digit_adc[10] = {
    278, 375, 465, 554, 643, 713, 783, 844, 899, 942,
};

static uint8_t s_portal_stable_packet[FUNKEY_PORTAL_PACKET_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00};

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0110,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = FUNKEY_USB_VID,
    .idProduct = FUNKEY_USB_PID,
    .bcdDevice = FUNKEY_BCD_DEVICE,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x00,
    .bNumConfigurations = 0x01,
};

static const char *s_string_descriptor[] = {
    (const char[]){0x09, 0x04},
    "Funkey Revival",
    "Funkey Shifter Portal",
};

static const uint16_t s_config_descriptor_len = TUD_CONFIG_DESC_LEN + FUNKEY_PORTAL_DESC_LEN;

static const uint8_t s_config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, FUNKEY_ITF_TOTAL, 0, TUD_CONFIG_DESC_LEN + FUNKEY_PORTAL_DESC_LEN, 0, 100),
    TUD_FUNKEY_PORTAL_DESCRIPTOR(FUNKEY_ITF_PORTAL, 0, FUNKEY_EP_PORTAL_IN, FUNKEY_REPORT_LEN, 10),
};

static void report_copy(uint8_t out[FUNKEY_REPORT_LEN])
{
    portENTER_CRITICAL(&s_report_mux);
    memcpy(out, s_current_report, FUNKEY_REPORT_LEN);
    portEXIT_CRITICAL(&s_report_mux);
}

static bool report_is_removed(const uint8_t report[FUNKEY_REPORT_LEN])
{
    static const uint8_t removed[FUNKEY_REPORT_LEN] = {0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00};
    return memcmp(report, removed, FUNKEY_REPORT_LEN) == 0;
}

static uint32_t report_id(const uint8_t report[FUNKEY_REPORT_LEN])
{
    return ((uint32_t)report[4] << 24) | ((uint32_t)report[5] << 16) |
           ((uint32_t)report[6] << 8) | (uint32_t)report[7];
}

static void portal_set_stable_packet_locked(const uint8_t packet[FUNKEY_PORTAL_PACKET_LEN])
{
    memcpy(s_portal_stable_packet, packet, FUNKEY_PORTAL_PACKET_LEN);
}

static bool portal_packet_from_id(uint32_t id, uint8_t out[FUNKEY_PORTAL_PACKET_LEN])
{
    if (id > 999) {
        return false;
    }

    uint8_t digits[4] = {
        id % 10,
        (id / 10) % 10,
        (id / 100) % 10,
        0,
    };
    digits[3] = (digits[0] + digits[1] + digits[2]) % 10;

    memset(out, 0, FUNKEY_PORTAL_PACKET_LEN);
    for (size_t i = 0; i < 4; ++i) {
        uint16_t adc = s_portal_digit_adc[digits[i]];
        out[i] = adc & 0xFF;
        out[4] |= ((adc >> 8) & 0x03) << (i * 2);
    }

    out[5] = s_portal_baseline_adc & 0xFF;
    out[6] = (s_portal_baseline_adc >> 8) & 0xFF;
    return true;
}

static void portal_packet_for_report_locked(const uint8_t report[FUNKEY_REPORT_LEN])
{
    if (report_is_removed(report)) {
        portal_set_stable_packet_locked(s_raw_no_figure);
        return;
    }

    uint8_t packet[FUNKEY_PORTAL_PACKET_LEN];
    if (portal_packet_from_id(report_id(report), packet)) {
        portal_set_stable_packet_locked(packet);
    } else {
        portal_set_stable_packet_locked(s_raw_no_figure);
    }
}

static void report_set(const uint8_t in[FUNKEY_REPORT_LEN])
{
    portENTER_CRITICAL(&s_report_mux);
    memcpy(s_current_report, in, FUNKEY_REPORT_LEN);
    portal_packet_for_report_locked(in);
    s_report_dirty = true;
    portEXIT_CRITICAL(&s_report_mux);
}

static void report_remove(void)
{
    const uint8_t removed[FUNKEY_REPORT_LEN] = {0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00};
    report_set(removed);
}

static void portal_set_raw_packet(const uint8_t packet[FUNKEY_PORTAL_PACKET_LEN])
{
    portENTER_CRITICAL(&s_report_mux);
    portal_set_stable_packet_locked(packet);
    s_report_dirty = true;
    portEXIT_CRITICAL(&s_report_mux);
}

static bool send_portal_report(void)
{
    if (!tud_mounted() || s_portal_ep_in == 0 || usbd_edpt_busy(0, s_portal_ep_in)) {
        return false;
    }

    portENTER_CRITICAL(&s_report_mux);
    memcpy(s_portal_report_xfer, s_portal_stable_packet, FUNKEY_PORTAL_PACKET_LEN);
    portEXIT_CRITICAL(&s_report_mux);

    if (usbd_edpt_busy(0, s_portal_ep_in) || !usbd_edpt_claim(0, s_portal_ep_in)) {
        return false;
    }

    if (!usbd_edpt_xfer(0, s_portal_ep_in, s_portal_report_xfer, FUNKEY_PORTAL_PACKET_LEN)) {
        usbd_edpt_release(0, s_portal_ep_in);
        return false;
    }

    return true;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    static const uint8_t init_response[4] = {0x00, 0x00, 0x00, 0x00};
    static uint8_t get_report_buf[FUNKEY_REPORT_LEN];

    if (stage == CONTROL_STAGE_ACK && s_pending_control_set) {
        s_pending_control_set = false;
        report_set(s_pending_control_report);
        return true;
    }

    if (stage == CONTROL_STAGE_ACK && s_pending_raw_set) {
        s_pending_raw_set = false;
        portal_set_raw_packet(s_pending_raw_packet);
        return true;
    }

    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bRequest == FUNKEY_REQ_INIT &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->wLength > 0) {
        uint16_t len = request->wLength < sizeof(init_response) ? request->wLength : sizeof(init_response);
        return tud_control_xfer(rhport, request, (void *)init_response, len);
    }

    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) {
        return false;
    }

    switch (request->bRequest) {
    case FUNKEY_REQ_GET_REPORT:
        if (request->bmRequestType_bit.direction != TUSB_DIR_IN || request->wLength == 0) {
            return false;
        }
        report_copy(get_report_buf);
        return tud_control_xfer(rhport, request, get_report_buf,
                                request->wLength < FUNKEY_REPORT_LEN ? request->wLength : FUNKEY_REPORT_LEN);

    case FUNKEY_REQ_SET_REPORT:
        if (request->bmRequestType_bit.direction != TUSB_DIR_OUT || request->wLength != FUNKEY_REPORT_LEN) {
            return false;
        }
        s_pending_control_set = true;
        return tud_control_xfer(rhport, request, s_pending_control_report, FUNKEY_REPORT_LEN);

    case FUNKEY_REQ_REMOVE:
        if (request->bmRequestType_bit.direction != TUSB_DIR_OUT || request->wLength != 0) {
            return false;
        }
        report_remove();
        return tud_control_status(rhport, request);

    case FUNKEY_REQ_SET_RAW_PACKET:
        if (request->bmRequestType_bit.direction != TUSB_DIR_OUT || request->wLength != FUNKEY_PORTAL_PACKET_LEN) {
            return false;
        }
        s_pending_raw_set = true;
        return tud_control_xfer(rhport, request, s_pending_raw_packet, FUNKEY_PORTAL_PACKET_LEN);

    default:
        return false;
    }
}

void tud_vendor_tx_cb(uint8_t idx, uint32_t sent_bytes)
{
    (void)idx;
    (void)sent_bytes;
}

void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
{
    (void)idx;
    (void)buffer;
    (void)bufsize;
}

static void portal_driver_init(void)
{
    s_portal_ep_in = 0;
}

static bool portal_driver_deinit(void)
{
    s_portal_ep_in = 0;
    return true;
}

static void portal_driver_reset(uint8_t rhport)
{
    (void)rhport;
    s_portal_ep_in = 0;
}

static uint16_t portal_driver_open(uint8_t rhport, const tusb_desc_interface_t *desc_itf, uint16_t max_len)
{
    if (desc_itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC || desc_itf->bInterfaceNumber != FUNKEY_ITF_PORTAL) {
        return 0;
    }

    const uint8_t *p_desc = tu_desc_next(desc_itf);
    const uint8_t *desc_end = (const uint8_t *)desc_itf + max_len;

    while (tu_desc_in_bounds(p_desc, desc_end)) {
        uint8_t desc_type = tu_desc_type(p_desc);
        if (desc_type == TUSB_DESC_INTERFACE || desc_type == TUSB_DESC_INTERFACE_ASSOCIATION) {
            break;
        }

        if (desc_type == TUSB_DESC_ENDPOINT) {
            const tusb_desc_endpoint_t *desc_ep = (const tusb_desc_endpoint_t *)p_desc;
            if (desc_ep->bEndpointAddress == FUNKEY_EP_PORTAL_IN) {
                if (!usbd_edpt_open(rhport, desc_ep)) {
                    return 0;
                }
                s_portal_ep_in = desc_ep->bEndpointAddress;
            }
        }

        p_desc = tu_desc_next(p_desc);
    }

    return s_portal_ep_in == 0 ? 0 : (uint16_t)((uintptr_t)p_desc - (uintptr_t)desc_itf);
}

static bool portal_driver_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
    static const uint8_t init_response[4] = {0x00, 0x00, 0x00, 0x00};

    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bRequest == FUNKEY_REQ_INIT &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->wLength > 0) {
        uint16_t len = request->wLength < sizeof(init_response) ? request->wLength : sizeof(init_response);
        return tud_control_xfer(rhport, request, (void *)init_response, len);
    }

    return false;
}

static bool portal_driver_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    (void)rhport;
    (void)result;
    (void)xferred_bytes;
    return ep_addr == s_portal_ep_in;
}

static const usbd_class_driver_t s_app_drivers[] = {
    {
        .name = "FUNKEY_PORTAL",
        .init = portal_driver_init,
        .deinit = portal_driver_deinit,
        .reset = portal_driver_reset,
        .open = portal_driver_open,
        .control_xfer_cb = portal_driver_control_xfer_cb,
        .xfer_cb = portal_driver_xfer_cb,
        .xfer_isr = NULL,
        .sof = NULL,
    },
};

const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = TU_ARRAY_SIZE(s_app_drivers);
    return s_app_drivers;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Funkey Shifter Portal USB device");
    ESP_LOGI(TAG, "VID:PID %04X:%04X", FUNKEY_USB_VID, FUNKEY_USB_PID);

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_descriptor;
    tusb_cfg.descriptor.string = s_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]);
    tusb_cfg.descriptor.full_speed_config = s_config_descriptor;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_config_descriptor;
#endif

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    ESP_LOGI(TAG, "USB initialized");

    while (true) {
        if (send_portal_report()) {
            portENTER_CRITICAL(&s_report_mux);
            s_report_dirty = false;
            portEXIT_CRITICAL(&s_report_mux);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
