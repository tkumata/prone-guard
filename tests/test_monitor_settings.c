#include "monitor_settings.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t stored[sizeof(monitor_settings_t) + 1];
static size_t stored_size;
static uint8_t pending[sizeof(stored)];
static size_t pending_size;
static esp_err_t open_error, read_error, write_error, commit_error;
static unsigned commits, closes;

esp_err_t nvs_open_from_partition(const char *partition, const char *namespace_name,
                                  nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(partition, "secure_nvs"));
    assert(!strcmp(namespace_name, "prone_guard"));
    assert(mode == NVS_READONLY || mode == NVS_READWRITE);
    if (open_error)
        return open_error;
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    assert(handle == 1 && !strcmp(key, "edge_settings"));
    if (read_error)
        return read_error;
    if (*length < stored_size)
        return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(value, stored, stored_size);
    *length = stored_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    assert(handle == 1 && !strcmp(key, "edge_settings") && length <= sizeof(pending));
    if (write_error)
        return write_error;
    memcpy(pending, value, length);
    pending_size = length;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    ++commits;
    if (commit_error)
        return commit_error;
    memcpy(stored, pending, pending_size);
    stored_size = pending_size;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    (void)key;
    assert(!"Settings must not be erased during load or save");
    return ESP_FAIL;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
    ++closes;
}

int main(void)
{
    monitor_settings_t expected = {.wifi_ssid = "saved-network",
                                   .wifi_password = "saved-password",
                                   .bed_id = "bed-01",
                                   .destination_url = "https://edge.example/v1/measurements"};
    memset(expected.device_key, 'a', 64);
    monitor_settings_t loaded;
    // This is the byte layout written by the firmware before the mirror setting existed.
    stored_size = 741;
    memcpy(stored, &expected, stored_size);
    assert(monitor_settings_load(&loaded) == ESP_OK);
    assert(!memcmp(&loaded, &expected, sizeof(expected)));
    assert(commits == 0 && stored_size == 741);

    for (unsigned mirror = 0; mirror <= 1; ++mirror)
    {
        expected.tft_mirror_x = mirror;
        assert(monitor_settings_save(&expected) == ESP_OK);
        assert(stored_size == sizeof(expected));
        memset(&loaded, 0xa5, sizeof(loaded));
        assert(monitor_settings_load(&loaded) == ESP_OK);
        assert(!memcmp(&loaded, &expected, sizeof(expected)));
    }
    expected.tft_mirror_x = 0;
    assert(monitor_settings_save(&expected) == ESP_OK);
    monitor_settings_t before = loaded;
    const size_t bad_sizes[] = {0, 740, sizeof(stored)};
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); ++i)
    {
        stored_size = bad_sizes[i];
        assert(monitor_settings_load(&loaded) != ESP_OK);
        assert(!memcmp(&loaded, &before, sizeof(loaded)));
    }
    stored_size = sizeof(expected);
    stored[741] = 2;
    assert(monitor_settings_load(&loaded) == ESP_ERR_INVALID_STATE);
    stored[741] = 0;
    stored[0] = 0;
    assert(monitor_settings_load(&loaded) == ESP_ERR_INVALID_STATE);
    stored_size = 741;
    assert(monitor_settings_load(&loaded) == ESP_ERR_INVALID_STATE);
    assert(!memcmp(&loaded, &before, sizeof(loaded)));
    memcpy(stored, &expected, sizeof(expected));
    stored_size = sizeof(expected);

    read_error = ESP_ERR_NVS_NOT_FOUND;
    assert(monitor_settings_load(&loaded) == read_error);
    read_error = ESP_FAIL;
    assert(monitor_settings_load(&loaded) == read_error);
    read_error = ESP_OK;
    open_error = ESP_FAIL;
    unsigned closed_before = closes;
    assert(monitor_settings_load(&loaded) == open_error);
    assert(monitor_settings_save(&expected) == open_error);
    assert(closes == closed_before);
    open_error = ESP_OK;
    assert(!memcmp(&loaded, &before, sizeof(loaded)));

    expected.tft_mirror_x = 1;
    write_error = ESP_FAIL;
    unsigned commits_before = commits;
    assert(monitor_settings_save(&expected) == write_error);
    assert(commits == commits_before);
    write_error = ESP_OK;
    commit_error = ESP_FAIL;
    assert(monitor_settings_save(&expected) == commit_error);
    commit_error = ESP_OK;
    assert(monitor_settings_load(&loaded) == ESP_OK);
    assert(loaded.tft_mirror_x == 0);
    expected.tft_mirror_x = 2;
    assert(monitor_settings_save(&expected) == ESP_ERR_INVALID_ARG);
    puts("Settings NVS compatibility, round-trip and failure tests passed");
}
