#pragma once
#include "esp_err.h"
#include "protocomm.h"
#include <stdint.h>

esp_err_t pairing_usb_init(void);
esp_err_t pairing_usb_run(protocomm_t *protocol, uint32_t session_id, int64_t deadline_us);
