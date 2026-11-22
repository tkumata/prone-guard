#include "device_security.h"
#include "mbedtls/constant_time.h"
#include "nvs.h"
#include <string.h>

esp_err_t device_security_load(device_security_t *security)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition("secure_nvs", "prone_guard", NVS_READONLY, &handle);
    if (err != ESP_OK)
        return err;
    size_t size = sizeof(*security);
    err = nvs_get_blob(handle, "security", security, &size);
    nvs_close(handle);
    if (err != ESP_OK)
        return err;
    if (size != sizeof(*security) || security->version != 1 ||
        !memchr(security->certificate, 0, sizeof(security->certificate)) ||
        !memchr(security->private_key, 0, sizeof(security->private_key)) ||
        !memchr(security->edge_ca, 0, sizeof(security->edge_ca)) || !security->certificate[0] ||
        !security->private_key[0] || !security->edge_ca[0])
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t device_security_authorize_provision(device_security_t *security,
                                              const unsigned char digest[32])
{
    nvs_stats_t stats;
    esp_err_t err = nvs_get_stats("secure_nvs", &stats);
    if (err != ESP_OK)
        return err;
    // A failed key lookup does not prove that the dedicated partition is empty.
    if (stats.used_entries == stats.namespace_count)
        return ESP_OK;

    err = device_security_load(security);
    if (err != ESP_OK)
        return err;
    return mbedtls_ct_memcmp(digest, security->admin_sha256, sizeof(security->admin_sha256)) == 0
               ? ESP_OK
               : ESP_ERR_INVALID_ARG;
}
