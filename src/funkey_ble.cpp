#include "funkey_ble.hpp"

#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
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

#include "funkey_report.hpp"

// UUIDs:
// service  8a8f9f85-0d1c-4e54-9f54-1f2e2a94d839
// report   8a8f9f86-0d1c-4e54-9f54-1f2e2a94d839
// command  8a8f9f87-0d1c-4e54-9f54-1f2e2a94d839
// physical 8a8f9f88-0d1c-4e54-9f54-1f2e2a94d839
static const ble_uuid128_t s_ble_service_uuid =
    BLE_UUID128_INIT(0x39, 0xD8, 0x94, 0x2A, 0x2E, 0x1F, 0x54, 0x9F,
                     0x54, 0x4E, 0x1C, 0x0D, 0x85, 0x9F, 0x8F, 0x8A);
static const ble_uuid128_t s_ble_report_uuid =
    BLE_UUID128_INIT(0x39, 0xD8, 0x94, 0x2A, 0x2E, 0x1F, 0x54, 0x9F,
                     0x54, 0x4E, 0x1C, 0x0D, 0x86, 0x9F, 0x8F, 0x8A);
static const ble_uuid128_t s_ble_command_uuid =
    BLE_UUID128_INIT(0x39, 0xD8, 0x94, 0x2A, 0x2E, 0x1F, 0x54, 0x9F,
                     0x54, 0x4E, 0x1C, 0x0D, 0x87, 0x9F, 0x8F, 0x8A);
#if FUNKEY_PHYSICAL_ENABLED
static const ble_uuid128_t s_ble_physical_report_uuid =
    BLE_UUID128_INIT(0x39, 0xD8, 0x94, 0x2A, 0x2E, 0x1F, 0x54, 0x9F,
                     0x54, 0x4E, 0x1C, 0x0D, 0x88, 0x9F, 0x8F, 0x8A);
#endif

static uint8_t s_ble_own_addr_type;
static uint16_t s_ble_report_handle;
#if FUNKEY_PHYSICAL_ENABLED
static uint16_t s_ble_physical_report_handle;
#endif
static uint16_t s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_ble_report_notify_enabled = false;
#if FUNKEY_PHYSICAL_ENABLED
static bool s_ble_physical_report_notify_enabled = false;
#endif
static volatile bool s_ble_init_ok = false;
static volatile bool s_ble_synced = false;
static volatile bool s_ble_advertising = false;
static volatile int s_ble_last_error = 0;

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static int ble_command_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_report_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);
#if FUNKEY_PHYSICAL_ENABLED
static int ble_physical_report_access(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg);
#endif
static const char *ble_error_name(int rc);
static const char *ble_own_addr_type_name(uint8_t addr_type);
static void ble_log_error(const char *operation, int rc);
static void ble_report_notify(void);
#if FUNKEY_PHYSICAL_ENABLED
static void ble_physical_report_notify(void);
#endif
static void ble_start_advertising(void);

static ble_gatt_chr_def make_report_characteristic()
{
    ble_gatt_chr_def characteristic = {};
    characteristic.uuid = &s_ble_report_uuid.u;
    characteristic.access_cb = ble_report_access;
    characteristic.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
    characteristic.val_handle = &s_ble_report_handle;
    return characteristic;
}

#if FUNKEY_PHYSICAL_ENABLED
static ble_gatt_chr_def make_physical_report_characteristic()
{
    ble_gatt_chr_def characteristic = {};
    characteristic.uuid = &s_ble_physical_report_uuid.u;
    characteristic.access_cb = ble_physical_report_access;
    characteristic.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
    characteristic.val_handle = &s_ble_physical_report_handle;
    return characteristic;
}
#endif

static ble_gatt_chr_def make_command_characteristic()
{
    ble_gatt_chr_def characteristic = {};
    characteristic.uuid = &s_ble_command_uuid.u;
    characteristic.access_cb = ble_command_access;
    characteristic.flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
    return characteristic;
}

static ble_gatt_chr_def s_ble_characteristics[] = {
    make_report_characteristic(),
#if FUNKEY_PHYSICAL_ENABLED
    make_physical_report_characteristic(),
#endif
    make_command_characteristic(),
    {},
};

static ble_gatt_svc_def make_service()
{
    ble_gatt_svc_def service = {};
    service.type = BLE_GATT_SVC_TYPE_PRIMARY;
    service.uuid = &s_ble_service_uuid.u;
    service.characteristics = s_ble_characteristics;
    return service;
}

static const ble_gatt_svc_def s_ble_services[] = {
    make_service(),
    {},
};

static int ble_report_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR || attr_handle != s_ble_report_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t report[FUNKEY_REPORT_LEN];
    funkey::report::copy(report);
    return os_mbuf_append(ctxt->om, report, sizeof(report)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

#if FUNKEY_PHYSICAL_ENABLED
static int ble_physical_report_access(uint16_t conn_handle, uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR || attr_handle != s_ble_physical_report_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t report[FUNKEY_REPORT_LEN];
    funkey::physical_report::copy(report);
    return os_mbuf_append(ctxt->om, report, sizeof(report)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
#endif

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
        funkey::report::remove();
        ESP_LOGI(FUNKEY_LOG_TAG, "BLE remove");
        return 0;
    }

#if FUNKEY_PHYSICAL_ENABLED
    if (len == 1 && command[0] == FUNKEY_BLE_CMD_USE_PHYSICAL) {
        uint8_t report[FUNKEY_REPORT_LEN];
        funkey::physical_report::copy(report);
        if (funkey::physical_report::isContactIssue(report)) {
            ESP_LOGW(FUNKEY_LOG_TAG, "BLE use physical ignored: physical contact issue");
            return 0;
        }

        funkey::report::set(report);
        ESP_LOGI(FUNKEY_LOG_TAG, "BLE use physical");
        return 0;
    }
#endif

    if (len == 1 + FUNKEY_REPORT_LEN && command[0] == FUNKEY_BLE_CMD_SET_REPORT) {
        funkey::report::set(&command[1]);
        ESP_LOGI(FUNKEY_LOG_TAG, "BLE set %02X%02X%02X%02X%02X%02X%02X%02X",
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
    ESP_LOGE(FUNKEY_LOG_TAG, "%s failed: %d (%s)", operation, rc, ble_error_name(rc));
}

void funkey::ble::copyStatus(uint8_t out[FUNKEY_BLE_STATUS_LEN])
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
    funkey::report::copy(report);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om == nullptr) {
        return;
    }

    int rc = ble_gatts_notify_custom(s_ble_conn_handle, s_ble_report_handle, om);
    if (rc != 0) {
        ESP_LOGW(FUNKEY_LOG_TAG, "BLE notify failed: %d", rc);
    }
}

#if FUNKEY_PHYSICAL_ENABLED
static void ble_physical_report_notify(void)
{
    if (s_ble_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_ble_physical_report_notify_enabled ||
        s_ble_physical_report_handle == 0) {
        return;
    }

    uint8_t report[FUNKEY_REPORT_LEN];
    funkey::physical_report::copy(report);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, sizeof(report));
    if (om == nullptr) {
        return;
    }

    int rc = ble_gatts_notify_custom(s_ble_conn_handle, s_ble_physical_report_handle, om);
    if (rc != 0) {
        ESP_LOGW(FUNKEY_LOG_TAG, "BLE physical notify failed: %d", rc);
    }
}
#endif

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

    rc = ble_gap_adv_start(s_ble_own_addr_type, nullptr, BLE_HS_FOREVER, &adv_params,
                           ble_gap_event, nullptr);
    if (rc != 0) {
        ble_log_error("BLE advertising", rc);
        return;
    }

    s_ble_advertising = true;
    s_ble_last_error = 0;
    ESP_LOGI(FUNKEY_LOG_TAG, "BLE advertising as %s", name);
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ble_conn_handle = event->connect.conn_handle;
            s_ble_report_notify_enabled = false;
#if FUNKEY_PHYSICAL_ENABLED
            s_ble_physical_report_notify_enabled = false;
#endif
            s_ble_advertising = false;
            ESP_LOGI(FUNKEY_LOG_TAG, "BLE connected");
        } else {
            ESP_LOGW(FUNKEY_LOG_TAG, "BLE connect failed: %d", event->connect.status);
            ble_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(FUNKEY_LOG_TAG, "BLE disconnected: %d", event->disconnect.reason);
        s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_ble_report_notify_enabled = false;
#if FUNKEY_PHYSICAL_ENABLED
        s_ble_physical_report_notify_enabled = false;
#endif
        s_ble_advertising = false;
        ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ble_report_handle) {
            s_ble_report_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(FUNKEY_LOG_TAG, "BLE report notifications %s",
                     s_ble_report_notify_enabled ? "enabled" : "disabled");
        }
#if FUNKEY_PHYSICAL_ENABLED
        if (event->subscribe.attr_handle == s_ble_physical_report_handle) {
            s_ble_physical_report_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(FUNKEY_LOG_TAG, "BLE physical notifications %s",
                     s_ble_physical_report_notify_enabled ? "enabled" : "disabled");
        }
#endif
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
    ESP_LOGI(FUNKEY_LOG_TAG, "BLE synced with %s own address type (%u)",
             ble_own_addr_type_name(s_ble_own_addr_type), s_ble_own_addr_type);
    ble_start_advertising();
}

static void ble_on_reset(int reason)
{
    s_ble_synced = false;
    s_ble_advertising = false;
    ESP_LOGW(FUNKEY_LOG_TAG, "BLE reset: %d", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t funkey::ble::init()
{
#if FUNKEY_PHYSICAL_ENABLED
    funkey::report::setNotifyCallbacks(ble_report_notify, ble_physical_report_notify);
#else
    funkey::report::setNotifyCallbacks(ble_report_notify, nullptr);
#endif

    s_ble_init_ok = false;
    s_ble_synced = false;
    s_ble_advertising = false;
    s_ble_last_error = 0;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), FUNKEY_LOG_TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, FUNKEY_LOG_TAG, "NVS init failed");

    err = nimble_port_init();
    ESP_RETURN_ON_ERROR(err, FUNKEY_LOG_TAG, "NimBLE init failed");

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
