#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#define WIFI_SSID "Rakuten-EBBB"
#define WIFI_PASSWORD "8X62VENBT2"

#define WIFI_RETRY_INTERVAL_MS 5000
#define WIFI_CONNECTED_BIT BIT0

// Freenove ESP32-S3 WROOM CAM (OV2640) 想定ピン定義
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D7 16
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2 8
#define CAM_PIN_D1 9
#define CAM_PIN_D0 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

static const char *TAG = "prone_guard";

typedef enum {
    SYSTEM_STATE_BOOT = 0,
    SYSTEM_STATE_WIFI_CONNECTING,
    SYSTEM_STATE_READY,
    SYSTEM_STATE_MONITORING,
    SYSTEM_STATE_ALERT,
    SYSTEM_STATE_FAULT_CAMERA,
    SYSTEM_STATE_FAULT_INFERENCE,
} system_state_t;

static EventGroupHandle_t s_wifi_event_group;
static httpd_handle_t s_http_server;
static system_state_t s_system_state = SYSTEM_STATE_BOOT;
static bool s_wifi_connected;
static int64_t s_last_wifi_retry_ms;
static esp_timer_handle_t s_wifi_retry_timer;
static bool s_camera_ready;

static const char *state_to_string(system_state_t state)
{
    switch (state) {
    case SYSTEM_STATE_BOOT:
        return "BOOT";
    case SYSTEM_STATE_WIFI_CONNECTING:
        return "WIFI_CONNECTING";
    case SYSTEM_STATE_READY:
        return "READY";
    case SYSTEM_STATE_MONITORING:
        return "MONITORING";
    case SYSTEM_STATE_ALERT:
        return "ALERT";
    case SYSTEM_STATE_FAULT_CAMERA:
        return "FAULT_CAMERA";
    case SYSTEM_STATE_FAULT_INFERENCE:
        return "FAULT_INFERENCE";
    default:
        return "UNKNOWN";
    }
}

static void set_system_state(system_state_t next_state)
{
    if (s_system_state == next_state) {
        return;
    }

    ESP_LOGI(TAG, "状態遷移: %s -> %s", state_to_string(s_system_state), state_to_string(next_state));
    s_system_state = next_state;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char html[] =
        "<!doctype html>"
        "<html><head><meta charset=\"utf-8\"><title>うつ伏せ監視</title></head>"
        "<body>"
        "<h1>うつ伏せ監視</h1>"
        "<img src=\"/stream\" alt=\"stream\" width=\"320\" height=\"240\">"
        "<p>状態確認: <a href=\"/health\">/health</a></p>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    char json[160];
    const char *wifi_status = s_wifi_connected ? "connected" : "disconnected";
    const char *camera_status = s_camera_ready ? "ok" : "fault";

    int written = snprintf(json,
                           sizeof(json),
                           "{\"state\":\"%s\",\"wifi\":\"%s\",\"camera\":\"%s\",\"inference\":\"not_ready\"}",
                           state_to_string(s_system_state),
                           wifi_status,
                           camera_status);
    if (written < 0 || written >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    if (!s_camera_ready) {
        static const char message[] = "camera not ready";
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
    }

    static const char *stream_content_type = "multipart/x-mixed-replace;boundary=frame";
    static const char *stream_boundary = "\r\n--frame\r\n";
    char part_header[64];

    httpd_resp_set_type(req, stream_content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGW(TAG, "カメラフレーム取得失敗");
            continue;
        }

        int hlen = snprintf(part_header,
                            sizeof(part_header),
                            "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                            (unsigned)fb->len);
        if (hlen <= 0 || hlen >= (int)sizeof(part_header)) {
            esp_camera_fb_return(fb);
            return ESP_FAIL;
        }

        esp_err_t err = httpd_resp_send_chunk(req, stream_boundary, strlen(stream_boundary));
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, part_header, hlen);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, "\r\n", 2);
        }

        esp_camera_fb_return(fb);
        if (err != ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }

    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP サーバ開始失敗: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };

    const httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_get_handler,
        .user_ctx = NULL,
    };

    const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_http_server, &root_uri);
    httpd_register_uri_handler(s_http_server, &stream_uri);
    httpd_register_uri_handler(s_http_server, &health_uri);

    ESP_LOGI(TAG, "HTTP サーバ開始");
    return ESP_OK;
}

static void wifi_retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_wifi_connected) {
        return;
    }

    s_last_wifi_retry_ms = esp_timer_get_time() / 1000;
    ESP_LOGW(TAG, "Wi-Fi 再接続試行");
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        set_system_state(SYSTEM_STATE_WIFI_CONNECTING);
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        s_wifi_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        set_system_state(SYSTEM_STATE_WIFI_CONNECTING);

        int64_t elapsed_ms = now_ms - s_last_wifi_retry_ms;
        if (elapsed_ms >= WIFI_RETRY_INTERVAL_MS) {
            wifi_retry_timer_cb(NULL);
            return;
        }

        int64_t wait_ms = WIFI_RETRY_INTERVAL_MS - elapsed_ms;
        esp_err_t stop_err = esp_timer_stop(s_wifi_retry_timer);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(stop_err);
        }
        ESP_ERROR_CHECK(esp_timer_start_once(s_wifi_retry_timer, wait_ms * 1000));
        ESP_LOGW(TAG, "Wi-Fi 切断。%lld ms 後に再接続", (long long)wait_ms);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        set_system_state(SYSTEM_STATE_READY);
        ESP_LOGI(TAG, "Wi-Fi 接続完了");
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t start_wifi_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    s_last_wifi_retry_ms = 0;

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_retry_timer_cb,
        .name = "wifi_retry_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_wifi_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

static esp_err_t init_camera(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        s_camera_ready = false;
        set_system_state(SYSTEM_STATE_FAULT_CAMERA);
        ESP_LOGE(TAG, "カメラ初期化失敗: %s", esp_err_to_name(err));
        return err;
    }

    s_camera_ready = true;
    ESP_LOGI(TAG, "カメラ初期化完了");
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());
    set_system_state(SYSTEM_STATE_BOOT);

    if (strcmp(WIFI_SSID, "YOUR_SSID") == 0 || strcmp(WIFI_PASSWORD, "YOUR_PASSWORD") == 0) {
        ESP_LOGW(TAG, "WIFI_SSID / WIFI_PASSWORD を実環境の値に変更してください");
    }

    ESP_ERROR_CHECK(start_wifi_sta());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        esp_err_t cam_err = init_camera();
        if (cam_err != ESP_OK) {
            ESP_LOGW(TAG, "カメラが未準備のため /stream は 503 を返します");
        }
        ESP_ERROR_CHECK(start_http_server());
        if (s_camera_ready) {
            set_system_state(SYSTEM_STATE_MONITORING);
        }
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
