#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define JEV_API_KEY_MAX 512

typedef struct {
    uint8_t use_jev;
    float threshold;
    char api_key[JEV_API_KEY_MAX + 1];
} monitor_settings_t;

bool monitor_settings_valid(const monitor_settings_t *settings);
esp_err_t monitor_settings_load(monitor_settings_t *settings);
esp_err_t monitor_settings_save(const monitor_settings_t *settings);
esp_err_t monitor_settings_decode(const char *json, const monitor_settings_t *current,
                                  monitor_settings_t *updated);
