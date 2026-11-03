#pragma once
#include "esp_http_server.h"
#include "monitor_settings.h"

typedef struct
{
    uint32_t version;
    char certificate[4096];
    char private_key[2048];
    char edge_ca[4096];
    unsigned char admin_sha256[32];
} device_security_t;

esp_err_t device_storage_init(void);
esp_err_t device_security_load(device_security_t *security);
esp_err_t device_provision(const char *json);
bool device_authorize(httpd_req_t *req, const device_security_t *security);
