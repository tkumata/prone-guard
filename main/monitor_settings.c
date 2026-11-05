#include "monitor_settings.h"
#include "nvs.h"
#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(monitor_settings_t, tft_mirror_x) == 741,
               "Keep the legacy settings blob layout unchanged");
_Static_assert(offsetof(monitor_settings_t, face_model) == 742,
               "Keep the settings blob with TFT mirror compatible");

esp_err_t monitor_settings_load(monitor_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition("secure_nvs", "prone_guard", NVS_READONLY, &handle);
    if (err != ESP_OK)
        return err;
    monitor_settings_t loaded = {0};
    size_t size = sizeof(loaded);
    err = nvs_get_blob(handle, "edge_settings", &loaded, &size);
    nvs_close(handle);
    if (err != ESP_OK)
        return err;
    if ((size != offsetof(monitor_settings_t, tft_mirror_x) &&
         size != offsetof(monitor_settings_t, face_model) && size != sizeof(loaded)) ||
        !monitor_settings_valid(&loaded))
        return ESP_ERR_INVALID_STATE;
    *settings = loaded;
    return ESP_OK;
}

esp_err_t monitor_settings_save(const monitor_settings_t *settings)
{
    if (!monitor_settings_valid(settings))
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition("secure_nvs", "prone_guard", NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_set_blob(handle, "edge_settings", settings, sizeof(*settings));
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK)
        return err;
    return ESP_OK;
}

esp_err_t monitor_settings_cleanup_legacy(void)
{
    nvs_handle_t handle;
    // Remove only the obsolete Jev blob after the replacement settings are committed.
    esp_err_t err = nvs_open_from_partition("nvs", "prone_guard", NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (err != ESP_OK)
        return err;
    err = nvs_erase_key(handle, "settings");
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
