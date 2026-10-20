#include "monitor_settings.h"

#include <string.h>
#include "nvs.h"

esp_err_t monitor_settings_load(monitor_settings_t *settings)
{
    *settings = (monitor_settings_t){.threshold = 0.5f};
    nvs_handle_t handle;
    esp_err_t err = nvs_open("prone_guard", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    size_t size = sizeof(*settings);
    monitor_settings_t stored;
    err = nvs_get_blob(handle, "settings", &stored, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (size != sizeof(stored) || !monitor_settings_valid(&stored)) return ESP_ERR_INVALID_STATE;
    *settings = stored;
    return ESP_OK;
}

esp_err_t monitor_settings_save(const monitor_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("prone_guard", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "settings", settings, sizeof(*settings));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
