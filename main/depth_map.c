#include "depth_map.h"
#include <assert.h>
#include <string.h>

static bool valid_cell(const depth_map_t *map, int cell)
{
    return map->target_status[cell] == 5 && map->distance[cell] >= 0 && map->distance[cell] <= 4000;
}

static bool text_pixel(const char *text, int x, int y)
{
    static const char letters[] = "-04=ADEHILMNOPRSTVWm";
    static const uint8_t glyphs[][5] = {
        {0x08, 0x08, 0x08, 0x08, 0x08}, // -
        {0x3e, 0x51, 0x49, 0x45, 0x3e}, // 0
        {0x18, 0x14, 0x12, 0x7f, 0x10}, // 4
        {0x14, 0x14, 0x14, 0x14, 0x14}, // =
        {0x7e, 0x11, 0x11, 0x11, 0x7e}, // A
        {0x7f, 0x41, 0x41, 0x22, 0x1c}, // D
        {0x7f, 0x49, 0x49, 0x49, 0x41}, // E
        {0x7f, 0x08, 0x08, 0x08, 0x7f}, // H
        {0x00, 0x41, 0x7f, 0x41, 0x00}, // I
        {0x7f, 0x40, 0x40, 0x40, 0x40}, // L
        {0x7f, 0x02, 0x0c, 0x02, 0x7f}, // M
        {0x7f, 0x02, 0x04, 0x08, 0x7f}, // N
        {0x3e, 0x41, 0x41, 0x41, 0x3e}, // O
        {0x7f, 0x09, 0x09, 0x09, 0x06}, // P
        {0x7f, 0x09, 0x19, 0x29, 0x46}, // R
        {0x46, 0x49, 0x49, 0x49, 0x31}, // S
        {0x01, 0x01, 0x7f, 0x01, 0x01}, // T
        {0x1f, 0x20, 0x40, 0x20, 0x1f}, // V
        {0x3f, 0x40, 0x38, 0x40, 0x3f}, // W
        {0x7c, 0x04, 0x18, 0x04, 0x78}, // m
    };
    int column = x % 8 - 1;
    if ((size_t)(x / 8) >= strlen(text) || column < 0 || column >= 5 || y < 1 || y > 14)
        return false;
    const char *letter = strchr(letters, text[x / 8]);
    return letter && ((glyphs[letter - letters][column] >> ((y - 1) / 2)) & 1);
}

void depth_map_render_strip(const depth_map_t *map, int64_t now_us, int first_y,
                            uint8_t pixels[DEPTH_MAP_STRIP_BYTES])
{
    assert(map && pixels && first_y >= 0 && first_y <= DEPTH_MAP_HEIGHT - DEPTH_MAP_STRIP_HEIGHT &&
           first_y % DEPTH_MAP_STRIP_HEIGHT == 0);
    const char *state = map->error                            ? "ERROR"
                        : !map->available                     ? "NO DATA"
                        : now_us - map->acquired_us > 5000000 ? "STALE"
                                                              : NULL;
    bool show_map = !state;
    if (show_map)
    {
        bool any_valid = false;
        for (int cell = 0; cell < 64; ++cell)
            any_valid |= valid_cell(map, cell);
        state = any_valid ? "DEPTH" : "NO VALID";
    }
    memset(pixels, 0, DEPTH_MAP_STRIP_BYTES);
    for (int y = first_y; y < first_y + DEPTH_MAP_STRIP_HEIGHT; ++y)
        for (int x = 0; x < DEPTH_MAP_WIDTH; ++x)
        {
            uint8_t *pixel = pixels + ((y - first_y) * DEPTH_MAP_WIDTH + x) * 3;
            if (y >= 128)
            {
                if (text_pixel(y < 144 ? state : "0-4m NEAR=WHITE", x, y % 16))
                    pixel[0] = pixel[1] = pixel[2] = 0xfc;
            }
            else if (show_map)
            {
                int cell = (y / 16) * 8 + x / 16;
                if (valid_cell(map, cell))
                {
                    uint8_t level = ((4000 - map->distance[cell]) * 63 + 2000) / 4000;
                    pixel[0] = pixel[1] = pixel[2] = level << 2;
                }
                else if (x % 16 == y % 16 || x % 16 + y % 16 == 15)
                    pixel[0] = 0xfc;
            }
        }
}
