#include "tft_display.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>

#define TFT_DC GPIO_NUM_41
#define TFT_RESET GPIO_NUM_42
// The panel's visible 128 columns start at controller RAM column 2.
#define TFT_COLUMN_OFFSET 2

static spi_device_handle_t device;
static bool bus_ready, panel_ready, pending;
static spi_transaction_t transaction;
static uint8_t transfer_buffer[DEPTH_MAP_STRIP_BYTES] __attribute__((aligned(4)));

static esp_err_t finish_transfer(void)
{
    if (!pending)
        return ESP_OK;
    spi_transaction_t *completed;
    esp_err_t err = spi_device_get_trans_result(device, &completed, pdMS_TO_TICKS(100));
    if (err == ESP_OK)
        pending = false;
    return err;
}

static esp_err_t transfer(int data, const uint8_t *bytes, size_t size)
{
    esp_err_t err = finish_transfer();
    if (err != ESP_OK)
        return err;
    err = gpio_set_level(TFT_DC, data);
    if (err != ESP_OK)
        return err;
    transaction = (spi_transaction_t){.length = size * 8};
    if (size <= sizeof(transaction.tx_data))
    {
        transaction.flags = SPI_TRANS_USE_TXDATA;
        memcpy(transaction.tx_data, bytes, size);
    }
    else
    {
        memcpy(transfer_buffer, bytes, size);
        transaction.tx_buffer = transfer_buffer;
    }
    err = spi_device_queue_trans(device, &transaction, pdMS_TO_TICKS(100));
    if (err != ESP_OK)
        return err;
    pending = true;
    // Keep the descriptor and data alive after a timeout until completion is collected.
    return finish_transfer();
}

static esp_err_t command(uint8_t value, const uint8_t *data, size_t size)
{
    esp_err_t err = transfer(0, &value, 1);
    return err == ESP_OK && size ? transfer(1, data, size) : err;
}

esp_err_t tft_display_init(void)
{
    panel_ready = false;
    esp_err_t err = finish_transfer();
    if (err != ESP_OK)
        return err;
    const gpio_config_t pins = {.pin_bit_mask = (1ULL << TFT_DC) | (1ULL << TFT_RESET),
                                .mode = GPIO_MODE_OUTPUT,
                                .intr_type = GPIO_INTR_DISABLE};
    err = gpio_config(&pins);
    if (err != ESP_OK)
        return err;
    if (!bus_ready)
    {
        const spi_bus_config_t bus = {.mosi_io_num = GPIO_NUM_2,
                                      .miso_io_num = -1,
                                      .sclk_io_num = GPIO_NUM_1,
                                      .quadwp_io_num = -1,
                                      .quadhd_io_num = -1,
                                      .max_transfer_sz = DEPTH_MAP_STRIP_BYTES};
        err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK)
            return err;
        bus_ready = true;
    }
    if (!device)
    {
        const spi_device_interface_config_t config = {
            .clock_speed_hz = 8000000, .mode = 0, .spics_io_num = GPIO_NUM_38, .queue_size = 1};
        err = spi_bus_add_device(SPI2_HOST, &config, &device);
        if (err != ESP_OK)
            return err;
    }
    err = gpio_set_level(TFT_RESET, 0);
    if (err != ESP_OK)
        return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    err = gpio_set_level(TFT_RESET, 1);
    if (err != ESP_OK)
        return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    err = command(0x11, NULL, 0);
    if (err != ESP_OK)
        return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    // shortcut: generic panel settings, calibrate offsets/inversion on the target hardware.
    static const struct
    {
        uint8_t command, size, data[6];
    } initialization[] = {
        {0xb1, 3, {0x01, 0x2c, 0x2d}},
        {0xb2, 3, {0x01, 0x2c, 0x2d}},
        {0xb3, 6, {0x01, 0x2c, 0x2d, 0x01, 0x2c, 0x2d}},
        {0xb4, 1, {0x07}},
        {0xc0, 3, {0xa2, 0x02, 0x84}},
        {0xc1, 1, {0xc5}},
        {0xc2, 2, {0x0a, 0x00}},
        {0xc3, 2, {0x8a, 0x2a}},
        {0xc4, 2, {0x8a, 0xee}},
        {0xc5, 1, {0x0e}},
        {0x20, 0, {0}},
        {0x36, 1, {0xc8}},
        {0x3a, 1, {0x06}},
    };
    for (size_t i = 0; i < sizeof(initialization) / sizeof(initialization[0]); ++i)
    {
        err = command(initialization[i].command, initialization[i].data, initialization[i].size);
        if (err != ESP_OK)
            return err;
    }
    panel_ready = true;
    return ESP_OK;
}

esp_err_t tft_display_write_strip(int first_y, const uint8_t pixels[DEPTH_MAP_STRIP_BYTES])
{
    if (!pixels || first_y < 0 || first_y > DEPTH_MAP_HEIGHT - DEPTH_MAP_STRIP_HEIGHT ||
        first_y % DEPTH_MAP_STRIP_HEIGHT)
        return ESP_ERR_INVALID_ARG;
    if (!panel_ready)
        return ESP_ERR_INVALID_STATE;
    const uint8_t columns[] = {0, TFT_COLUMN_OFFSET, 0, TFT_COLUMN_OFFSET + DEPTH_MAP_WIDTH - 1};
    const uint8_t rows[] = {0, first_y, 0, first_y + DEPTH_MAP_STRIP_HEIGHT - 1};
    esp_err_t err = command(0x2a, columns, sizeof(columns));
    if (err == ESP_OK)
        err = command(0x2b, rows, sizeof(rows));
    if (err == ESP_OK)
        err = command(0x2c, NULL, 0);
    if (err == ESP_OK)
        err = transfer(1, pixels, DEPTH_MAP_STRIP_BYTES);
    if (err == ESP_OK && first_y == DEPTH_MAP_HEIGHT - DEPTH_MAP_STRIP_HEIGHT)
        err = command(0x29, NULL, 0);
    if (err != ESP_OK)
        panel_ready = false;
    return err;
}
