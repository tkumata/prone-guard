#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "telemetry.h"
esp_err_t depth_sensor_init(i2c_master_dev_handle_t sensor);
esp_err_t depth_sensor_read(sensor_observation_t *observation);
