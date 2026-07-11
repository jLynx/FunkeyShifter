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
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "tusb_config.h"

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
#define FUNKEY_REQ_GET_BLE_STATUS 0x05

#define FUNKEY_BLE_NAME "Funkey Shifter"
#define FUNKEY_BLE_SHORT_NAME "Funkey"
#define FUNKEY_BLE_CMD_SET_REPORT 0x02
#define FUNKEY_BLE_CMD_REMOVE 0x03
#define FUNKEY_BLE_STATUS_LEN 8

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

// UUIDs:
// service 8a8f9f85-0d1c-4e54-9f54-1f2e2a94d839
// report  8a8f9f86-0d1c-4e54-9f54-1f2e2a94d839
// command 8a8f9f87-0d1c-4e54-9f54-1f2e2a94d839
static const ble_uuid128_t s_ble_service_uuid =
    BLE_UUID128_INIT(0x39, 0xD8, 0x94, 0x2A, 0x2E, 0x1F, 0x54, 0x9F,
                     0x54, 0x4E, 0x1C, 0x0D, 0x85, 0x9F, 0x8F, 0x8A);
static const ble_uuid128_t s_ble_report_uuid =
    BLE_UUID128_INIT(0x39, 0xD8, 0x94, 0x2A, 0x2E, 0x1F, 0x54, 0x9F,
                     0x54, 0x4E, 0x1C, 0x0D, 0x86, 0x9F, 0x8F, 0x8A);
static const ble_uuid128_t s_ble_command_uuid =
    BLE_UUID128_INIT(0x39, 0xD8, 0x94, 0x2A, 0x2E, 0x1F, 0x54, 0x9F,
                     0x54, 0x4E, 0x1C, 0x0D, 0x87, 0x9F, 0x8F, 0x8A);

static uint8_t s_ble_own_addr_type;
static uint16_t s_ble_report_handle;
static uint16_t s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_ble_report_notify_enabled = false;
static volatile bool s_ble_init_ok = false;
static volatile bool s_ble_synced = false;
static volatile bool s_ble_advertising = false;
static volatile int s_ble_last_error = 0;

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static int ble_command_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_report_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);
static const char *ble_error_name(int rc);
static const char *ble_own_addr_type_name(uint8_t addr_type);
static void ble_log_error(const char *operation, int rc);
static void ble_status_copy(uint8_t out[FUNKEY_BLE_STATUS_LEN]);
static void ble_report_notify(void);
static void ble_start_advertising(void);

static const struct ble_gatt_svc_def s_ble_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_ble_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_ble_report_uuid.u,
                .access_cb = ble_report_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ble_report_handle,
            },
            {
                .uuid = &s_ble_command_uuid.u,
                .access_cb = ble_command_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0},
        },
    },
    {0},
};

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
    ble_report_notify();
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

static int ble_report_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR || attr_handle != s_ble_report_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t report[FUNKEY_REPORT_LEN];
    report_copy(report);
    return os_mbuf_append(ctxt->om, report, sizeof(report)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_command_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t command[1 + FUNKEY_REPORT_LEN];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(command)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    int rc = os_mbuf_copydata(ctxt->om, 0, len, command);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (len == 1 && command[0] == FUNKEY_BLE_CMD_REMOVE) {
        report_remove();
        ESP_LOGI(TAG, "BLE remove");
        return 0;
    }

    if (len == 1 + FUNKEY_REPORT_LEN && command[0] == FUNKEY_BLE_CMD_SET_REPORT) {
        report_set(&command[1]);
        ESP_LOGI(TAG, "BLE set %02X%02X%02X%02X%02X%02X%02X%02X",
                 command[1], command[2], command[3], command[4],
                 command[5], command[6], command[7], command[8]);
        return 0;
    }

    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
}

static const char *ble_error_name(int rc)
{
    switch (rc) {
    case 0:
        return "ok";
    case BLE_HS_EAGAIN:
        return "try again";
    case BLE_HS_EALREADY:
        return "already in requested state";
    case BLE_HS_EINVAL:
        return "invalid argument";
    case BLE_HS_EMSGSIZE:
        return "advertising data too large";
    case BLE_HS_ENOMEM:
        return "out of memory";
    case BLE_HS_EBUSY:
        return "busy";
    case BLE_HS_ENOADDR:
        return "no BLE address";
    case BLE_HS_ENOTSYNCED:
        return "host not synced";
    case BLE_HS_EDISABLED:
        return "disabled";
    case BLE_HS_ERR_HCI_BASE + 0x07:
        return "hci memory capacity exceeded";
    case BLE_HS_ERR_HCI_BASE + 0x0C:
        return "hci command disallowed";
    case BLE_HS_ERR_HCI_BASE + 0x11:
        return "hci unsupported feature or parameter";
    case BLE_HS_ERR_HCI_BASE + 0x12:
        return "hci invalid command parameters";
    case BLE_HS_ERR_HCI_BASE + 0x3A:
        return "hci controller busy";
    default:
        return "unknown";
    }
}

static const char *ble_own_addr_type_name(uint8_t addr_type)
{
    switch (addr_type) {
    case BLE_OWN_ADDR_PUBLIC:
        return "public";
    case BLE_OWN_ADDR_RANDOM:
        return "random";
    case BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT:
        return "rpa-public-default";
    case BLE_OWN_ADDR_RPA_RANDOM_DEFAULT:
        return "rpa-random-default";
    default:
        return "unknown";
    }
}

static void ble_log_error(const char *operation, int rc)
{
    s_ble_last_error = rc;
    ESP_LOGE(TAG, "%s failed: %d (%s)", operation, rc, ble_error_name(rc));
}

static void ble_status_copy(uint8_t out[FUNKEY_BLE_STATUS_LEN])
{
    uint8_t flags = 0;
    uint16_t last_error = (uint16_t)s_ble_last_error;

    if (s_ble_init_ok) {
        flags |= 0x01;
    }
    if (s_ble_synced) {
        flags |= 0x02;
    }
    if (s_ble_advertising) {
        flags |= 0x04;
    }
    if (s_ble_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        flags |= 0x08;
    }
    if (s_ble_report_notify_enabled) {
        flags |= 0x10;
    }

    out[0] = 'B';
    out[1] = 1;
    out[2] = flags;
    out[3] = s_ble_own_addr_type;
    out[4] = last_error & 0xFF;
    out[5] = last_error >> 8;
    out[6] = s_ble_report_handle & 0xFF;
    out[7] = s_ble_report_handle >> 8;
}

static void ble_report_notify(void)
{
    if (s_ble_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_ble_report_notify_enabled ||
        s_ble_report_handle == 0) {
        return;
    }

    uint8_t report[FUNKEY_REPORT_LEN];
    report_copy(report);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om == NULL) {
        return;
    }

    int rc = ble_gatts_notify_custom(s_ble_conn_handle, s_ble_report_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "BLE notify failed: %d", rc);
    }
}

static void ble_start_advertising(void)
{
    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields adv_fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    struct ble_gap_adv_params adv_params = {0};

    s_ble_advertising = false;
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.uuids128 = &s_ble_service_uuid;
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;
    adv_fields.name = (const uint8_t *)FUNKEY_BLE_SHORT_NAME;
    adv_fields.name_len = strlen(FUNKEY_BLE_SHORT_NAME);
    adv_fields.name_is_complete = 0;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ble_log_error("BLE adv fields", rc);
        return;
    }

    rsp_fields.name = (const uint8_t *)name;
    rsp_fields.name_len = strlen(name);
    rsp_fields.name_is_complete = 1;
    rsp_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    rsp_fields.tx_pwr_lvl_is_present = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ble_log_error("BLE scan response", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(120);

    rc = ble_gap_adv_start(s_ble_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ble_log_error("BLE advertising", rc);
        return;
    }

    s_ble_advertising = true;
    s_ble_last_error = 0;
    ESP_LOGI(TAG, "BLE advertising as %s", name);
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ble_conn_handle = event->connect.conn_handle;
            s_ble_report_notify_enabled = false;
            s_ble_advertising = false;
            ESP_LOGI(TAG, "BLE connected");
        } else {
            ESP_LOGW(TAG, "BLE connect failed: %d", event->connect.status);
            ble_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected: %d", event->disconnect.reason);
        s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_ble_report_notify_enabled = false;
        s_ble_advertising = false;
        ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ble_report_handle) {
            s_ble_report_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "BLE report notifications %s",
                     s_ble_report_notify_enabled ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_ble_advertising = false;
        ble_start_advertising();
        return 0;

    default:
        return 0;
    }
}

static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ble_log_error("BLE address setup", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_ble_own_addr_type);
    if (rc != 0) {
        ble_log_error("BLE address inference", rc);
        return;
    }

    s_ble_synced = true;
    ESP_LOGI(TAG, "BLE synced with %s own address type (%u)",
             ble_own_addr_type_name(s_ble_own_addr_type), s_ble_own_addr_type);
    ble_start_advertising();
}

static void ble_on_reset(int reason)
{
    s_ble_synced = false;
    s_ble_advertising = false;
    ESP_LOGW(TAG, "BLE reset: %d", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_init(void)
{
    s_ble_init_ok = false;
    s_ble_synced = false;
    s_ble_advertising = false;
    s_ble_last_error = 0;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");

    err = nimble_port_init();
    ESP_RETURN_ON_ERROR(err, TAG, "NimBLE init failed");

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set(FUNKEY_BLE_NAME);
    if (rc != 0) {
        ble_log_error("BLE name", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_count_cfg(s_ble_services);
    if (rc != 0) {
        ble_log_error("BLE count services", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_ble_services);
    if (rc != 0) {
        ble_log_error("BLE add services", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
    s_ble_init_ok = true;
    return ESP_OK;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    static const uint8_t init_response[4] = {0x00, 0x00, 0x00, 0x00};
    static uint8_t get_report_buf[FUNKEY_REPORT_LEN];
    static uint8_t ble_status_buf[FUNKEY_BLE_STATUS_LEN];

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

    case FUNKEY_REQ_GET_BLE_STATUS:
        if (request->bmRequestType_bit.direction != TUSB_DIR_IN || request->wLength == 0) {
            return false;
        }
        ble_status_copy(ble_status_buf);
        return tud_control_xfer(rhport, request, ble_status_buf,
                                request->wLength < FUNKEY_BLE_STATUS_LEN ? request->wLength : FUNKEY_BLE_STATUS_LEN);

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
    static uint8_t ble_status_buf[FUNKEY_BLE_STATUS_LEN];

    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bRequest == FUNKEY_REQ_INIT &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->wLength > 0) {
        uint16_t len = request->wLength < sizeof(init_response) ? request->wLength : sizeof(init_response);
        return tud_control_xfer(rhport, request, (void *)init_response, len);
    }

    if (request->bRequest == FUNKEY_REQ_GET_BLE_STATUS &&
        request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->wLength > 0) {
        uint16_t len = request->wLength < FUNKEY_BLE_STATUS_LEN ? request->wLength : FUNKEY_BLE_STATUS_LEN;
        ble_status_copy(ble_status_buf);
        return tud_control_xfer(rhport, request, ble_status_buf, len);
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
    ESP_LOGI(TAG, "Starting Funkey Shifter Portal device");
    ESP_LOGI(TAG, "VID:PID %04X:%04X", FUNKEY_USB_VID, FUNKEY_USB_PID);

    esp_err_t err = ble_init();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "BLE initialized");
    } else {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_descriptor;
    tusb_cfg.descriptor.string = s_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]);
    tusb_cfg.descriptor.full_speed_config = s_config_descriptor;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_config_descriptor;
#endif

    err = tinyusb_driver_install(&tusb_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "USB initialized");
    } else {
        ESP_LOGE(TAG, "USB init failed: %s", esp_err_to_name(err));
    }

    while (true) {
        if (send_portal_report()) {
            portENTER_CRITICAL(&s_report_mux);
            s_report_dirty = false;
            portEXIT_CRITICAL(&s_report_mux);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
