#include "pairing_usb.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocomm_security2.h"
#include "psa/crypto.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "test_credentials.h"
#include "cJSON.h"

static unsigned expected_request;

static esp_err_t command(uint32_t session_id, const uint8_t *data, ssize_t size,
                          uint8_t **out, ssize_t *out_size, void *context)
{
    (void)session_id;
    (void)context;
    if (size <= 0 || size > 16384 || memchr(data, 0, size))
        return ESP_ERR_INVALID_ARG;
    const char *end = NULL;
    cJSON *input = cJSON_ParseWithLengthOpts((const char *)data, size, &end, false);
    if (!cJSON_IsObject(input) || end != (const char *)data + size)
    {
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *request = cJSON_GetObjectItemCaseSensitive(input, "request");
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(input, "op");
    bool valid = cJSON_IsNumber(request) &&
                 request->valuedouble == expected_request && cJSON_IsString(op);
    const cJSON *field;
    cJSON_ArrayForEach(field, input)
    {
        if (strcmp(field->string, "op") && strcmp(field->string, "request") && strcmp(field->string, "utc"))
            valid = false;
        for (const cJSON *other = field->next; other; other = other->next)
            if (!strcmp(field->string, other->string))
                valid = false;
    }
    if (valid && !strcmp(op->valuestring, "time"))
    {
        const cJSON *utc = cJSON_GetObjectItemCaseSensitive(input, "utc");
        valid = cJSON_IsNumber(utc) && utc->valuedouble >= 1704067200 &&
                utc->valuedouble <= 4102444800 && floor(utc->valuedouble) == utc->valuedouble;
        if (valid)
        {
            struct timeval now = {.tv_sec = (time_t)utc->valuedouble};
            valid = settimeofday(&now, NULL) == 0;
        }
    }
    else if (valid)
        valid = !strcmp(op->valuestring, "identity");
    cJSON_Delete(input);
    if (!valid)
        return ESP_ERR_INVALID_ARG;
    char response[160];
    int length = snprintf(response, sizeof(response),
                          "{\"ok\":true,\"request\":%u,\"device_id\":\"%s\",\"utc\":%lld}",
                          expected_request++, TEST_DEVICE_ID, (long long)time(NULL));
    if (length <= 0 || length >= sizeof(response))
        return ESP_FAIL;
    *out = malloc(length);
    if (!*out)
        return ESP_ERR_NO_MEM;
    memcpy(*out, response, length);
    *out_size = length;
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(pairing_usb_init());
    gpio_config_t button = {.pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT,
                            .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&button));
    ESP_LOGW("pairing_test", "TEST FIRMWARE ONLY: public credentials, no manufacturing or shipping protection");
    while (true)
    {
        int64_t pressed = esp_timer_get_time();
        while (!gpio_get_level(GPIO_NUM_0))
            vTaskDelay(pdMS_TO_TICKS(20));
        if (esp_timer_get_time() - pressed >= 3000000)
        {
            protocomm_t *protocol = protocomm_new();
            ESP_ERROR_CHECK(protocol ? ESP_OK : ESP_ERR_NO_MEM);
            protocomm_security2_params_t credentials = {.salt = (const char *)test_salt, .salt_len = sizeof(test_salt),
                .verifier = (const char *)test_verifier, .verifier_len = sizeof(test_verifier)};
            ESP_ERROR_CHECK(protocomm_set_security(protocol, "pairing-session", &protocomm_security2, &credentials));
            ESP_ERROR_CHECK(protocomm_add_endpoint(protocol, "pairing-command", command, NULL));
            expected_request = 0;
            esp_err_t err = pairing_usb_run(protocol, esp_random(), esp_timer_get_time() + 120000000);
            protocomm_delete(protocol);
            ESP_LOGI("pairing_test", "session closed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
