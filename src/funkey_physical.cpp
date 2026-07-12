#include "funkey_physical.hpp"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "funkey_protocol.hpp"
#include "funkey_report.hpp"

#define FUNKEY_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#if FUNKEY_PHYSICAL_ENABLED
static const uint32_t s_funkey_digit_resistor_ohms[10] = {
    1020, 18300, 39300, 68300, 114000, 164000, 244000, 364000, 564000, 914000,
};

typedef enum {
    PHYSICAL_STATE_INVALID = 0,
    PHYSICAL_STATE_REMOVED,
    PHYSICAL_STATE_CONTACT_ISSUE,
    PHYSICAL_STATE_PRESENT,
} physical_state_type_t;

typedef struct {
    const char *label;
    gpio_num_t gpio;
    adc_unit_t unit;
    adc_channel_t channel;
} physical_adc_input_t;

typedef struct {
    physical_state_type_t type;
    uint32_t id;
    uint8_t digits[4];
    uint16_t raw[4];
} physical_state_t;

static const physical_adc_input_t s_physical_adc_inputs[4] = {
    {"pad2/R4/checksum", FUNKEY_PHYSICAL_PAD2_R4_GPIO,
     FUNKEY_PHYSICAL_PAD2_R4_ADC_UNIT, FUNKEY_PHYSICAL_PAD2_R4_ADC_CHANNEL},
    {"pad3/R1/ones", FUNKEY_PHYSICAL_PAD3_R1_GPIO,
     FUNKEY_PHYSICAL_PAD3_R1_ADC_UNIT, FUNKEY_PHYSICAL_PAD3_R1_ADC_CHANNEL},
    {"pad4/R2/tens", FUNKEY_PHYSICAL_PAD4_R2_GPIO,
     FUNKEY_PHYSICAL_PAD4_R2_ADC_UNIT, FUNKEY_PHYSICAL_PAD4_R2_ADC_CHANNEL},
    {"pad5/R3/hundreds", FUNKEY_PHYSICAL_PAD5_R3_GPIO,
     FUNKEY_PHYSICAL_PAD5_R3_ADC_UNIT, FUNKEY_PHYSICAL_PAD5_R3_ADC_CHANNEL},
};

static adc_oneshot_unit_handle_t s_physical_adc1_unit;
static adc_oneshot_unit_handle_t s_physical_adc2_unit;
static bool s_physical_adc_ready = false;
static uint16_t s_physical_digit_raw[10];
static uint16_t s_physical_no_figure_threshold_raw;
static physical_state_t s_physical_candidate = {PHYSICAL_STATE_INVALID, 0, {0}, {0}};
static physical_state_t s_physical_observed = {PHYSICAL_STATE_INVALID, 0, {0}, {0}};
static uint8_t s_physical_stable_count = 0;

static esp_err_t physical_config_input_pin(const physical_adc_input_t *input);

static uint16_t physical_raw_for_resistor(uint32_t resistor_ohms)
{
    uint32_t denominator = resistor_ohms + FUNKEY_PHYSICAL_PULLUP_OHMS;
    uint64_t numerator = (uint64_t)FUNKEY_PHYSICAL_ADC_FULL_SCALE_RAW * resistor_ohms;
    uint32_t ideal_raw = (uint32_t)((numerator + denominator / 2) / denominator);
    return (uint16_t)(((uint64_t)ideal_raw * FUNKEY_PHYSICAL_ADC_TARGET_SCALE_NUM +
                       (FUNKEY_PHYSICAL_ADC_TARGET_SCALE_DEN / 2U)) /
                      FUNKEY_PHYSICAL_ADC_TARGET_SCALE_DEN);
}

static uint16_t physical_raw_delta(uint16_t left, uint16_t right)
{
    return left > right ? left - right : right - left;
}

static adc_oneshot_unit_handle_t physical_adc_handle(adc_unit_t unit)
{
    return unit == ADC_UNIT_1 ? s_physical_adc1_unit : s_physical_adc2_unit;
}

static bool physical_state_equal(const physical_state_t *left, const physical_state_t *right)
{
    return left->type == right->type &&
           (left->type != PHYSICAL_STATE_PRESENT || left->id == right->id);
}

static int physical_digit_from_raw(uint16_t raw)
{
    int best_digit = 0;
    uint16_t best_delta = physical_raw_delta(raw, s_physical_digit_raw[0]);

    for (int digit = 1; digit < 10; ++digit) {
        uint16_t delta = physical_raw_delta(raw, s_physical_digit_raw[digit]);
        if (delta < best_delta) {
            best_digit = digit;
            best_delta = delta;
        }
    }

    if (best_delta > FUNKEY_PHYSICAL_MAX_BUCKET_DELTA_RAW) {
        return -1;
    }

    return best_digit;
}

static esp_err_t physical_settle_adc_channel(const physical_adc_input_t *input,
                                             adc_oneshot_unit_handle_t unit)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(FUNKEY_PHYSICAL_ADC_SETTLE_TIMEOUT_MS);
    uint16_t last_raw = 0;
    uint32_t stable_reads = 0;
    bool have_last = false;

    while (true) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(unit, input->channel, &raw);
        if (err != ESP_OK) {
            return err;
        }

        uint16_t current_raw = (uint16_t)raw;
        if (have_last &&
            physical_raw_delta(current_raw, last_raw) <= FUNKEY_PHYSICAL_ADC_SETTLE_DELTA_RAW) {
            ++stable_reads;
            if (stable_reads >= FUNKEY_PHYSICAL_ADC_SETTLE_READS) {
                return ESP_OK;
            }
        } else {
            stable_reads = 1;
        }

        have_last = true;
        last_raw = current_raw;

        if ((xTaskGetTickCount() - start) >= timeout) {
            ESP_LOGW(FUNKEY_LOG_TAG,
                     "Physical ADC did not settle on %s GPIO%d within %u ms; last raw=%u",
                     input->label,
                     input->gpio,
                     (unsigned)FUNKEY_PHYSICAL_ADC_SETTLE_TIMEOUT_MS,
                     last_raw);
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(FUNKEY_PHYSICAL_ADC_SETTLE_DELAY_MS));
    }
}

static esp_err_t physical_read_average(const physical_adc_input_t *input, uint16_t *out_raw)
{
    adc_oneshot_unit_handle_t unit = physical_adc_handle(input->unit);
    if (unit == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t total = 0;
    esp_err_t err = physical_config_input_pin(input);
    if (err != ESP_OK) {
        return err;
    }

    err = physical_settle_adc_channel(input, unit);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t sample = 0; sample < FUNKEY_PHYSICAL_SAMPLE_COUNT; ++sample) {
        int raw = 0;
        err = adc_oneshot_read(unit, input->channel, &raw);
        if (err != ESP_OK) {
            return err;
        }
        total += (uint32_t)raw;
        vTaskDelay(pdMS_TO_TICKS(FUNKEY_PHYSICAL_SAMPLE_DELAY_MS));
    }

    *out_raw = (uint16_t)((total + FUNKEY_PHYSICAL_SAMPLE_COUNT / 2) /
                          FUNKEY_PHYSICAL_SAMPLE_COUNT);
    return ESP_OK;
}

static bool physical_decode_once(physical_state_t *state)
{
    bool all_open = true;

    memset(state, 0, sizeof(*state));
    state->type = PHYSICAL_STATE_INVALID;

    for (size_t index = 0; index < FUNKEY_ARRAY_SIZE(s_physical_adc_inputs); ++index) {
        esp_err_t err = physical_read_average(&s_physical_adc_inputs[index], &state->raw[index]);
        if (err != ESP_OK) {
            ESP_LOGW(FUNKEY_LOG_TAG, "Physical ADC read failed on %s: %s",
                     s_physical_adc_inputs[index].label, esp_err_to_name(err));
            state->type = PHYSICAL_STATE_CONTACT_ISSUE;
            return true;
        }

        if (state->raw[index] < s_physical_no_figure_threshold_raw) {
            all_open = false;
        }
    }

    if (all_open) {
        state->type = PHYSICAL_STATE_REMOVED;
        return true;
    }

    for (size_t index = 0; index < FUNKEY_ARRAY_SIZE(s_physical_adc_inputs); ++index) {
        if (state->raw[index] >= s_physical_no_figure_threshold_raw) {
            state->type = PHYSICAL_STATE_CONTACT_ISSUE;
            return true;
        }
    }

    int checksum = physical_digit_from_raw(state->raw[0]);
    int ones = physical_digit_from_raw(state->raw[1]);
    int tens = physical_digit_from_raw(state->raw[2]);
    int hundreds = physical_digit_from_raw(state->raw[3]);

    if (checksum < 0 || ones < 0 || tens < 0 || hundreds < 0) {
        state->type = PHYSICAL_STATE_CONTACT_ISSUE;
        return true;
    }

    int expected_checksum = (ones + tens + hundreds) % 10;
    if (checksum != expected_checksum) {
        state->type = PHYSICAL_STATE_CONTACT_ISSUE;
        return true;
    }

    state->type = PHYSICAL_STATE_PRESENT;
    state->digits[0] = (uint8_t)ones;
    state->digits[1] = (uint8_t)tens;
    state->digits[2] = (uint8_t)hundreds;
    state->digits[3] = (uint8_t)checksum;
    state->id = (uint32_t)(hundreds * 100 + tens * 10 + ones);
    return true;
}

static void physical_apply_observed_state(const physical_state_t *state)
{
    if (s_physical_observed.type == PHYSICAL_STATE_INVALID &&
        state->type == PHYSICAL_STATE_REMOVED) {
        s_physical_observed = *state;
        funkey::physical_report::remove();
        ESP_LOGI(FUNKEY_LOG_TAG, "Physical reader idle: no Funkey present");
        return;
    }

    s_physical_observed = *state;

    if (state->type == PHYSICAL_STATE_REMOVED) {
        ESP_LOGI(FUNKEY_LOG_TAG, "Physical Funkey removed");
        funkey::physical_report::remove();
        funkey::report::remove();
        return;
    }

    if (state->type == PHYSICAL_STATE_CONTACT_ISSUE) {
        ESP_LOGW(FUNKEY_LOG_TAG,
                 "Physical reader contact issue raw %u,%u,%u,%u",
                 state->raw[0], state->raw[1], state->raw[2], state->raw[3]);
        funkey::physical_report::contactIssue();
        return;
    }

    if (state->type == PHYSICAL_STATE_PRESENT) {
        ESP_LOGI(FUNKEY_LOG_TAG,
                 "Physical Funkey ID %" PRIu32 " (%08" PRIX32
                 ") digits %u,%u,%u,%u raw %u,%u,%u,%u",
                 state->id, state->id,
                 state->digits[0], state->digits[1], state->digits[2], state->digits[3],
                 state->raw[0], state->raw[1], state->raw[2], state->raw[3]);
        funkey::physical_report::setId(state->id);
        funkey::report::setId(state->id);
    }
}

void funkey::physical::pollReader()
{
    if (!s_physical_adc_ready) {
        return;
    }

    physical_state_t state;
    if (!physical_decode_once(&state)) {
        s_physical_candidate.type = PHYSICAL_STATE_INVALID;
        s_physical_stable_count = 0;
        return;
    }

    if (!physical_state_equal(&state, &s_physical_candidate)) {
        s_physical_candidate = state;
        s_physical_stable_count = 1;
        return;
    }

    if (s_physical_stable_count < FUNKEY_PHYSICAL_STABLE_POLLS) {
        ++s_physical_stable_count;
    }

    if (s_physical_stable_count >= FUNKEY_PHYSICAL_STABLE_POLLS &&
        !physical_state_equal(&state, &s_physical_observed)) {
        physical_apply_observed_state(&state);
    }
}

static esp_err_t physical_adc_init_unit(adc_unit_t unit, adc_oneshot_unit_handle_t *out_handle)
{
    if (*out_handle != nullptr) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = unit;
    init_config.ulp_mode = ADC_ULP_MODE_DISABLE;
    esp_err_t err = adc_oneshot_new_unit(&init_config, out_handle);
    if (err != ESP_OK) {
        ESP_LOGE(FUNKEY_LOG_TAG, "Physical ADC unit %d init failed: %s",
                 unit, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t physical_config_input_pin(const physical_adc_input_t *input)
{
    esp_err_t err = gpio_set_direction(input->gpio, GPIO_MODE_INPUT);
    if (err != ESP_OK) {
        return err;
    }

#if FUNKEY_PHYSICAL_USE_INTERNAL_PULLUPS
    err = gpio_set_pull_mode(input->gpio, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) {
        return err;
    }

    if (rtc_gpio_is_valid_gpio(input->gpio)) {
        err = rtc_gpio_pulldown_dis(input->gpio);
        if (err != ESP_OK) {
            return err;
        }

        err = rtc_gpio_pullup_en(input->gpio);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
#else
    err = gpio_set_pull_mode(input->gpio, GPIO_FLOATING);
    if (err != ESP_OK) {
        return err;
    }

    if (rtc_gpio_is_valid_gpio(input->gpio)) {
        err = rtc_gpio_pulldown_dis(input->gpio);
        if (err != ESP_OK) {
            return err;
        }

        err = rtc_gpio_pullup_dis(input->gpio);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
#endif
}

esp_err_t funkey::physical::initReader()
{
    adc_oneshot_chan_cfg_t channel_config = {};
    channel_config.atten = FUNKEY_PHYSICAL_ADC_ATTEN;
    channel_config.bitwidth = ADC_BITWIDTH_12;

    for (size_t index = 0; index < FUNKEY_ARRAY_SIZE(s_physical_adc_inputs); ++index) {
        const physical_adc_input_t *input = &s_physical_adc_inputs[index];
        adc_oneshot_unit_handle_t *unit_handle =
            input->unit == ADC_UNIT_1 ? &s_physical_adc1_unit : &s_physical_adc2_unit;

        esp_err_t err = physical_adc_init_unit(input->unit, unit_handle);
        if (err != ESP_OK) {
            return err;
        }

        err = adc_oneshot_config_channel(*unit_handle, input->channel, &channel_config);
        if (err != ESP_OK) {
            ESP_LOGE(FUNKEY_LOG_TAG, "Physical ADC channel init failed on %s unit %d channel %d: %s",
                     input->label, input->unit, input->channel, esp_err_to_name(err));
            return err;
        }

        err = physical_config_input_pin(input);
        if (err != ESP_OK) {
            ESP_LOGE(FUNKEY_LOG_TAG, "Physical GPIO config failed on %s GPIO%d: %s",
                     input->label, input->gpio, esp_err_to_name(err));
            return err;
        }
    }

    for (int digit = 0; digit < 10; ++digit) {
        s_physical_digit_raw[digit] = physical_raw_for_resistor(s_funkey_digit_resistor_ohms[digit]);
    }
    s_physical_no_figure_threshold_raw =
        s_physical_digit_raw[9] +
        (uint16_t)((FUNKEY_PHYSICAL_ADC_FULL_SCALE_RAW - s_physical_digit_raw[9]) / 2);

    s_physical_adc_ready = true;
    ESP_LOGI(FUNKEY_LOG_TAG,
             "Physical reader enabled: %s pull-up %u ohms, ADC target scale %u/%u, open threshold raw %u, GPIO/unit/channel R4=%d/%d/%d R1=%d/%d/%d R2=%d/%d/%d R3=%d/%d/%d",
#if FUNKEY_PHYSICAL_USE_INTERNAL_PULLUPS
             "internal",
#else
             "external",
#endif
             FUNKEY_PHYSICAL_PULLUP_OHMS,
             FUNKEY_PHYSICAL_ADC_TARGET_SCALE_NUM,
             FUNKEY_PHYSICAL_ADC_TARGET_SCALE_DEN,
             s_physical_no_figure_threshold_raw,
             FUNKEY_PHYSICAL_PAD2_R4_GPIO,
             FUNKEY_PHYSICAL_PAD2_R4_ADC_UNIT,
             FUNKEY_PHYSICAL_PAD2_R4_ADC_CHANNEL,
             FUNKEY_PHYSICAL_PAD3_R1_GPIO,
             FUNKEY_PHYSICAL_PAD3_R1_ADC_UNIT,
             FUNKEY_PHYSICAL_PAD3_R1_ADC_CHANNEL,
             FUNKEY_PHYSICAL_PAD4_R2_GPIO,
             FUNKEY_PHYSICAL_PAD4_R2_ADC_UNIT,
             FUNKEY_PHYSICAL_PAD4_R2_ADC_CHANNEL,
             FUNKEY_PHYSICAL_PAD5_R3_GPIO,
             FUNKEY_PHYSICAL_PAD5_R3_ADC_UNIT,
             FUNKEY_PHYSICAL_PAD5_R3_ADC_CHANNEL);
    return ESP_OK;
}
#else
esp_err_t funkey::physical::initReader()
{
    return ESP_ERR_NOT_SUPPORTED;
}

void funkey::physical::pollReader()
{
}
#endif
