#pragma once
#include "depth_map.h"
#include "driver/i2c_master.h"

esp_err_t oled_display_init(i2c_master_dev_handle_t device);
esp_err_t oled_display_write(i2c_master_dev_handle_t device,
                             const uint8_t frame[DEPTH_MAP_FRAME_BYTES]);
