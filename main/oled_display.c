#include "oled_display.h"

esp_err_t oled_display_init(i2c_master_dev_handle_t device)
{
    if (!device)
        return ESP_ERR_INVALID_ARG;
    // Keep the panel off until the first complete frame replaces its RAM contents.
    static const uint8_t commands[] = {0x00, 0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40,
                                       0x8d, 0x14, 0x20, 0x00, 0xa1, 0xc8, 0xda, 0x12, 0x81,
                                       0x7f, 0xd9, 0xf1, 0xdb, 0x20, 0xa4, 0xa6, 0x2e};
    return i2c_master_transmit(device, commands, sizeof(commands), 100);
}

esp_err_t oled_display_write(i2c_master_dev_handle_t device,
                             const uint8_t frame[DEPTH_MAP_FRAME_BYTES])
{
    if (!device || !frame)
        return ESP_ERR_INVALID_ARG;
    static const uint8_t address[] = {0x00, 0x21, 0, 127, 0x22, 0, 7};
    esp_err_t err = i2c_master_transmit(device, address, sizeof(address), 100);
    if (err != ESP_OK)
        return err;
    const uint8_t control = 0x40;
    i2c_master_transmit_multi_buffer_info_t buffers[] = {
        {.write_buffer = &control, .buffer_size = 1},
        {.write_buffer = frame, .buffer_size = DEPTH_MAP_FRAME_BYTES}};
    err = i2c_master_multi_buffer_transmit(device, buffers, 2, 100);
    if (err != ESP_OK)
        return err;
    static const uint8_t on[] = {0x00, 0xaf};
    return i2c_master_transmit(device, on, sizeof(on), 100);
}
