#pragma once
#include "depth_map.h"
#include "esp_err.h"

// Only the display task owns this panel and its SPI transactions.
esp_err_t tft_display_init(void);
esp_err_t tft_display_write_strip(int first_y, const uint8_t pixels[DEPTH_MAP_STRIP_BYTES]);
