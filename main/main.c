#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"

void app_main(void)
{
    ESP_LOGI("PSRAM", "init=%d", esp_psram_is_initialized());
    ESP_LOGI("PSRAM", "total=%u", (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI("PSRAM", "free=%u",  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
