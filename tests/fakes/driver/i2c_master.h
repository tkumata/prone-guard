#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;
typedef struct
{
    const uint8_t *write_buffer;
    size_t buffer_size;
} i2c_master_transmit_multi_buffer_info_t;

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t size,
                              int timeout_ms);
esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t device,
                                           i2c_master_transmit_multi_buffer_info_t *buffers,
                                           size_t count, int timeout_ms);
