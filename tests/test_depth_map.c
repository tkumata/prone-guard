#include "depth_map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool pixel(const uint8_t *frame, int x, int y)
{
    return (frame[(y / 8) * 128 + x] >> (y % 8)) & 1;
}

static int lit_pixels(const uint8_t *frame, int cell)
{
    int count = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            count += pixel(frame, (cell % 8) * 8 + x, (cell / 8) * 8 + y);
    return count;
}

static void blank_map(const uint8_t *frame)
{
    for (int cell = 0; cell < 64; ++cell)
        assert(lit_pixels(frame, cell) == 0);
}

static void cross(const uint8_t *frame, int cell)
{
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            assert(pixel(frame, (cell % 8) * 8 + x, (cell / 8) * 8 + y) == (x == y || x + y == 7));
}

int main(void)
{
    depth_map_t map = {0};
    uint8_t frame[DEPTH_MAP_FRAME_BYTES], no_data[DEPTH_MAP_FRAME_BYTES];
    memset(frame, 0xff, sizeof(frame));
    depth_map_render(&map, 9000000, frame);
    blank_map(frame);
    assert(frame[68] == 0x7f && frame[69] == 0x02); // N
    assert(frame[86] == 0x7f && frame[87] == 0x41); // D in NO DATA
    memcpy(no_data, frame, sizeof(frame));

    map.available = true;
    map.acquired_us = 10000000;
    memset(map.target_status, 5, sizeof(map.target_status));
    for (int level = 0; level <= 16; ++level)
    {
        for (int i = 0; i < 64; ++i)
            map.distance[i] = 4000 - level * 250;
        depth_map_t before = map;
        depth_map_render(&map, 10000000, frame);
        assert(!memcmp(&map, &before, sizeof(map)));
        for (int cell = 0; cell < 64; ++cell)
            assert(lit_pixels(frame, cell) == level * 4);
        assert(frame[68] == 0x7f && frame[69] == 0x41); // D in DEPTH
    }

    for (int i = 0; i < 64; ++i)
        map.distance[i] = 4000;
    for (int cell = 0; cell < 64; ++cell)
    {
        map.distance[cell] = 0;
        depth_map_render(&map, 10000000, frame);
        for (int other = 0; other < 64; ++other)
            assert(lit_pixels(frame, other) == (other == cell ? 64 : 0));
        map.distance[cell] = 4000;
    }

    map.distance[0] = 2000;
    depth_map_render(&map, 10000000, frame);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            assert(pixel(frame, x, y) == ((x + y) % 2 == 0));
    map.distance[0] = 3875; // Halfway between levels 0 and 1 rounds up.
    depth_map_render(&map, 10000000, frame);
    assert(lit_pixels(frame, 0) == 4);
    map.distance[0] = 3876;
    depth_map_render(&map, 10000000, frame);
    assert(lit_pixels(frame, 0) == 0);

    const uint8_t invalid_statuses[] = {0, 6, 9, 255};
    for (unsigned i = 0; i < sizeof(invalid_statuses); ++i)
    {
        map.target_status[63] = invalid_statuses[i];
        depth_map_render(&map, 10000000, frame);
        cross(frame, 63);
    }
    map.target_status[63] = 5;
    map.distance[63] = -1;
    depth_map_render(&map, 10000000, frame);
    cross(frame, 63);
    map.distance[63] = 4001;
    depth_map_render(&map, 10000000, frame);
    cross(frame, 63);

    memset(map.target_status, 255, sizeof(map.target_status));
    depth_map_render(&map, 10000000, frame);
    for (int cell = 0; cell < 64; ++cell)
        cross(frame, cell);
    assert(frame[68] == 0x7f && frame[69] == 0x02); // N in NO VALID
    assert(frame[86] == 0x1f && frame[87] == 0x20); // V

    memset(map.target_status, 5, sizeof(map.target_status));
    map.distance[0] = 0;
    depth_map_render(&map, 15000000, frame);
    assert(lit_pixels(frame, 0) == 64);
    depth_map_render(&map, 15000001, frame);
    blank_map(frame);
    assert(frame[68] == 0x46 && frame[69] == 0x49); // S in STALE
    map.error = true;
    depth_map_render(&map, 15000001, frame);
    blank_map(frame);
    assert(frame[68] == 0x7f && frame[69] == 0x49); // E in ERROR
    map.available = false;
    depth_map_render(&map, 15000001, frame);
    assert(frame[68] == 0x7f && frame[69] == 0x49);
    map.available = true;
    map.error = false;
    map.acquired_us = 15000001;
    depth_map_render(&map, 15000001, frame);
    assert(lit_pixels(frame, 0) == 64);
    assert(frame[68] == 0x7f && frame[69] == 0x41);
    assert(memcmp(no_data, frame, sizeof(frame)));
    puts("Depth map rendering behavior tests passed");
}
