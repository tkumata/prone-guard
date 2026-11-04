#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef int gpio_num_t;
#define GPIO_NUM_1 1
#define GPIO_NUM_2 2
#define GPIO_NUM_38 38
#define GPIO_NUM_41 41
#define GPIO_NUM_42 42
#define GPIO_MODE_OUTPUT 2
#define GPIO_INTR_DISABLE 0
typedef struct
{
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
