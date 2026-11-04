#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define MONITOR_ID_MAX 64
#define MONITOR_URL_MAX 512
#define MONITOR_TOKEN_LEN 64

typedef struct
{
    char wifi_ssid[33];
    char wifi_password[65];
    char bed_id[MONITOR_ID_MAX + 1];
    char destination_url[MONITOR_URL_MAX + 1];
    char device_key[MONITOR_TOKEN_LEN + 1];
    uint8_t tft_mirror_x; // Store 0/1 so invalid NVS bytes can be rejected.
} monitor_settings_t;

typedef struct
{
    char hostname[254];
    uint16_t port;
} monitor_destination_t;

bool monitor_destination_parse(const char *url, monitor_destination_t *destination);

bool monitor_settings_valid(const monitor_settings_t *settings);
bool monitor_settings_ready(const monitor_settings_t *settings);
esp_err_t monitor_settings_load(monitor_settings_t *settings);
esp_err_t monitor_settings_save(const monitor_settings_t *settings);
esp_err_t monitor_settings_cleanup_legacy(void);
esp_err_t monitor_settings_decode(const char *json, const monitor_settings_t *current,
                                  monitor_settings_t *updated);
bool monitor_id_valid(const char *value);
bool monitor_token_valid(const char *value);
