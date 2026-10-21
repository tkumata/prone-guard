#include "jev_client.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/x509_crt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if !defined(CONFIG_MBEDTLS_HAVE_TIME_DATE)
#error "Enable MBEDTLS_HAVE_TIME_DATE for verified Jev HTTPS"
#endif

#define RESPONSE_MAX 8192
#define BOUNDARY "prone-guard-7a67ca37401c"

extern const char gts_root_r4_pem_start[] asm("_binary_gts_root_r4_pem_start");

typedef struct {
    char data[RESPONSE_MAX + 1];
    size_t len;
    bool overflow;
    atomic_int socket;
    atomic_bool expired;
    SemaphoreHandle_t timeout_done;
    const char *stage;
} response_t;

static void log_request_failure(esp_http_client_handle_t client, esp_err_t err,
                                const response_t *response, jev_status_t status)
{
    const char *tag = "jev_client";
    time_t now = time(NULL);
    struct tm utc;
    char timestamp[32] = "unavailable";
    if (gmtime_r(&now, &utc)) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    }
    ESP_LOGW(tag, "Jev failed: stage=%s status=%s err=%s http_status=%d received=%u utc=%s epoch=%lld",
             response->stage, jev_status_name(status), esp_err_to_name(err),
             esp_http_client_get_status_code(client), (unsigned)response->len,
             timestamp, (long long)now);

    int tls_code = 0;
    int flags = 0;
    esp_err_t tls_err = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &flags);
    ESP_LOGW(tag, "Jev TLS: esp_tls=%s tls_code=0x%x cert_flags=0x%x",
             esp_err_to_name(tls_err), (unsigned)tls_code, (unsigned)flags);
    if (flags) {
        char detail[512];
        int ret = mbedtls_x509_crt_verify_info(detail, sizeof(detail), "  ", (uint32_t)flags);
        if (ret >= 0) ESP_LOGW(tag, "Jev certificate verification: %s", detail);
        else ESP_LOGW(tag, "Jev certificate verification details unavailable: format_error=%d", ret);
    } else {
        // The IDF async transport may not forward handshake errors to this API.
        ESP_LOGW(tag, "Jev certificate verification details unavailable through HTTP client; zero flags do not confirm successful verification");
    }
}

static void expire_request(void *arg)
{
    response_t *response = arg;
    atomic_store(&response->expired, true);
    int socket = atomic_load(&response->socket);
    if (socket >= 0) shutdown(socket, SHUT_RDWR);
    xSemaphoreGive(response->timeout_done);
}

static esp_err_t receive_event(esp_http_client_event_t *event)
{
    response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        response->stage = "send_request";
        int socket = esp_http_client_get_socket(event->client);
        atomic_store(&response->socket, socket);
        if (atomic_load(&response->expired) && socket >= 0) shutdown(socket, SHUT_RDWR);
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER || event->event_id == HTTP_EVENT_ON_DATA) {
        response->stage = "receive_response";
    }
    if (event->event_id == HTTP_EVENT_DISCONNECTED) atomic_store(&response->socket, -1);
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if ((size_t)event->data_len > RESPONSE_MAX - response->len) {
            response->overflow = true;
            int socket = atomic_load(&response->socket);
            if (socket >= 0) shutdown(socket, SHUT_RDWR);
            return ESP_FAIL;
        }
        memcpy(response->data + response->len, event->data, event->data_len);
        response->len += event->data_len;
        response->data[response->len] = 0;
    }
    return ESP_OK;
}

jev_result_t jev_classify(const uint8_t *jpeg, size_t jpeg_len, const char *api_key, float threshold)
{
    jev_result_t result = {.status = JEV_NETWORK_ERROR};
    if (!api_key[0]) return (jev_result_t){.status = JEV_NOT_CONFIGURED};
    static const char first[] = "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"request\"\r\n"
        "Content-Type: application/json\r\n\r\n";
    static const char image[] = "\r\n--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"frame\"; "
        "filename=\"frame.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    static const char end[] = "\r\n--" BOUNDARY "--\r\n";
    size_t json_len = strlen(jev_request_json);
    size_t body_len = sizeof(first)-1 + json_len + sizeof(image)-1 + jpeg_len + sizeof(end)-1;
    char *body = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    response_t *response = calloc(1, sizeof(*response));
    if (!body || !response) goto done;
    response->stage = "connect";
    atomic_init(&response->socket, -1);
    atomic_init(&response->expired, false);
    response->timeout_done = xSemaphoreCreateBinary();
    if (!response->timeout_done) goto done;
    char *p = body;
    memcpy(p, first, sizeof(first)-1); p += sizeof(first)-1;
    memcpy(p, jev_request_json, json_len); p += json_len;
    memcpy(p, image, sizeof(image)-1); p += sizeof(image)-1;
    memcpy(p, jpeg, jpeg_len); p += jpeg_len;
    memcpy(p, end, sizeof(end)-1);
    char authorization[520];
    int auth_len = snprintf(authorization, sizeof(authorization), "Bearer %s", api_key);
    if (auth_len < 0 || auth_len >= sizeof(authorization)) goto done;
    esp_http_client_config_t config = {
        .url = "https://api.typesafe.ai/v1/systemone",
        .method = HTTP_METHOD_POST,
        .timeout_ms = JEV_TIMEOUT_MS,
        .is_async = true,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .cert_pem = gts_root_r4_pem_start,
        .event_handler = receive_event,
        .user_data = response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) goto done;
    response->stage = "configure_request";
    esp_err_t err = esp_http_client_set_header(client, "Authorization", authorization);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "TypeSafe-Danger-Zone-Images", "true");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=" BOUNDARY);
    if (err == ESP_OK) err = esp_http_client_set_post_field(client, body, body_len);
    esp_timer_handle_t timer = NULL;
    const esp_timer_create_args_t timer_args = {.callback = expire_request, .arg = response, .name = "jev_timeout"};
    if (err == ESP_OK) err = esp_timer_create(&timer_args, &timer);
    bool timer_started = false;
    if (err == ESP_OK) {
        err = esp_timer_start_once(timer, (uint64_t)JEV_TIMEOUT_MS * 1000);
        timer_started = err == ESP_OK;
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)JEV_TIMEOUT_MS * 1000;
    if (err == ESP_OK) {
        response->stage = "connect";
        do {
            err = esp_http_client_perform(client);
            if (atomic_load(&response->expired) || esp_timer_get_time() >= deadline) {
                result.status = JEV_TIMEOUT;
                break;
            }
            if (response->overflow) {
                result.status = JEV_INVALID_RESPONSE;
                break;
            }
            if (err != ESP_ERR_HTTP_EAGAIN) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        } while (true);
        if (result.status != JEV_TIMEOUT && result.status != JEV_INVALID_RESPONSE && err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            result = status == 200 && !memchr(response->data, 0, response->len) ? jev_parse_response(response->data, threshold) :
                (jev_result_t){.status = status == 200 ? JEV_INVALID_RESPONSE : JEV_HTTP_ERROR};
        }
    }
    if (timer_started && esp_timer_stop(timer) == ESP_ERR_INVALID_STATE) {
        // Wait for shutdown to finish before the socket can be closed or reused.
        xSemaphoreTake(response->timeout_done, portMAX_DELAY);
    }
    if (timer) esp_timer_delete(timer);
    if (atomic_load(&response->expired)) result.status = JEV_TIMEOUT;
    if (result.status != JEV_OK && result.status != JEV_UNJUDGEABLE) {
        log_request_failure(client, err, response, result.status);
    }
    esp_http_client_cleanup(client);
    memset(authorization, 0, sizeof(authorization));
done:
    if (response && response->timeout_done) vSemaphoreDelete(response->timeout_done);
    free(response);
    heap_caps_free(body);
    return result;
}
