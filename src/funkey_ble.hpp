#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "funkey_protocol.hpp"

namespace funkey::ble {

esp_err_t init();
void copyStatus(uint8_t out[FUNKEY_BLE_STATUS_LEN]);

} // namespace funkey::ble
