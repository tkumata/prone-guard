#include "device_security.h"
#include "mbedtls/constant_time.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static device_security_t stored = {.version = 1, .certificate = "certificate",
                                   .private_key = "key", .edge_ca = "ca", .admin_sha256 = {1}};
static esp_err_t stats_status, blob_status;
static nvs_stats_t stats;

esp_err_t nvs_open_from_partition(const char *partition, const char *namespace_name,
                                  nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!strcmp(partition, "secure_nvs") && !strcmp(namespace_name, "prone_guard"));
    assert(mode == NVS_READONLY);
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_stats(const char *partition, nvs_stats_t *result)
{
    assert(!strcmp(partition, "secure_nvs"));
    *result = stats;
    return stats_status;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    assert(handle == 1 && !strcmp(key, "security") && *length == sizeof(stored));
    if (blob_status == ESP_OK)
        memcpy(value, &stored, sizeof(stored));
    return blob_status;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
}

int mbedtls_ct_memcmp(const void *a, const void *b, size_t length)
{
    return memcmp(a, b, length);
}

int main(void)
{
    device_security_t scratch;
    const unsigned char owner[32] = {1};
    const unsigned char other[32] = {2};

    assert(device_security_authorize_provision(&scratch, other) == ESP_OK);
    stats.used_entries = stats.namespace_count = 1;
    assert(device_security_authorize_provision(&scratch, other) == ESP_OK);
    stats_status = ESP_ERR_INVALID_STATE;
    assert(device_security_authorize_provision(&scratch, other) == ESP_ERR_INVALID_STATE);
    stats_status = ESP_OK;

    stats.used_entries = 8;
    blob_status = ESP_ERR_NVS_NOT_FOUND;
    assert(device_security_authorize_provision(&scratch, other) == ESP_ERR_NVS_NOT_FOUND);
    blob_status = ESP_OK;
    assert(device_security_authorize_provision(&scratch, other) == ESP_ERR_INVALID_ARG);
    assert(device_security_authorize_provision(&scratch, owner) == ESP_OK);

    blob_status = ESP_FAIL;
    assert(device_security_authorize_provision(&scratch, owner) == ESP_FAIL);
    blob_status = ESP_OK;
    stored.version = 0;
    assert(device_security_authorize_provision(&scratch, owner) == ESP_ERR_INVALID_STATE);
    puts("USB provisioning authorization tests passed");
}
