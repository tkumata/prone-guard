#pragma once
#include <stdbool.h>
#include <stdint.h>

#define JEV_INTERVAL_MS 10000
#define JEV_TIMEOUT_MS 8000

typedef enum {
    JEV_OK = 0,
    JEV_PENDING,
    JEV_NOT_CONFIGURED,
    JEV_NETWORK_ERROR,
    JEV_TIMEOUT,
    JEV_HTTP_ERROR,
    JEV_INVALID_RESPONSE,
    JEV_UNJUDGEABLE,
    JEV_CAMERA_ERROR,
    JEV_CLOCK_NOT_READY,
} jev_status_t;

typedef struct {
    jev_status_t status;
    float prone_probability;
    bool is_prone;
} jev_result_t;

extern const char jev_request_json[];
const char *jev_status_name(jev_status_t status);
jev_result_t jev_parse_response(const char *json, float threshold);
bool jev_schedule_due(int64_t now_ms, int64_t *next_ms);
