#include "device_security.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include <stdlib.h>
#include <string.h>

esp_err_t device_storage_init(void)
{
    if (psa_crypto_init() != PSA_SUCCESS)
        return ESP_FAIL;
    nvs_sec_scheme_t *scheme = nvs_flash_get_default_security_scheme();
    if (!scheme)
        return ESP_ERR_INVALID_STATE;
    nvs_sec_cfg_t keys = {0};
    // Read an existing HMAC key only; firmware must never burn an eFuse automatically.
    esp_err_t err = nvs_flash_read_security_cfg_v2(scheme, &keys);
    if (err == ESP_OK)
        err = nvs_flash_secure_init_partition("secure_nvs", &keys);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    if (err != ESP_OK)
        return err;
    return nvs_flash_init_partition("nvs");
}

static bool copy_field(const cJSON *object, const char *name, char *out, size_t capacity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(value) || !*value->valuestring || strlen(value->valuestring) >= capacity)
        return false;
    memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
    return true;
}

esp_err_t device_provision(const char *json)
{
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    const cJSON *provision = cJSON_GetObjectItemCaseSensitive(root, "provision");
    const cJSON *admin = cJSON_GetObjectItemCaseSensitive(provision, "admin_password");
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(provision, "settings");
    device_security_t *security = calloc(1, sizeof(*security));
    monitor_settings_t updated = {0};
    esp_err_t err = ESP_ERR_INVALID_ARG;
    char *settings_json = NULL;
    unsigned char digest[sizeof(security->admin_sha256)] = {0};
    if (!security)
    {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    if (!cJSON_IsObject(provision) || !cJSON_IsObject(settings) || !cJSON_IsString(admin) ||
        strlen(admin->valuestring) != 64 || !monitor_token_valid(admin->valuestring))
        goto done;
    size_t hash_length = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const unsigned char *)admin->valuestring, 64,
                         digest, sizeof(digest), &hash_length) != PSA_SUCCESS ||
        hash_length != sizeof(digest))
        goto done;
    err = device_security_authorize_provision(security, digest);
    if (err != ESP_OK)
        goto done;

    err = ESP_ERR_INVALID_ARG;
    mbedtls_platform_zeroize(security, sizeof(*security));
    security->version = 1;
    memcpy(security->admin_sha256, digest, sizeof(digest));
    if (!copy_field(provision, "server_certificate", security->certificate,
                    sizeof(security->certificate)) ||
        !copy_field(provision, "server_private_key", security->private_key,
                    sizeof(security->private_key)) ||
        !copy_field(provision, "edge_ca", security->edge_ca, sizeof(security->edge_ca)))
        goto done;
    settings_json = cJSON_PrintUnformatted(settings);
    if (!settings_json)
    {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    monitor_settings_t empty = {0};
    if (monitor_settings_decode(settings_json, &empty, &updated, NULL) != ESP_OK)
        goto done;
    mbedtls_x509_crt certificate, ca;
    mbedtls_pk_context key;
    mbedtls_x509_crt_init(&certificate);
    mbedtls_x509_crt_init(&ca);
    mbedtls_pk_init(&key);
    bool valid = mbedtls_x509_crt_parse(&certificate, (unsigned char *)security->certificate,
                                        strlen(security->certificate) + 1) == 0 &&
                 mbedtls_x509_crt_parse(&ca, (unsigned char *)security->edge_ca,
                                        strlen(security->edge_ca) + 1) == 0 &&
                 mbedtls_pk_parse_key(&key, (unsigned char *)security->private_key,
                                      strlen(security->private_key) + 1, NULL, 0) == 0 &&
                 mbedtls_pk_check_pair(&certificate.pk, &key) == 0;
    mbedtls_x509_crt_free(&certificate);
    mbedtls_x509_crt_free(&ca);
    mbedtls_pk_free(&key);
    if (!valid)
        goto done;
    nvs_handle_t handle;
    err = nvs_open_from_partition("secure_nvs", "prone_guard", NVS_READWRITE, &handle);
    if (err != ESP_OK)
        goto done;
    err = nvs_set_blob(handle, "security", security, sizeof(*security));
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK)
        err = monitor_settings_save(&updated);
    if (err == ESP_OK)
    {
        esp_err_t cleanup = monitor_settings_cleanup_legacy();
        if (cleanup != ESP_OK)
            ESP_LOGE("provision", "legacy credential cleanup failed: %s", esp_err_to_name(cleanup));
    }
done:
    if (settings_json)
    {
        mbedtls_platform_zeroize(settings_json, strlen(settings_json));
        cJSON_free(settings_json);
    }
    if (security)
    {
        mbedtls_platform_zeroize(security, sizeof(*security));
        free(security);
    }
    mbedtls_platform_zeroize(digest, sizeof(digest));
    mbedtls_platform_zeroize(&updated, sizeof(updated));
    cJSON_Delete(root);
    return err;
}

bool device_authorize(httpd_req_t *req, const device_security_t *security)
{
    char header[160] = {0};
    unsigned char credentials[100] = {0}, digest[32] = {0};
    size_t length = 0;
    bool valid =
        httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        strncmp(header, "Basic ", 6) == 0 &&
        mbedtls_base64_decode(credentials, sizeof(credentials), &length,
                              (unsigned char *)header + 6, strlen(header + 6)) == 0 &&
        length == 70 && memcmp(credentials, "admin:", 6) == 0;
    size_t hash_length = 0;
    if (valid)
        valid = psa_hash_compute(PSA_ALG_SHA_256, credentials + 6, 64, digest, 32, &hash_length) ==
                    PSA_SUCCESS &&
                hash_length == 32;
    valid = valid && mbedtls_ct_memcmp(digest, security->admin_sha256, 32) == 0;
    mbedtls_platform_zeroize(header, sizeof(header));
    mbedtls_platform_zeroize(credentials, sizeof(credentials));
    mbedtls_platform_zeroize(digest, sizeof(digest));
    if (!valid)
    {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate",
                           "Basic realm=\"prone-guard\", charset=\"UTF-8\"");
        httpd_resp_send(req, "authentication required", HTTPD_RESP_USE_STRLEN);
    }
    return valid;
}
