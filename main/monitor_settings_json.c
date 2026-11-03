#include "cJSON.h"
#include "http_parser.h"
#include "monitor_settings.h"
#include <string.h>

bool monitor_id_valid(const char *value)
{
    size_t len = strlen(value);
    if (len > MONITOR_ID_MAX)
        return false;
    for (size_t i = 0; i < len; ++i)
    {
        char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

bool monitor_token_valid(const char *value)
{
    size_t len = strlen(value);
    if (!len)
        return true;
    if (len != MONITOR_TOKEN_LEN)
        return false;
    for (size_t i = 0; i < len; ++i)
    {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    }
    return true;
}

bool monitor_destination_parse(const char *url, monitor_destination_t *destination)
{
    if (strlen(url) > MONITOR_URL_MAX || strncmp(url, "https://", 8) || !url[8] || url[8] == '/')
        return false;
    struct http_parser_url parsed;
    http_parser_url_init(&parsed);
    if (http_parser_parse_url(url, strlen(url), 0, &parsed) ||
        !(parsed.field_set & (1 << UF_HOST)) || !parsed.field_data[UF_HOST].len ||
        parsed.field_data[UF_HOST].len > 253 ||
        ((parsed.field_set & (1 << UF_PORT)) && !parsed.port) ||
        (parsed.field_set & ((1 << UF_USERINFO) | (1 << UF_QUERY) | (1 << UF_FRAGMENT))))
        return false;
    const char *path = strchr(url + 8, '/');
    if (!path || strcmp(path, "/v1/measurements"))
        return false;
    for (const char *p = url + 8; *p; ++p)
    {
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || strchr("@?#\\", *p))
            return false;
    }
    size_t len = parsed.field_data[UF_HOST].len;
    memcpy(destination->hostname, url + parsed.field_data[UF_HOST].off, len);
    destination->hostname[len] = 0;
    destination->port = (parsed.field_set & (1 << UF_PORT)) ? parsed.port : 443;
    return true;
}

bool monitor_settings_valid(const monitor_settings_t *settings)
{
    if (!memchr(settings->wifi_ssid, 0, sizeof(settings->wifi_ssid)) ||
        !memchr(settings->wifi_password, 0, sizeof(settings->wifi_password)) ||
        !memchr(settings->bed_id, 0, sizeof(settings->bed_id)) ||
        !memchr(settings->destination_url, 0, sizeof(settings->destination_url)) ||
        !memchr(settings->device_key, 0, sizeof(settings->device_key)))
        return false;
    size_t password_len = strlen(settings->wifi_password);
    if (!settings->wifi_ssid[0] || password_len < 8 || password_len > 63)
        return false;
    for (const char *p = settings->wifi_ssid; *p; ++p)
        if ((unsigned char)*p < 32)
            return false;
    for (const char *p = settings->wifi_password; *p; ++p)
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126)
            return false;
    monitor_destination_t destination;
    return monitor_id_valid(settings->bed_id) &&
           (!settings->destination_url[0] ||
            monitor_destination_parse(settings->destination_url, &destination)) &&
           monitor_token_valid(settings->device_key);
}

bool monitor_settings_ready(const monitor_settings_t *settings)
{
    return monitor_settings_valid(settings) && settings->bed_id[0] &&
           settings->destination_url[0] && settings->device_key[0];
}

static bool field(const cJSON *root, const char *name, char *out, size_t capacity, bool keep_blank)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= capacity)
        return false;
    if (*value->valuestring || !keep_blank)
        memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
    return true;
}

esp_err_t monitor_settings_decode(const char *json, const monitor_settings_t *current,
                                  monitor_settings_t *updated)
{
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, root)
    {
        bool known = false;
        const char *names[] = {"wifi_ssid",       "wifi_password", "bed_id",
                               "destination_url", "device_key",    "clear_device_key"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (!strcmp(item->string, names[i]))
                known = true;
        for (const cJSON *other = item->next; other; other = other->next)
            if (!strcmp(item->string, other->string))
                known = false;
        if (!known || strstr(json, "\\u0000"))
        {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }
    *updated = *current;
    const cJSON *clear = cJSON_GetObjectItemCaseSensitive(root, "clear_device_key");
    bool valid = (!clear || cJSON_IsBool(clear)) &&
                 field(root, "wifi_ssid", updated->wifi_ssid, sizeof(updated->wifi_ssid), false) &&
                 field(root, "wifi_password", updated->wifi_password,
                       sizeof(updated->wifi_password), true) &&
                 field(root, "bed_id", updated->bed_id, sizeof(updated->bed_id), false) &&
                 field(root, "destination_url", updated->destination_url,
                       sizeof(updated->destination_url), false) &&
                 field(root, "device_key", updated->device_key, sizeof(updated->device_key), true);
    if (cJSON_IsTrue(clear))
        updated->device_key[0] = 0;
    valid = valid && monitor_settings_valid(updated);
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_ARG;
}
