#pragma once

#include "esp_err.h"

namespace funkey::physical {

esp_err_t initReader();
void pollReader();

} // namespace funkey::physical
