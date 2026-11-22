#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open_from_partition(const char *partition, const char *namespace_name,
                                  nvs_open_mode_t mode, nvs_handle_t *handle);
typedef struct {
    size_t used_entries, free_entries, total_entries, namespace_count, available_entries;
} nvs_stats_t;
esp_err_t nvs_get_stats(const char *partition, nvs_stats_t *stats);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
void nvs_close(nvs_handle_t handle);
