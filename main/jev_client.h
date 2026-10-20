#pragma once
#include <stddef.h>
#include <stdint.h>
#include "jev_protocol.h"

jev_result_t jev_classify(const uint8_t *jpeg, size_t jpeg_len, const char *api_key, float threshold);
