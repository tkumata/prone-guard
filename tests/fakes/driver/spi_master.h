#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 3
#define SPI_TRANS_USE_TXDATA 4

typedef struct spi_device_t *spi_device_handle_t;
typedef struct
{
    uint32_t flags;
    size_t length;
    union
    {
        const void *tx_buffer;
        uint8_t tx_data[4];
    };
} spi_transaction_t;
typedef struct
{
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz;
} spi_bus_config_t;
typedef struct
{
    int clock_speed_hz, mode, spics_io_num, queue_size;
} spi_device_interface_config_t;
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma);
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config,
                             spi_device_handle_t *device);
esp_err_t spi_device_queue_trans(spi_device_handle_t device, spi_transaction_t *transaction,
                                 uint32_t ticks);
esp_err_t spi_device_get_trans_result(spi_device_handle_t device, spi_transaction_t **transaction,
                                      uint32_t ticks);
