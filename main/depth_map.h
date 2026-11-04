#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DEPTH_MAP_WIDTH 128
#define DEPTH_MAP_HEIGHT 160
#define DEPTH_MAP_STRIP_HEIGHT 16
#define DEPTH_MAP_STRIP_BYTES (DEPTH_MAP_WIDTH * DEPTH_MAP_STRIP_HEIGHT * 3)

typedef struct
{
    bool available;
    bool error;
    int64_t acquired_us;
    int16_t distance[64];
    uint8_t target_status[64];
} depth_map_t;

void depth_map_render_strip(const depth_map_t *map, int64_t now_us, bool mirror_x, int first_y,
                            uint8_t pixels[DEPTH_MAP_STRIP_BYTES]);
