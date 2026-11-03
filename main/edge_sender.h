#pragma once
#include "esp_err.h"
esp_err_t edge_sender_send(const char *url, const char *key, const char *jsonl, const char *ca);
