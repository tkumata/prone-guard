#include "oled_display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t writes[4][DEPTH_MAP_FRAME_BYTES + 1];
static size_t sizes[4];
static int calls, fail_at;
static esp_err_t failure;
static char device_storage;
static i2c_master_dev_handle_t device = (i2c_master_dev_handle_t)&device_storage;

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t target, const uint8_t *data, size_t size,
                              int timeout_ms)
{
    assert(target == device && timeout_ms > 0 && timeout_ms <= 100);
    assert(calls < 4 && size <= sizeof(writes[0]));
    memcpy(writes[calls], data, size);
    sizes[calls++] = size;
    return calls == fail_at ? failure : ESP_OK;
}

esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t target,
                                           i2c_master_transmit_multi_buffer_info_t *buffers,
                                           size_t count, int timeout_ms)
{
    uint8_t packet[DEPTH_MAP_FRAME_BYTES + 1];
    size_t size = 0;
    for (size_t i = 0; i < count; ++i)
    {
        assert(size + buffers[i].buffer_size <= sizeof(packet));
        memcpy(packet + size, buffers[i].write_buffer, buffers[i].buffer_size);
        size += buffers[i].buffer_size;
    }
    return i2c_master_transmit(target, packet, size, timeout_ms);
}

int main(void)
{
    uint8_t frame[DEPTH_MAP_FRAME_BYTES];
    for (size_t i = 0; i < sizeof(frame); ++i)
        frame[i] = i % 251;
    assert(oled_display_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(oled_display_write(NULL, frame) == ESP_ERR_INVALID_ARG);
    assert(oled_display_write(device, NULL) == ESP_ERR_INVALID_ARG);
    assert(calls == 0);
    assert(oled_display_init(device) == ESP_OK);
    // Check the controller mode, geometry and charge pump, including their parameters.
    const uint8_t initialization[] = {0x00, 0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40,
                                      0x8d, 0x14, 0x20, 0x00, 0xa1, 0xc8, 0xda, 0x12, 0x81,
                                      0x7f, 0xd9, 0xf1, 0xdb, 0x20, 0xa4, 0xa6, 0x2e};
    assert(sizes[0] == sizeof(initialization));
    assert(!memcmp(writes[0], initialization, sizeof(initialization)));

    calls = 0;
    assert(oled_display_write(device, frame) == ESP_OK);
    assert(calls == 3);
    const uint8_t address[] = {0, 0x21, 0, 127, 0x22, 0, 7};
    assert(sizes[0] == sizeof(address) && !memcmp(writes[0], address, sizeof(address)));
    assert(sizes[1] == sizeof(frame) + 1 && writes[1][0] == 0x40);
    assert(!memcmp(writes[1] + 1, frame, sizeof(frame)));
    assert(sizes[2] == 2 && writes[2][0] == 0 && writes[2][1] == 0xaf);

    const esp_err_t failures[] = {ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i)
    {
        failure = failures[i];
        calls = 0;
        fail_at = 1;
        assert(oled_display_init(device) == failure && calls == 1);
        for (fail_at = 1; fail_at <= 3; ++fail_at)
        {
            calls = 0;
            assert(oled_display_write(device, frame) == failure);
            assert(calls == fail_at);
        }
    }
    calls = 0;
    fail_at = 0;
    assert(oled_display_init(device) == ESP_OK);
    assert(oled_display_write(device, frame) == ESP_OK);
    puts("OLED I2C transfer and failure behavior tests passed");
}
