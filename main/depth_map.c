#include "depth_map.h"
#include <string.h>

static void label(uint8_t *frame, const char *text)
{
    static const char letters[] = "ADEHILNOPRSTV";
    static const uint8_t glyphs[][5] = {
        {0x7e, 0x11, 0x11, 0x11, 0x7e}, // A
        {0x7f, 0x41, 0x41, 0x22, 0x1c}, // D
        {0x7f, 0x49, 0x49, 0x49, 0x41}, // E
        {0x7f, 0x08, 0x08, 0x08, 0x7f}, // H
        {0x00, 0x41, 0x7f, 0x41, 0x00}, // I
        {0x7f, 0x40, 0x40, 0x40, 0x40}, // L
        {0x7f, 0x02, 0x04, 0x08, 0x7f}, // N
        {0x3e, 0x41, 0x41, 0x41, 0x3e}, // O
        {0x7f, 0x09, 0x09, 0x09, 0x06}, // P
        {0x7f, 0x09, 0x19, 0x29, 0x46}, // R
        {0x46, 0x49, 0x49, 0x49, 0x31}, // S
        {0x01, 0x01, 0x7f, 0x01, 0x01}, // T
        {0x1f, 0x20, 0x40, 0x20, 0x1f}, // V
    };
    for (int x = 68; *text; ++text, x += 6)
    {
        const char *letter = strchr(letters, *text);
        if (letter)
            memcpy(frame + x, glyphs[letter - letters], 5);
    }
}

void depth_map_render(const depth_map_t *map, int64_t now_us, uint8_t frame[DEPTH_MAP_FRAME_BYTES])
{
    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    memset(frame, 0, DEPTH_MAP_FRAME_BYTES);
    if (map->error || !map->available || now_us - map->acquired_us > 5000000)
    {
        label(frame, map->error ? "ERROR" : !map->available ? "NO DATA" : "STALE");
        return;
    }

    bool any_valid = false;
    for (int cell = 0; cell < 64; ++cell)
    {
        int distance = map->distance[cell];
        bool valid = map->target_status[cell] == 5 && distance >= 0 && distance <= 4000;
        int level = valid ? ((4000 - distance) * 16 + 2000) / 4000 : 0;
        any_valid |= valid;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
            {
                bool on = valid ? bayer[y % 4][x % 4] < level : x == y || x + y == 7;
                if (on)
                    frame[(cell / 8) * 128 + (cell % 8) * 8 + x] |= 1u << y;
            }
    }
    label(frame, any_valid ? "DEPTH" : "NO VALID");
}
