#include "depth_sensor.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vl53l5cx_api.h"
#include <string.h>
#include <time.h>

static i2c_master_dev_handle_t device;
static VL53L5CX_Configuration configuration;
static VL53L5CX_ResultsData results;
static bool ready;

uint8_t WrMulti(VL53L5CX_Platform *platform, uint16_t address, uint8_t *values, uint32_t size)
{
    (void)platform;
    uint8_t buffer[258];
    for (uint32_t offset = 0; offset < size;)
    {
        size_t count = size - offset > 256 ? 256 : size - offset;
        uint16_t reg = address + offset;
        buffer[0] = reg >> 8;
        buffer[1] = reg & 255;
        memcpy(buffer + 2, values + offset, count);
        if (i2c_master_transmit(device, buffer, count + 2, 1000) != ESP_OK)
            return 255;
        offset += count;
    }
    return 0;
}

uint8_t RdMulti(VL53L5CX_Platform *platform, uint16_t address, uint8_t *values, uint32_t size)
{
    (void)platform;
    for (uint32_t offset = 0; offset < size;)
    {
        size_t count = size - offset > 256 ? 256 : size - offset;
        uint16_t reg = address + offset;
        uint8_t buffer[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
        if (i2c_master_transmit_receive(device, buffer, 2, values + offset, count, 1000) != ESP_OK)
            return 255;
        offset += count;
    }
    return 0;
}

uint8_t RdByte(VL53L5CX_Platform *p, uint16_t address, uint8_t *value)
{
    return RdMulti(p, address, value, 1);
}
uint8_t WrByte(VL53L5CX_Platform *p, uint16_t address, uint8_t value)
{
    return WrMulti(p, address, &value, 1);
}
uint8_t WaitMs(VL53L5CX_Platform *p, uint32_t ms)
{
    (void)p;
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
    return 0;
}
void SwapBuffer(uint8_t *buffer, uint16_t size)
{
    for (uint16_t i = 0; i + 3 < size; i += 4)
    {
        uint8_t a = buffer[i], b = buffer[i + 1];
        buffer[i] = buffer[i + 3];
        buffer[i + 1] = buffer[i + 2];
        buffer[i + 2] = b;
        buffer[i + 3] = a;
    }
}

esp_err_t depth_sensor_init(i2c_master_dev_handle_t sensor)
{
    ready = false;
    if (!sensor)
        return ESP_ERR_INVALID_ARG;
    device = sensor;
    const gpio_config_t interrupt = {
        .pin_bit_mask = 1ULL << 47, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_DISABLE};
    esp_err_t err = gpio_config(&interrupt);
    if (err != ESP_OK)
        return err;
    uint8_t alive = 0;
    if (vl53l5cx_is_alive(&configuration, &alive) || !alive || vl53l5cx_init(&configuration) ||
        vl53l5cx_set_resolution(&configuration, VL53L5CX_RESOLUTION_8X8) ||
        vl53l5cx_set_ranging_frequency_hz(&configuration, 5) ||
        vl53l5cx_set_target_order(&configuration, VL53L5CX_TARGET_ORDER_STRONGEST) ||
        vl53l5cx_start_ranging(&configuration))
        return ESP_FAIL;
    ready = true;
    return ESP_OK;
}

esp_err_t depth_sensor_read(sensor_observation_t *observation)
{
    if (!ready)
        return ESP_ERR_INVALID_STATE;
    uint8_t available = 0;
    if (vl53l5cx_check_data_ready(&configuration, &available))
    {
        ready = false;
        return ESP_FAIL;
    }
    if (!available)
        return ESP_ERR_NOT_FINISHED;
    if (vl53l5cx_get_ranging_data(&configuration, &results))
    {
        ready = false;
        return ESP_FAIL;
    }
    memcpy(observation->distance, results.distance_mm, sizeof(observation->distance));
    memcpy(observation->target_status, results.target_status, sizeof(observation->target_status));
    observation->depth_timestamp = time(NULL);
    observation->depth_ok = true;
    return ESP_OK;
}
