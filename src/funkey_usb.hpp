#pragma once

#include "esp_err.h"

namespace funkey::usb {

esp_err_t init();
bool sendPortalReport();

} // namespace funkey::usb
