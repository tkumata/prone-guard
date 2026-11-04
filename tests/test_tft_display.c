#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/task.h"
#include "tft_display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct
{
    int dc;
    size_t size;
    uint8_t bytes[DEPTH_MAP_STRIP_BYTES];
} writes[128];
static int calls, queue_fail_at, stall_at, stalls_left, dc;
static int bus_calls, device_calls, gpio_calls, reset_calls, delays;
static esp_err_t gpio_error, bus_error, device_error;
static spi_transaction_t *active, descriptor;
static uint8_t active_bytes[DEPTH_MAP_STRIP_BYTES];
static char device_storage;
static spi_device_handle_t device = (spi_device_handle_t)&device_storage;

static const uint8_t *data(const spi_transaction_t *transaction)
{
    return transaction->flags & SPI_TRANS_USE_TXDATA ? transaction->tx_data
                                                     : transaction->tx_buffer;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(!active);
    assert(config->pin_bit_mask == ((1ULL << 41) | (1ULL << 42)));
    assert(config->mode == GPIO_MODE_OUTPUT && config->intr_type == GPIO_INTR_DISABLE);
    ++gpio_calls;
    return gpio_error;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    assert(!active && level <= 1);
    if (pin == 41)
        dc = level;
    else
    {
        assert(pin == 42);
        ++reset_calls;
    }
    return gpio_error;
}

void vTaskDelay(TickType_t ticks)
{
    assert(!active && (ticks == 10 || ticks == 120));
    ++delays;
}

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma)
{
    assert(host == SPI2_HOST && dma == SPI_DMA_CH_AUTO);
    assert(config->mosi_io_num == 2 && config->sclk_io_num == 1 && config->miso_io_num == -1);
    assert(config->quadwp_io_num == -1 && config->quadhd_io_num == -1);
    assert(config->max_transfer_sz == DEPTH_MAP_STRIP_BYTES);
    ++bus_calls;
    return bus_error;
}

esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config,
                             spi_device_handle_t *result)
{
    assert(host == SPI2_HOST && config->mode == 0 && config->clock_speed_hz == 8000000);
    assert(config->spics_io_num == 38 && config->queue_size == 1);
    ++device_calls;
    if (device_error == ESP_OK)
        *result = device;
    return device_error;
}

esp_err_t spi_device_queue_trans(spi_device_handle_t target, spi_transaction_t *transaction,
                                 uint32_t ticks)
{
    assert(target == device && !active && ticks > 0 && ticks <= 100);
    assert(calls < 128 && transaction->length % 8 == 0);
    size_t size = transaction->length / 8;
    assert(size > 0 && size <= DEPTH_MAP_STRIP_BYTES);
    writes[calls].dc = dc;
    writes[calls].size = size;
    memcpy(writes[calls].bytes, data(transaction), size);
    if (++calls == queue_fail_at)
        return ESP_ERR_TIMEOUT;
    active = transaction;
    memcpy(&descriptor, transaction, sizeof(descriptor));
    memcpy(active_bytes, data(transaction), size);
    return ESP_OK;
}

esp_err_t spi_device_get_trans_result(spi_device_handle_t target, spi_transaction_t **result,
                                      uint32_t ticks)
{
    assert(target == device && active && ticks > 0 && ticks <= 100);
    assert(!memcmp(active, &descriptor, sizeof(descriptor)));
    assert(!memcmp(data(active), active_bytes, active->length / 8));
    if (calls == stall_at && stalls_left > 0)
    {
        --stalls_left;
        return ESP_ERR_TIMEOUT;
    }
    *result = active;
    active = NULL;
    return ESP_OK;
}

static void reset_log(void)
{
    assert(!active);
    calls = queue_fail_at = stall_at = stalls_left = 0;
}

int main(void)
{
    static uint8_t pixels[DEPTH_MAP_STRIP_BYTES];
    assert(tft_display_write_strip(0, pixels) == ESP_ERR_INVALID_STATE);
    assert(tft_display_write_strip(0, NULL) == ESP_ERR_INVALID_ARG);
    assert(tft_display_write_strip(-16, pixels) == ESP_ERR_INVALID_ARG);
    assert(tft_display_write_strip(1, pixels) == ESP_ERR_INVALID_ARG);
    assert(tft_display_write_strip(160, pixels) == ESP_ERR_INVALID_ARG);
    assert(calls == 0);

    gpio_error = ESP_FAIL;
    assert(tft_display_init() == ESP_FAIL && bus_calls == 0);
    gpio_error = ESP_OK;
    bus_error = ESP_ERR_NO_MEM;
    assert(tft_display_init() == ESP_ERR_NO_MEM && bus_calls == 1);
    bus_error = ESP_OK;
    device_error = ESP_ERR_NO_MEM;
    assert(tft_display_init() == ESP_ERR_NO_MEM && bus_calls == 2 && device_calls == 1);
    device_error = ESP_OK;
    queue_fail_at = 1;
    assert(tft_display_init() == ESP_ERR_TIMEOUT && calls == 1);
    assert(tft_display_write_strip(0, pixels) == ESP_ERR_INVALID_STATE);
    reset_log();
    assert(tft_display_init() == ESP_OK);
    assert(bus_calls == 2 && device_calls == 2 && reset_calls == 4 && delays == 5);
    bool rgb666 = false, portrait = false;
    for (int i = 0; i < calls; ++i)
        if (writes[i].dc == 0)
        {
            assert(writes[i].size == 1 && writes[i].bytes[0] != 0x29);
            if (writes[i].bytes[0] == 0x3a)
                rgb666 =
                    writes[i + 1].dc == 1 && writes[i + 1].size == 1 && writes[i + 1].bytes[0] == 6;
            if (writes[i].bytes[0] == 0x36)
                portrait = writes[i + 1].dc == 1 && !(writes[i + 1].bytes[0] & 0x20);
        }
    assert(rgb666 && portrait);

    reset_log();
    for (size_t i = 0; i < sizeof(pixels); ++i)
        pixels[i] = (i % 64) << 2;
    for (int y = 0; y < 160; y += 16)
    {
        int start = calls;
        assert(tft_display_write_strip(y, pixels) == ESP_OK);
        assert(writes[start].dc == 0 && writes[start].bytes[0] == 0x2a);
        const uint8_t columns[] = {0, 2, 0, 129};
        assert(writes[start + 1].dc == 1 && writes[start + 1].size == 4);
        assert(!memcmp(writes[start + 1].bytes, columns, 4));
        assert(writes[start + 2].dc == 0 && writes[start + 2].bytes[0] == 0x2b);
        const uint8_t rows[] = {0, y, 0, y + 15};
        assert(writes[start + 3].dc == 1 && writes[start + 3].size == 4);
        assert(!memcmp(writes[start + 3].bytes, rows, 4));
        assert(writes[start + 4].dc == 0 && writes[start + 4].bytes[0] == 0x2c);
        assert(writes[start + 5].dc == 1 && writes[start + 5].size == sizeof(pixels));
        assert(!memcmp(writes[start + 5].bytes, pixels, sizeof(pixels)));
        assert(calls - start == (y == 144 ? 7 : 6));
    }
    assert(writes[calls - 1].dc == 0 && writes[calls - 1].bytes[0] == 0x29);

    for (int failed = 1; failed <= 7; ++failed)
    {
        reset_log();
        queue_fail_at = failed;
        assert(tft_display_write_strip(144, pixels) == ESP_ERR_TIMEOUT && calls == failed);
        assert(tft_display_write_strip(0, pixels) == ESP_ERR_INVALID_STATE && calls == failed);
        reset_log();
        assert(tft_display_init() == ESP_OK);
    }

    reset_log();
    stall_at = 6;
    stalls_left = 2;
    assert(tft_display_write_strip(0, pixels) == ESP_ERR_TIMEOUT && calls == 6 && active);
    memset(pixels, 0, sizeof(pixels));
    int previous_gpio = gpio_calls;
    assert(tft_display_init() == ESP_ERR_TIMEOUT && calls == 6 && gpio_calls == previous_gpio);
    assert(active && tft_display_write_strip(16, pixels) == ESP_ERR_INVALID_STATE);
    assert(tft_display_init() == ESP_OK && !active);
    assert(bus_calls == 2 && device_calls == 2);

    reset_log();
    stall_at = 1;
    stalls_left = 1;
    assert(tft_display_init() == ESP_ERR_TIMEOUT && active);
    assert(tft_display_init() == ESP_OK && !active);
    puts("TFT SPI geometry, initialization, failures and in-flight lifetime "
         "tests passed");
}
