#include "funkey_usb.hpp"

#include <stdint.h>
#include <string.h>

#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "esp_err.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "tusb_config.h"

#include "funkey_ble.hpp"
#include "funkey_protocol.hpp"
#include "funkey_report.hpp"

#define TUD_FUNKEY_PORTAL_DESCRIPTOR(_itfnum, _stridx, _epin, _epsize, _interval) \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 1, TUSB_CLASS_VENDOR_SPECIFIC, 0x00, 0x00, _stridx, \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(_epsize), _interval

static uint8_t s_pending_control_report[FUNKEY_REPORT_LEN];
static bool s_pending_control_set = false;
static uint8_t s_pending_raw_packet[FUNKEY_PORTAL_PACKET_LEN];
static bool s_pending_raw_set = false;

static uint8_t s_portal_ep_in = 0;
static uint8_t s_portal_report_xfer[FUNKEY_PORTAL_PACKET_LEN];

static tusb_desc_device_t make_device_descriptor()
{
    tusb_desc_device_t descriptor = {};
    descriptor.bLength = static_cast<uint8_t>(sizeof(tusb_desc_device_t));
    descriptor.bDescriptorType = TUSB_DESC_DEVICE;
    descriptor.bcdUSB = 0x0110;
    descriptor.bDeviceClass = 0x00;
    descriptor.bDeviceSubClass = 0x00;
    descriptor.bDeviceProtocol = 0x00;
    descriptor.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE;
    descriptor.idVendor = FUNKEY_USB_VID;
    descriptor.idProduct = FUNKEY_USB_PID;
    descriptor.bcdDevice = FUNKEY_BCD_DEVICE;
    descriptor.iManufacturer = 0x01;
    descriptor.iProduct = 0x02;
    descriptor.iSerialNumber = 0x00;
    descriptor.bNumConfigurations = 0x01;
    return descriptor;
}

static const tusb_desc_device_t s_device_descriptor = make_device_descriptor();

static const char s_lang_id_descriptor[] = {0x09, 0x04};

static const char *s_string_descriptor[] = {
    s_lang_id_descriptor,
    "Radica Games, Ltd",
    "MegaByte Portal",
};

static const uint8_t s_config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, FUNKEY_ITF_TOTAL, 0,
                          TUD_CONFIG_DESC_LEN + FUNKEY_PORTAL_DESC_LEN, 0, 100),
    TUD_FUNKEY_PORTAL_DESCRIPTOR(FUNKEY_ITF_PORTAL, 0, FUNKEY_EP_PORTAL_IN,
                                 FUNKEY_REPORT_LEN, 10),
};

static uint16_t response_len(uint16_t requested, size_t available)
{
    return requested < available ? requested : (uint16_t)available;
}

extern "C" bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                           tusb_control_request_t const *request)
{
    static const uint8_t init_response[4] = {0x00, 0x00, 0x00, 0x00};
    static uint8_t get_report_buf[FUNKEY_REPORT_LEN];
    static uint8_t ble_status_buf[FUNKEY_BLE_STATUS_LEN];

    if (stage == CONTROL_STAGE_ACK && s_pending_control_set) {
        s_pending_control_set = false;
        funkey::report::set(s_pending_control_report);
        return true;
    }

    if (stage == CONTROL_STAGE_ACK && s_pending_raw_set) {
        s_pending_raw_set = false;
        funkey::portal::setRawPacket(s_pending_raw_packet);
        return true;
    }

    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bRequest == FUNKEY_REQ_INIT &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->wLength > 0) {
        return tud_control_xfer(rhport, request, (void *)init_response,
                                response_len(request->wLength, sizeof(init_response)));
    }

    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) {
        return false;
    }

    switch (request->bRequest) {
    case FUNKEY_REQ_GET_REPORT:
        if (request->bmRequestType_bit.direction != TUSB_DIR_IN || request->wLength == 0) {
            return false;
        }
        funkey::report::copy(get_report_buf);
        return tud_control_xfer(rhport, request, get_report_buf,
                                response_len(request->wLength, FUNKEY_REPORT_LEN));

    case FUNKEY_REQ_GET_BLE_STATUS:
        if (request->bmRequestType_bit.direction != TUSB_DIR_IN || request->wLength == 0) {
            return false;
        }
        funkey::ble::copyStatus(ble_status_buf);
        return tud_control_xfer(rhport, request, ble_status_buf,
                                response_len(request->wLength, FUNKEY_BLE_STATUS_LEN));

    case FUNKEY_REQ_GET_CAPABILITIES:
        if (request->bmRequestType_bit.direction != TUSB_DIR_IN || request->wLength == 0) {
            return false;
        }
        return tud_control_xfer(rhport, request,
                                (void *)funkey::protocol::capabilitiesResponse(),
                                response_len(request->wLength,
                                             funkey::protocol::capabilitiesResponseLen()));

    case FUNKEY_REQ_SET_REPORT:
        if (request->bmRequestType_bit.direction != TUSB_DIR_OUT ||
            request->wLength != FUNKEY_REPORT_LEN) {
            return false;
        }
        s_pending_control_set = true;
        return tud_control_xfer(rhport, request, s_pending_control_report, FUNKEY_REPORT_LEN);

    case FUNKEY_REQ_REMOVE:
        if (request->bmRequestType_bit.direction != TUSB_DIR_OUT || request->wLength != 0) {
            return false;
        }
        funkey::report::remove();
        return tud_control_status(rhport, request);

    case FUNKEY_REQ_SET_RAW_PACKET:
        if (request->bmRequestType_bit.direction != TUSB_DIR_OUT ||
            request->wLength != FUNKEY_PORTAL_PACKET_LEN) {
            return false;
        }
        s_pending_raw_set = true;
        return tud_control_xfer(rhport, request, s_pending_raw_packet, FUNKEY_PORTAL_PACKET_LEN);

    default:
        return false;
    }
}

extern "C" void tud_vendor_tx_cb(uint8_t idx, uint32_t sent_bytes)
{
    (void)idx;
    (void)sent_bytes;
}

extern "C" void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize)
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

static uint16_t portal_driver_open(uint8_t rhport, const tusb_desc_interface_t *desc_itf,
                                   uint16_t max_len)
{
    if (desc_itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC ||
        desc_itf->bInterfaceNumber != FUNKEY_ITF_PORTAL) {
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

static bool portal_driver_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                          const tusb_control_request_t *request)
{
    static const uint8_t init_response[4] = {0x00, 0x00, 0x00, 0x00};
    static uint8_t ble_status_buf[FUNKEY_BLE_STATUS_LEN];

    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bRequest == FUNKEY_REQ_INIT &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->wLength > 0) {
        return tud_control_xfer(rhport, request, (void *)init_response,
                                response_len(request->wLength, sizeof(init_response)));
    }

    if (request->bRequest == FUNKEY_REQ_GET_BLE_STATUS &&
        request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->wLength > 0) {
        funkey::ble::copyStatus(ble_status_buf);
        return tud_control_xfer(rhport, request, ble_status_buf,
                                response_len(request->wLength, FUNKEY_BLE_STATUS_LEN));
    }

    if (request->bRequest == FUNKEY_REQ_GET_CAPABILITIES &&
        request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->wLength > 0) {
        return tud_control_xfer(rhport, request,
                                (void *)funkey::protocol::capabilitiesResponse(),
                                response_len(request->wLength,
                                             funkey::protocol::capabilitiesResponseLen()));
    }

    return false;
}

static bool portal_driver_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                                  uint32_t xferred_bytes)
{
    (void)rhport;
    (void)result;
    (void)xferred_bytes;
    return ep_addr == s_portal_ep_in;
}

static usbd_class_driver_t make_portal_driver()
{
    usbd_class_driver_t driver = {};
    driver.name = "FUNKEY_PORTAL";
    driver.init = portal_driver_init;
    driver.deinit = portal_driver_deinit;
    driver.reset = portal_driver_reset;
    driver.open = portal_driver_open;
    driver.control_xfer_cb = portal_driver_control_xfer_cb;
    driver.xfer_cb = portal_driver_xfer_cb;
    driver.xfer_isr = nullptr;
    driver.sof = nullptr;
    return driver;
}

static const usbd_class_driver_t s_app_drivers[] = {
    make_portal_driver(),
};

extern "C" const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = sizeof(s_app_drivers) / sizeof(s_app_drivers[0]);
    return s_app_drivers;
}

esp_err_t funkey::usb::init()
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_descriptor;
    tusb_cfg.descriptor.string = s_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]);
    tusb_cfg.descriptor.full_speed_config = s_config_descriptor;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_config_descriptor;
#endif

    return tinyusb_driver_install(&tusb_cfg);
}

bool funkey::usb::sendPortalReport()
{
    if (!tud_mounted() || s_portal_ep_in == 0 || usbd_edpt_busy(0, s_portal_ep_in)) {
        return false;
    }

    funkey::portal::copyStablePacket(s_portal_report_xfer);

    if (usbd_edpt_busy(0, s_portal_ep_in) || !usbd_edpt_claim(0, s_portal_ep_in)) {
        return false;
    }

    if (!usbd_edpt_xfer(0, s_portal_ep_in, s_portal_report_xfer,
                        FUNKEY_PORTAL_PACKET_LEN)) {
        usbd_edpt_release(0, s_portal_ep_in);
        return false;
    }

    return true;
}
