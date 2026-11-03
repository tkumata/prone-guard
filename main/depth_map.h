#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DEPTH_MAP_FRAME_BYTES 1024

typedef struct
{
    bool available;
    bool error;
    int64_t acquired_us;
    int16_t distance[64];
    uint8_t target_status[64];
} depth_map_t;

void depth_map_render(const depth_map_t *map, int64_t now_us, uint8_t frame[DEPTH_MAP_FRAME_BYTES]);
