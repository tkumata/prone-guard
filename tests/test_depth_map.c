#include "depth_map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t frame[DEPTH_MAP_WIDTH * DEPTH_MAP_HEIGHT * 3];

static const uint8_t *pixel(int x, int y)
{
    return frame + (y * DEPTH_MAP_WIDTH + x) * 3;
}

static void render(const depth_map_t *map, int64_t now, bool mirror_x, unsigned face_area_px2)
{
    depth_map_t before = *map;
    uint8_t guarded[DEPTH_MAP_STRIP_BYTES + 2];
    for (int y = 0; y < DEPTH_MAP_HEIGHT; y += DEPTH_MAP_STRIP_HEIGHT)
    {
        memset(guarded, 0xa5, sizeof(guarded));
        depth_map_render_strip(map, now, mirror_x, face_area_px2, y, guarded + 1);
        assert(guarded[0] == 0xa5 && guarded[sizeof(guarded) - 1] == 0xa5);
        memcpy(frame + y * DEPTH_MAP_WIDTH * 3, guarded + 1, DEPTH_MAP_STRIP_BYTES);
    }
    assert(!memcmp(map, &before, sizeof(before)));
    for (size_t i = 0; i < sizeof(frame); ++i)
        assert((frame[i] & 3) == 0);
}

static void cell_color(int cell, uint8_t brightness)
{
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
        {
            const uint8_t *p = pixel((cell % 8) * 16 + x, (cell / 8) * 16 + y);
            assert(p[0] == brightness && p[1] == brightness && p[2] == brightness);
        }
}

static void cross(int cell)
{
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
        {
            const uint8_t *p = pixel((cell % 8) * 16 + x, (cell / 8) * 16 + y);
            assert(p[0] == (x == y || x + y == 15 ? 0xfc : 0));
            assert(p[1] == 0 && p[2] == 0);
        }
}

static void blank_map(void)
{
    for (int cell = 0; cell < 64; ++cell)
        cell_color(cell, 0);
}

static void text_layout(int first_y, const char *text)
{
    int width = first_y == 128 ? 6 : 8;
    assert(strlen(text) * width <= DEPTH_MAP_WIDTH);
    for (int character = 0; character * width < DEPTH_MAP_WIDTH; ++character)
    {
        bool lit = false;
        char letter = (size_t)character < strlen(text) ? text[character] : ' ';
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < width && character * width + x < DEPTH_MAP_WIDTH; ++x)
            {
                const uint8_t *p = pixel(character * width + x, first_y + y);
                bool ink = p[0] || p[1] || p[2];
                assert(p[0] == (ink && letter != '#' && letter != 'x' ? 0xfc : 0));
                assert(p[1] == (ink && letter != 'x' ? 0xfc : 0));
                assert(p[2] == (ink && letter != '#' ? 0xfc : 0));
                int column = x - (width == 8 ? 1 : 0);
                if (column < 0 || column >= 5 || y == 0 || y == 15)
                    assert(!ink);
                lit |= ink;
            }
        assert(lit == (letter != ' '));
    }
}

static void glyph_columns(int character, const uint8_t columns[5])
{
    for (int x = 0; x < 5; ++x)
        for (int y = 1; y < 15; ++y)
        {
            const uint8_t *p = pixel(character * 6 + x, 128 + y);
            assert((p[0] || p[1] || p[2]) == ((columns[x] >> ((y - 1) / 2)) & 1));
        }
}

int main(void)
{
    depth_map_t map = {0};
    static uint8_t labels[5][DEPTH_MAP_STRIP_BYTES], legend[DEPTH_MAP_STRIP_BYTES];
    render(&map, 9000000, false, 0);
    blank_map();
    text_layout(128, "NO DATA/FACE x");
    text_layout(144, "0-4m NEAR=WHITE");
    memcpy(labels[0], pixel(0, 128), sizeof(labels[0]));
    memcpy(legend, pixel(0, 144), sizeof(legend));

    map.available = true;
    map.acquired_us = 10000000;
    memset(map.target_status, 5, sizeof(map.target_status));
    for (int level = 0; level < 64; ++level)
    {
        int distance = 4000 - (level * 4000 + 31) / 63;
        for (int cell = 0; cell < 64; ++cell)
            map.distance[cell] = distance;
        render(&map, 10000000, false, 0);
        for (int cell = 0; cell < 64; ++cell)
            cell_color(cell, level << 2);
        text_layout(128, "DEPTH / FACE x");
        assert(!memcmp(legend, pixel(0, 144), sizeof(legend)));
    }
    memcpy(labels[1], pixel(0, 128), sizeof(labels[1]));

    for (int cell = 0; cell < 64; ++cell)
        map.distance[cell] = 4000;
    for (int cell = 0; cell < 64; ++cell)
    {
        map.distance[cell] = 0;
        render(&map, 10000000, false, 0);
        for (int other = 0; other < 64; ++other)
            cell_color(other, other == cell ? 0xfc : 0);
        map.distance[cell] = 4000;
    }
    map.distance[0] = 2000;
    render(&map, 10000000, false, 0);
    cell_color(0, 32 << 2);
    map.distance[0] = 2001;
    render(&map, 10000000, false, 0);
    cell_color(0, 31 << 2);

    const uint8_t invalid_statuses[] = {0, 6, 9, 255};
    for (unsigned i = 0; i < sizeof(invalid_statuses); ++i)
    {
        map.target_status[63] = invalid_statuses[i];
        render(&map, 10000000, false, 0);
        cross(63);
        cell_color(62, 0);
    }
    map.target_status[63] = 5;
    map.distance[63] = -1;
    render(&map, 10000000, false, 0);
    cross(63);
    map.distance[63] = 4001;
    render(&map, 10000000, false, 0);
    cross(63);

    memset(map.target_status, 255, sizeof(map.target_status));
    render(&map, 10000000, false, 0);
    for (int cell = 0; cell < 64; ++cell)
        cross(cell);
    text_layout(128, "NO VALID/FACE x");
    memcpy(labels[2], pixel(0, 128), sizeof(labels[2]));

    memset(map.target_status, 5, sizeof(map.target_status));
    map.distance[0] = 0;
    render(&map, 15000000, false, 0);
    cell_color(0, 0xfc);
    assert(!memcmp(labels[1], pixel(0, 128), sizeof(labels[1])));
    render(&map, 15000001, false, 0);
    blank_map();
    text_layout(128, "STALE / FACE x");
    memcpy(labels[3], pixel(0, 128), sizeof(labels[3]));
    map.error = true;
    render(&map, 15000001, false, 0);
    blank_map();
    text_layout(128, "ERROR / FACE x");
    memcpy(labels[4], pixel(0, 128), sizeof(labels[4]));
    map.available = false;
    render(&map, 15000001, false, 0);
    assert(!memcmp(labels[4], pixel(0, 128), sizeof(labels[4])));
    map.error = false;
    render(&map, 15000001, false, 0);
    assert(!memcmp(labels[0], pixel(0, 128), sizeof(labels[0])));
    map.available = true;
    map.acquired_us = 15000001;
    render(&map, 15000001, false, 0);
    cell_color(0, 0xfc);
    assert(!memcmp(labels[1], pixel(0, 128), sizeof(labels[1])));
    assert(!memcmp(legend, pixel(0, 144), sizeof(legend)));
    for (int a = 0; a < 5; ++a)
        for (int b = 0; b < a; ++b)
            assert(memcmp(labels[a], labels[b], sizeof(labels[a])));
    static uint8_t original[sizeof(frame)];
    for (int state = 0; state < 5; ++state)
    {
        map.available = state != 0;
        map.error = state == 4;
        map.acquired_us = state == 3 ? 0 : 15000001;
        for (int cell = 0; cell < 64; ++cell)
        {
            map.distance[cell] = 4000 - (cell * 4000 + 31) / 63;
            map.target_status[cell] = state == 2 || cell % 7 == 0 ? 255 : 5;
        }
        unsigned area = state % 2 ? 12 : 76800;
        render(&map, 15000001, false, area);
        memcpy(original, frame, sizeof(frame));
        render(&map, 15000001, true, area);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x)
                assert(!memcmp(pixel(x, y), original + (y * 128 + 127 - x) * 3, 3));
        assert(!memcmp(pixel(0, 128), original + 128 * 128 * 3,
                       2 * DEPTH_MAP_STRIP_BYTES));
    }
    const unsigned areas[] = {76800, 12, 1, 34567, 68900, 0};
    const char *states[] = {"NO DATA", "DEPTH", "NO VALID", "STALE", "ERROR"};
    static const uint8_t digits[10][5] = {
        {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31},
        {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e},
    };
    for (int state = 0; state < 5; ++state)
    {
        map.available = state != 0;
        map.error = state == 4;
        map.acquired_us = state == 3 ? 0 : 15000001;
        memset(map.target_status, state == 2 ? 255 : 5, sizeof(map.target_status));
        render(&map, 15000001, false, 0);
        memcpy(original, frame, sizeof(frame));
        for (unsigned i = 0; i < sizeof(areas) / sizeof(areas[0]); ++i)
        {
            unsigned area = areas[i];
            char expected[24];
            const char *separator = strlen(states[state]) > 5 ? "/" : " / ";
            if (area)
                snprintf(expected, sizeof(expected), "%s%sFACE # %u", states[state], separator, area);
            else
                snprintf(expected, sizeof(expected), "%s%sFACE x", states[state], separator);
            render(&map, 15000001, false, area);
            text_layout(128, expected);
            assert(!memcmp(original, frame, 128 * DEPTH_MAP_WIDTH * 3));
            assert(!memcmp(legend, pixel(0, 144), sizeof(legend)));
            int indicator = strchr(expected, area ? '#' : 'x') - expected;
            const uint8_t square[] = {0x1c, 0x1c, 0x1c, 0x1c, 0x1c};
            const uint8_t cross[] = {0x44, 0x28, 0x10, 0x28, 0x44};
            glyph_columns(indicator, area ? square : cross);
            if (area)
                for (int c = indicator + 2; expected[c]; ++c)
                    glyph_columns(c, digits[expected[c] - '0']);
            else
                assert(!memcmp(original, frame, sizeof(frame)));
        }
    }
    puts("Depth map layout, RGB666 grayscale, text and state transition tests "
         "passed");
}
