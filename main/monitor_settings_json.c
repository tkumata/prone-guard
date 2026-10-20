#include "monitor_settings.h"
#include <math.h>
#include <string.h>
#include "cJSON.h"

static bool valid_key(const char *key)
{
    size_t len = strlen(key);
    if (len > JEV_API_KEY_MAX) return false;
    for (size_t i = 0; i < len; ++i) {
        if ((unsigned char)key[i] < 33 || (unsigned char)key[i] > 126) return false;
    }
    return true;
}

esp_err_t monitor_settings_decode(const char *json, const monitor_settings_t *current,
                                  monitor_settings_t *updated)
{
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    cJSON *threshold = cJSON_GetObjectItemCaseSensitive(root, "threshold");
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    bool valid = cJSON_IsString(mode) &&
        (!strcmp(mode->valuestring, "local") || !strcmp(mode->valuestring, "jev")) &&
        cJSON_IsNumber(threshold) && isfinite(threshold->valuedouble) &&
        threshold->valuedouble >= 0 && threshold->valuedouble <= 1 &&
        cJSON_IsString(key) && valid_key(key->valuestring);
    if (valid) {
        *updated = *current;
        updated->use_jev = !strcmp(mode->valuestring, "jev");
        updated->threshold = (float)threshold->valuedouble;
        if (key->valuestring[0]) {
            memcpy(updated->api_key, key->valuestring, strlen(key->valuestring) + 1);
        }
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_ARG;
}

bool monitor_settings_valid(const monitor_settings_t *settings)
{
    return settings->use_jev <= 1 && memchr(settings->api_key, 0, sizeof(settings->api_key)) &&
        valid_key(settings->api_key) && isfinite(settings->threshold) &&
        settings->threshold >= 0 && settings->threshold <= 1;
}
