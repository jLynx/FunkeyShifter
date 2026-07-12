#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FUNKEY_TEST_PULLUP_OHMS 220000U
#define FUNKEY_TEST_ADC_FULL_SCALE 4095U
#define FUNKEY_TEST_ADC_TARGET_SCALE_NUM 900U
#define FUNKEY_TEST_ADC_TARGET_SCALE_DEN 1000U
#define FUNKEY_TEST_SAMPLE_COUNT 32U
#define FUNKEY_TEST_STABLE_READS 3U
#define FUNKEY_TEST_ADC_SETTLE_DELTA_RAW 4U
#define FUNKEY_TEST_ADC_SETTLE_READS 4U
#define FUNKEY_TEST_ADC_SETTLE_TIMEOUT_MS 750U
#define FUNKEY_TEST_ADC_SETTLE_DELAY_MS 5U
#define FUNKEY_TEST_SAMPLE_DELAY_MS 2U

static const uint32_t s_digit_resistor_ohms[10] = {
    1020U,
    18300U,
    39300U,
    68300U,
    114000U,
    164000U,
    244000U,
    364000U,
    564000U,
    914000U,
};

typedef struct {
    const char *label;
    gpio_num_t gpio;
    adc_unit_t unit;
    adc_channel_t channel;
} funkey_input_t;

static const char *TAG = "funkey-test";
static adc_oneshot_unit_handle_t s_adc1;
static adc_oneshot_unit_handle_t s_adc2;

// Board bottom pads, left to right: R3 R2 R1 R4 GND.
static const funkey_input_t s_inputs[] = {
    {"R3", GPIO_NUM_11, ADC_UNIT_2, ADC_CHANNEL_0},
    {"R2", GPIO_NUM_10, ADC_UNIT_1, ADC_CHANNEL_9},
    {"R1", GPIO_NUM_9, ADC_UNIT_1, ADC_CHANNEL_8},
    {"R4", GPIO_NUM_8, ADC_UNIT_1, ADC_CHANNEL_7},
};

static adc_oneshot_unit_handle_t adc_handle_for(adc_unit_t unit)
{
    return unit == ADC_UNIT_1 ? s_adc1 : s_adc2;
}

static esp_err_t configure_external_pullup_input(const funkey_input_t *input)
{
    esp_err_t err = gpio_set_direction(input->gpio, GPIO_MODE_INPUT);
    if (err != ESP_OK) {
        return err;
    }

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
}

static uint32_t ideal_raw_for_resistance(uint32_t resistor_ohms)
{
    return (uint32_t)(((uint64_t)FUNKEY_TEST_ADC_FULL_SCALE * resistor_ohms) /
                      (FUNKEY_TEST_PULLUP_OHMS + resistor_ohms));
}

static uint32_t raw_for_resistance(uint32_t resistor_ohms)
{
    uint32_t ideal_raw = ideal_raw_for_resistance(resistor_ohms);
    return (uint32_t)(((uint64_t)ideal_raw * FUNKEY_TEST_ADC_TARGET_SCALE_NUM +
                       (FUNKEY_TEST_ADC_TARGET_SCALE_DEN / 2U)) /
                      FUNKEY_TEST_ADC_TARGET_SCALE_DEN);
}

static uint32_t no_contact_threshold_raw(void)
{
    uint32_t digit9_raw = raw_for_resistance(s_digit_resistor_ohms[9]);
    return digit9_raw + ((FUNKEY_TEST_ADC_FULL_SCALE - digit9_raw) / 2U);
}

static uint8_t nearest_digit_from_raw(uint32_t raw, uint32_t *out_target_raw)
{
    uint8_t best_digit = 0;
    uint32_t best_target = raw_for_resistance(s_digit_resistor_ohms[0]);
    uint32_t best_error = raw > best_target ? raw - best_target : best_target - raw;

    for (uint8_t digit = 1; digit < 10; ++digit) {
        uint32_t target = raw_for_resistance(s_digit_resistor_ohms[digit]);
        uint32_t error = raw > target ? raw - target : target - raw;
        if (error < best_error) {
            best_digit = digit;
            best_target = target;
            best_error = error;
        }
    }

    *out_target_raw = best_target;
    return best_digit;
}

static uint32_t resistance_from_raw(uint32_t raw)
{
    if (raw >= FUNKEY_TEST_ADC_FULL_SCALE) {
        return UINT32_MAX;
    }

    uint32_t corrected_raw = (uint32_t)(((uint64_t)raw * FUNKEY_TEST_ADC_TARGET_SCALE_DEN +
                                         (FUNKEY_TEST_ADC_TARGET_SCALE_NUM / 2U)) /
                                        FUNKEY_TEST_ADC_TARGET_SCALE_NUM);
    if (corrected_raw >= FUNKEY_TEST_ADC_FULL_SCALE) {
        return UINT32_MAX;
    }

    return (uint32_t)(((uint64_t)FUNKEY_TEST_PULLUP_OHMS * corrected_raw) /
                      (FUNKEY_TEST_ADC_FULL_SCALE - corrected_raw));
}

static uint32_t raw_delta(uint32_t left, uint32_t right)
{
    return left > right ? left - right : right - left;
}

static void print_resistance(uint32_t ohms)
{
    if (ohms == UINT32_MAX) {
        printf("open");
    } else if (ohms < 1000U) {
        printf("%" PRIu32 " ohm", ohms);
    } else {
        printf("%" PRIu32 ".%01" PRIu32 " kOhm", ohms / 1000U, (ohms % 1000U) / 100U);
    }
}

static esp_err_t settle_adc_channel(const funkey_input_t *input, adc_oneshot_unit_handle_t handle)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(FUNKEY_TEST_ADC_SETTLE_TIMEOUT_MS);
    uint32_t last_raw = 0;
    uint32_t stable_reads = 0;
    bool have_last = false;

    while (true) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(handle, input->channel, &raw);
        if (err != ESP_OK) {
            return err;
        }

        uint32_t current_raw = (uint32_t)raw;
        if (have_last &&
            raw_delta(current_raw, last_raw) <= FUNKEY_TEST_ADC_SETTLE_DELTA_RAW) {
            ++stable_reads;
            if (stable_reads >= FUNKEY_TEST_ADC_SETTLE_READS) {
                return ESP_OK;
            }
        } else {
            stable_reads = 1;
        }

        have_last = true;
        last_raw = current_raw;

        if ((xTaskGetTickCount() - start) >= timeout) {
            ESP_LOGW(TAG, "%s GPIO%d ADC did not settle within %" PRIu32
                          " ms; last raw=%" PRIu32,
                     input->label,
                     input->gpio,
                     (uint32_t)FUNKEY_TEST_ADC_SETTLE_TIMEOUT_MS,
                     last_raw);
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(FUNKEY_TEST_ADC_SETTLE_DELAY_MS));
    }
}

static esp_err_t read_average_raw(const funkey_input_t *input, uint32_t *out_raw)
{
    uint32_t total = 0;
    adc_oneshot_unit_handle_t handle = adc_handle_for(input->unit);
    esp_err_t err = configure_external_pullup_input(input);
    if (err != ESP_OK) {
        return err;
    }

    err = settle_adc_channel(input, handle);
    if (err != ESP_OK) {
        return err;
    }

    for (uint32_t sample = 0; sample < FUNKEY_TEST_SAMPLE_COUNT; ++sample) {
        int raw = 0;
        err = adc_oneshot_read(handle, input->channel, &raw);
        if (err != ESP_OK) {
            return err;
        }

        total += (uint32_t)raw;
        vTaskDelay(pdMS_TO_TICKS(FUNKEY_TEST_SAMPLE_DELAY_MS));
    }

    *out_raw = total / FUNKEY_TEST_SAMPLE_COUNT;
    return ESP_OK;
}

static void init_adc_unit(adc_unit_t unit, adc_oneshot_unit_handle_t *out_handle)
{
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, out_handle));
}

static void print_digital_pullup_check(void)
{
    printf("\nExternal pull-up sanity check before ADC setup\n");
    printf("With 220k pull-ups fitted, nothing connected should read digital=1 on every line.\n");

    for (size_t index = 0; index < sizeof(s_inputs) / sizeof(s_inputs[0]); ++index) {
        const funkey_input_t *input = &s_inputs[index];
        ESP_ERROR_CHECK(gpio_set_direction(input->gpio, GPIO_MODE_INPUT));
        ESP_ERROR_CHECK(configure_external_pullup_input(input));
    }

    vTaskDelay(pdMS_TO_TICKS(25));

    for (size_t index = 0; index < sizeof(s_inputs) / sizeof(s_inputs[0]); ++index) {
        const funkey_input_t *input = &s_inputs[index];
        printf("  %s GPIO%-2d digital=%d\n",
               input->label,
               input->gpio,
               gpio_get_level(input->gpio));
    }
}

static void init_funkey_inputs(void)
{
    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    init_adc_unit(ADC_UNIT_1, &s_adc1);
    init_adc_unit(ADC_UNIT_2, &s_adc2);

    for (size_t index = 0; index < sizeof(s_inputs) / sizeof(s_inputs[0]); ++index) {
        const funkey_input_t *input = &s_inputs[index];
        adc_oneshot_unit_handle_t handle = adc_handle_for(input->unit);

        ESP_ERROR_CHECK(adc_oneshot_config_channel(handle, input->channel, &channel_config));

        // adc_oneshot_config_channel() resets the ADC pad pull state, so set it last.
        ESP_ERROR_CHECK(configure_external_pullup_input(input));
    }
}

void app_main(void)
{
    print_digital_pullup_check();
    init_funkey_inputs();

    printf("\n\nFunkey resistor contact tester\n");
    printf("Wiring, board bottom pads left to right:\n");
    printf("  R3 -> GPIO11\n");
    printf("  R2 -> GPIO10\n");
    printf("  R1 -> GPIO9\n");
    printf("  R4 -> GPIO8\n");
    printf("  GND -> GND\n");
    printf("Using external pull-ups: %" PRIu32 " ohms\n",
           (uint32_t)FUNKEY_TEST_PULLUP_OHMS);
    printf("ADC bucket target scale: %" PRIu32 "/%" PRIu32 "\n",
           (uint32_t)FUNKEY_TEST_ADC_TARGET_SCALE_NUM,
           (uint32_t)FUNKEY_TEST_ADC_TARGET_SCALE_DEN);
    printf("Each GPIO line should have its own 220k pull-up to 3.3 V.\n");
    printf("Decode order is R3 hundreds, R2 tens, R1 ones, R4 checksum.\n");
    printf("No-contact threshold raw=%" PRIu32 "\n", no_contact_threshold_raw());
    printf("A valid ID must repeat for %" PRIu32 " reads before it is marked stable.\n",
           (uint32_t)FUNKEY_TEST_STABLE_READS);

    uint16_t last_valid_id = UINT16_MAX;
    uint32_t stable_read_count = 0;

    while (true) {
        uint8_t digits[4] = {0};
        uint32_t open_count = 0;
        uint32_t read_error_count = 0;
        uint32_t open_threshold = no_contact_threshold_raw();
        printf("\n");

        for (size_t index = 0; index < sizeof(s_inputs) / sizeof(s_inputs[0]); ++index) {
            const funkey_input_t *input = &s_inputs[index];
            uint32_t raw = 0;
            esp_err_t err = read_average_raw(input, &raw);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "%s GPIO%d ADC read failed: %s",
                         input->label, input->gpio, esp_err_to_name(err));
                ++read_error_count;
                continue;
            }

            uint32_t ohms = resistance_from_raw(raw);
            uint32_t target_raw = 0;
            uint8_t digit = nearest_digit_from_raw(raw, &target_raw);
            digits[index] = digit;
            int32_t raw_error = (int32_t)raw - (int32_t)target_raw;
            uint32_t ratio_per_mille =
                (raw * 1000U + (FUNKEY_TEST_ADC_FULL_SCALE / 2U)) / FUNKEY_TEST_ADC_FULL_SCALE;

            printf("%s GPIO%-2d raw=%4" PRIu32 " ratio=%u.%03u est=",
                   input->label,
                   input->gpio,
                   raw,
                   ratio_per_mille / 1000U,
                   ratio_per_mille % 1000U);
            print_resistance(ohms);
            printf(" digit=%u targetRaw=%" PRIu32 " err=%+" PRId32,
                   digit,
                   target_raw,
                   raw_error);

            if (raw >= open_threshold) {
                ++open_count;
                printf("  OPEN/no contact");
            }

            printf("\n");
        }

        if (read_error_count > 0) {
            last_valid_id = UINT16_MAX;
            stable_read_count = 0;
            printf("Decode invalid: %" PRIu32 "/%u ADC channel reads failed or did not settle\n",
                   read_error_count,
                   (unsigned)(sizeof(s_inputs) / sizeof(s_inputs[0])));
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (open_count > 0) {
            last_valid_id = UINT16_MAX;
            stable_read_count = 0;

            if (open_count == sizeof(s_inputs) / sizeof(s_inputs[0])) {
                printf("Decode invalid: all lines are open/no contact; no Funkey is seated or common GND is not connected\n");
            } else {
                printf("Decode invalid: %" PRIu32 "/%u lines open/no contact; check pad alignment and the affected wire(s)\n",
                       open_count,
                       (unsigned)(sizeof(s_inputs) / sizeof(s_inputs[0])));
            }
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint16_t candidate_id = (uint16_t)(digits[0] * 100U + digits[1] * 10U + digits[2]);
        uint8_t expected_checksum = (uint8_t)((digits[0] + digits[1] + digits[2]) % 10U);
        bool checksum_ok = digits[3] == expected_checksum;

        if (!checksum_ok) {
            last_valid_id = UINT16_MAX;
            stable_read_count = 0;
            printf("Decode invalid: R3/R2/R1/R4 digits %u/%u/%u/%u -> candidate ID %" PRIu16
                   " (0x%08" PRIX16 "), checksum FAIL expected %u got %u\n",
                   digits[0],
                   digits[1],
                   digits[2],
                   digits[3],
                   candidate_id,
                   candidate_id,
                   expected_checksum,
                   digits[3]);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (candidate_id == last_valid_id) {
            if (stable_read_count < FUNKEY_TEST_STABLE_READS) {
                ++stable_read_count;
            }
        } else {
            last_valid_id = candidate_id;
            stable_read_count = 1;
        }

        printf("%s R3/R2/R1/R4 digits: %u/%u/%u/%u -> ID %" PRIu16
               " (0x%08" PRIX16 "), checksum OK expected %u got %u, stable %" PRIu32 "/%" PRIu32 "\n",
               stable_read_count >= FUNKEY_TEST_STABLE_READS ? "Decoded stable" : "Valid candidate",
               digits[0],
               digits[1],
               digits[2],
               digits[3],
               candidate_id,
               candidate_id,
               expected_checksum,
               digits[3],
               stable_read_count,
               (uint32_t)FUNKEY_TEST_STABLE_READS);

        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
