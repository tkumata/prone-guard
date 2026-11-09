#pragma once
#include "monitor_settings.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TELEMETRY_MAX_BYTES 16384
typedef struct
{
    uint64_t sequence;
    int64_t timestamp;
    int64_t camera_timestamp;
    bool camera_ok;
    bool face_detected;
    double face_area_px2;
    double face_confidence;
    int face_box[4];
    int face_landmarks[10];
    bool face_landmarks_valid;
    int64_t depth_timestamp;
    bool depth_ok;
    int16_t distance[64];
    uint8_t target_status[64];
} sensor_observation_t;

char *telemetry_encode(const sensor_observation_t *observation, const char *boot_id,
                       const char *bed_id);
double telemetry_face_area(int x0, int y0, int x1, int y1);
