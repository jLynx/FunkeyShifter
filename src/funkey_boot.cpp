#include "funkey_boot.hpp"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

#include "funkey_protocol.hpp"

#if FUNKEY_FLASH_HOTKEY_ENABLED
static esp_err_t flash_hotkey_read_raw(uint16_t *out_raw)
{
    adc_unit_t unit_id;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel((int)FUNKEY_FLASH_HOTKEY_GPIO,
                                              &unit_id, &channel);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_unit_handle_t unit = nullptr;
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = unit_id;
    init_config.ulp_mode = ADC_ULP_MODE_DISABLE;
    err = adc_oneshot_new_unit(&init_config, &unit);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t channel_config = {};
    channel_config.atten = FUNKEY_FLASH_HOTKEY_ADC_ATTEN;
    channel_config.bitwidth = ADC_BITWIDTH_12;
    err = adc_oneshot_config_channel(unit, channel, &channel_config);
    if (err != ESP_OK) {
        (void)adc_oneshot_del_unit(unit);
        return err;
    }

    (void)gpio_set_pull_mode(FUNKEY_FLASH_HOTKEY_GPIO, GPIO_FLOATING);

    uint32_t sample_count = FUNKEY_FLASH_HOTKEY_ADC_SAMPLE_COUNT;
    if (sample_count == 0) {
        sample_count = 1;
    }

    uint32_t total = 0;
    for (uint32_t sample = 0; sample < sample_count; ++sample) {
        int raw = 0;
        err = adc_oneshot_read(unit, channel, &raw);
        if (err != ESP_OK) {
            (void)adc_oneshot_del_unit(unit);
            return err;
        }
        total += (uint32_t)raw;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    (void)adc_oneshot_del_unit(unit);
    *out_raw = (uint16_t)((total + (sample_count / 2U)) / sample_count);
    return ESP_OK;
}

static bool flash_hotkey_is_held(void)
{
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << (uint32_t)FUNKEY_FLASH_HOTKEY_GPIO);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGW(FUNKEY_LOG_TAG, "Flash hotkey GPIO%d config failed: %s",
                 (int)FUNKEY_FLASH_HOTKEY_GPIO, esp_err_to_name(err));
        return false;
    }

    uint32_t sample_ms = FUNKEY_FLASH_HOTKEY_SAMPLE_MS;
    if (sample_ms == 0) {
        sample_ms = 1;
    }

    TickType_t sample_ticks = pdMS_TO_TICKS(sample_ms);
    if (sample_ticks == 0) {
        sample_ticks = 1;
    }

    for (uint32_t elapsed_ms = 0; elapsed_ms < FUNKEY_FLASH_HOTKEY_HOLD_MS;
         elapsed_ms += sample_ms) {
        if (gpio_get_level(FUNKEY_FLASH_HOTKEY_GPIO) != 0) {
            (void)gpio_set_pull_mode(FUNKEY_FLASH_HOTKEY_GPIO, GPIO_FLOATING);
            return false;
        }
        vTaskDelay(sample_ticks);
    }

    uint16_t raw = 0;
    err = flash_hotkey_read_raw(&raw);
    if (err != ESP_OK) {
        ESP_LOGW(FUNKEY_LOG_TAG, "Flash hotkey GPIO%d ADC read failed: %s",
                 (int)FUNKEY_FLASH_HOTKEY_GPIO, esp_err_to_name(err));
        return false;
    }

    if (raw > FUNKEY_FLASH_HOTKEY_ADC_MAX_RAW) {
        ESP_LOGI(FUNKEY_LOG_TAG,
                 "Flash hotkey GPIO%d held low, but ADC raw %u is not a hard short",
                 (int)FUNKEY_FLASH_HOTKEY_GPIO, raw);
        return false;
    }

    return true;
}
#endif

void funkey::boot::maybeEnterDownloadMode()
{
#if FUNKEY_FLASH_HOTKEY_ENABLED
    if (!flash_hotkey_is_held()) {
        return;
    }

    ESP_LOGW(FUNKEY_LOG_TAG,
             "GPIO%d held low at boot; restarting into ESP32-S3 ROM download mode",
             (int)FUNKEY_FLASH_HOTKEY_GPIO);
    vTaskDelay(pdMS_TO_TICKS(100));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
#endif
}
