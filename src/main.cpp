#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "funkey_ble.hpp"
#include "funkey_boot.hpp"
#include "funkey_physical.hpp"
#include "funkey_protocol.hpp"
#include "funkey_usb.hpp"

extern "C" void app_main(void)
{
    funkey::boot::maybeEnterDownloadMode();

    ESP_LOGI(FUNKEY_LOG_TAG, "Starting Funkey Shifter Portal device");
    ESP_LOGI(FUNKEY_LOG_TAG, "VID:PID %04X:%04X", FUNKEY_USB_VID, FUNKEY_USB_PID);

    esp_err_t err = funkey::ble::init();
    if (err == ESP_OK) {
        ESP_LOGI(FUNKEY_LOG_TAG, "BLE initialized");
    } else {
        ESP_LOGE(FUNKEY_LOG_TAG, "BLE init failed: %s", esp_err_to_name(err));
    }

    err = funkey::usb::init();
    if (err == ESP_OK) {
        ESP_LOGI(FUNKEY_LOG_TAG, "USB initialized");
    } else {
        ESP_LOGE(FUNKEY_LOG_TAG, "USB init failed: %s", esp_err_to_name(err));
    }

#if FUNKEY_PHYSICAL_ENABLED
    err = funkey::physical::initReader();
    if (err == ESP_OK) {
        ESP_LOGI(FUNKEY_LOG_TAG, "Physical reader initialized");
    }

    TickType_t last_physical_poll = 0;
#endif

    while (true) {
        (void)funkey::usb::sendPortalReport();

#if FUNKEY_PHYSICAL_ENABLED
        TickType_t now = xTaskGetTickCount();
        if (now - last_physical_poll >= pdMS_TO_TICKS(FUNKEY_PHYSICAL_POLL_MS)) {
            funkey::physical::pollReader();
            last_physical_poll = now;
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
