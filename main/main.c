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
#include "prone_inference_bridge.h"

#define WIFI_SSID "Rakuten-EBBB"
#define WIFI_PASSWORD "8X62VENBT2"

#define WIFI_RETRY_INTERVAL_MS 5000
#define WIFI_CONNECTED_BIT BIT0
#define FRAME_INTERVAL_MS 500
#define FACE_CONFIDENCE_TH 0.50f
#define FACE_DETECT_HOLD_MS (1500)
#define FACE_MISS_FAULT_MS (3 * 1000)

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

typedef enum {
    INFERENCE_STATUS_NOT_READY = 0,
    INFERENCE_STATUS_OK,
    INFERENCE_STATUS_MODEL_MISSING,
    INFERENCE_STATUS_FAULT,
} inference_status_t;

static EventGroupHandle_t s_wifi_event_group;
static httpd_handle_t s_http_server;
static httpd_handle_t s_stream_http_server;
static system_state_t s_system_state = SYSTEM_STATE_BOOT;
static bool s_wifi_connected;
static int64_t s_last_wifi_retry_ms;
static esp_timer_handle_t s_wifi_retry_timer;
static bool s_camera_ready;
static inference_status_t s_inference_status = INFERENCE_STATUS_NOT_READY;
static bool s_is_face_detected;
static float s_face_confidence;
static int64_t s_face_missing_started_ms = -1;
static int64_t s_last_face_seen_ms = -1;
static float s_last_face_confidence;
static int64_t s_last_inference_ms;
static int64_t s_last_face_log_ms;
static prone_face_box_t s_last_face_box;

static esp_err_t run_prone_inference(camera_fb_t *fb, bool *is_face_detected, float *confidence);
static void update_face_monitor(bool is_face_detected, float confidence);
static esp_err_t face_box_get_handler(httpd_req_t *req);

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

static const char *inference_status_to_string(inference_status_t status)
{
    switch (status) {
    case INFERENCE_STATUS_NOT_READY:
        return "not_ready";
    case INFERENCE_STATUS_OK:
        return "ok";
    case INFERENCE_STATUS_MODEL_MISSING:
        return "model_missing";
    case INFERENCE_STATUS_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

static inference_status_t from_bridge_status(prone_inference_status_t status)
{
    switch (status) {
    case PRONE_INFERENCE_STATUS_NOT_READY:
        return INFERENCE_STATUS_NOT_READY;
    case PRONE_INFERENCE_STATUS_OK:
        return INFERENCE_STATUS_OK;
    case PRONE_INFERENCE_STATUS_MODEL_MISSING:
        return INFERENCE_STATUS_MODEL_MISSING;
    case PRONE_INFERENCE_STATUS_FAULT:
        return INFERENCE_STATUS_FAULT;
    default:
        return INFERENCE_STATUS_FAULT;
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
        "<html><head><meta charset=\"utf-8\"><title>顔認識監視</title>"
        "<style>"
        "#wrap{position:relative;width:320px;height:240px;display:inline-block;}"
        "#stream{width:320px;height:240px;display:block;}"
        "#face-box{position:absolute;border:2px solid red;display:none;pointer-events:none;box-sizing:border-box;}"
        "</style>"
        "</head>"
        "<body>"
        "<h1>顔認識監視</h1>"
        "<div id=\"wrap\">"
        "<img src=\"http://\" onerror=\"this.outerHTML='<p>stream 読み込み失敗</p>'\" id=\"stream\" alt=\"stream\" width=\"320\" height=\"240\">"
        "<div id=\"face-box\"></div>"
        "</div>"
        "<p>状態確認: <a href=\"/health\">/health</a></p>"
        "<p>枠座標: <a href=\"/face_box\">/face_box</a></p>"
        "<script>"
        "const img=document.getElementById('stream');"
        "const box=document.getElementById('face-box');"
        "img.src='http://'+location.hostname+':81/stream';"
        "const clamp=(v,min,max)=>Math.min(max,Math.max(min,v));"
        "setInterval(async()=>{"
        "try{"
        "const r=await fetch('/face_box',{cache:'no-store'});"
        "if(!r.ok){box.style.display='none';return;}"
        "const d=await r.json();"
        "if(!d.detected){box.style.display='none';return;}"
        "const x0=clamp(d.x0,0,319),y0=clamp(d.y0,0,239),x1=clamp(d.x1,0,319),y1=clamp(d.y1,0,239);"
        "if(x1<=x0||y1<=y0){box.style.display='none';return;}"
        "box.style.left=x0+'px';box.style.top=y0+'px';"
        "box.style.width=(x1-x0)+'px';box.style.height=(y1-y0)+'px';"
        "box.style.display='block';"
        "}catch(e){box.style.display='none';}"
        "},200);"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    char json[256];
    const char *wifi_status = s_wifi_connected ? "connected" : "disconnected";
    const char *camera_status = s_camera_ready ? "ok" : "fault";
    const char *inference_status = inference_status_to_string(s_inference_status);

    int written = snprintf(json,
                           sizeof(json),
                           "{\"state\":\"%s\",\"wifi\":\"%s\",\"camera\":\"%s\",\"inference\":\"%s\","
                           "\"face_detected\":%s,\"face_confidence\":%.3f}",
                           state_to_string(s_system_state),
                           wifi_status,
                           camera_status,
                           inference_status,
                           s_is_face_detected ? "true" : "false",
                           (double)s_face_confidence);
    if (written < 0 || written >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t face_box_get_handler(httpd_req_t *req)
{
    char json[192];
    prone_face_box_t box = s_last_face_box;
    bool detected = s_is_face_detected && box.valid;
    if (detected) {
        if (box.x0 < 0) {
            box.x0 = 0;
        }
        if (box.y0 < 0) {
            box.y0 = 0;
        }
        if (box.x1 > 319) {
            box.x1 = 319;
        }
        if (box.y1 > 239) {
            box.y1 = 239;
        }
        if (box.x1 <= box.x0 || box.y1 <= box.y0) {
            detected = false;
        }
    }

    int written = snprintf(json,
                           sizeof(json),
                           "{\"detected\":%s,\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d,\"confidence\":%.3f}",
                           detected ? "true" : "false",
                           detected ? box.x0 : -1,
                           detected ? box.y0 : -1,
                           detected ? box.x1 : -1,
                           detected ? box.y1 : -1,
                           (double)(detected ? box.confidence : 0.0f));
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

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_last_inference_ms >= FRAME_INTERVAL_MS) {
            s_last_inference_ms = now_ms;
            esp_err_t infer_err = run_prone_inference(fb, &s_is_face_detected, &s_face_confidence);
            if (infer_err == ESP_OK) {
                s_inference_status = INFERENCE_STATUS_OK;
            } else if (infer_err != ESP_ERR_NOT_FOUND) {
                s_inference_status = INFERENCE_STATUS_FAULT;
            }
            update_face_monitor(s_is_face_detected, s_face_confidence);
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

static esp_err_t run_prone_inference(camera_fb_t *fb, bool *is_face_detected, float *confidence)
{
    if (fb == NULL || is_face_detected == NULL || confidence == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = prone_inference_run_jpeg(fb->buf, fb->len, is_face_detected, confidence);
    if (err == ESP_OK) {
        prone_inference_get_last_face_box(&s_last_face_box);
    } else {
        s_last_face_box.valid = false;
    }
    s_inference_status = from_bridge_status(prone_inference_get_status());
    return err;
}

static void update_face_monitor(bool is_face_detected, float confidence)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool raw_face_ok = is_face_detected && (confidence >= FACE_CONFIDENCE_TH);

    if (raw_face_ok) {
        s_last_face_seen_ms = now_ms;
        s_last_face_confidence = confidence;
    }

    bool face_ok = raw_face_ok;
    if (!face_ok && s_last_face_seen_ms >= 0 && (now_ms - s_last_face_seen_ms) <= FACE_DETECT_HOLD_MS) {
        face_ok = true;
        is_face_detected = true;
        confidence = s_last_face_confidence;
    }

    s_is_face_detected = face_ok;
    s_face_confidence = face_ok ? confidence : 0.0f;

    if (now_ms - s_last_face_log_ms >= 1000) {
        s_last_face_log_ms = now_ms;
        ESP_LOGI(TAG,
                 "face monitor: detected=%d confidence=%.3f raw_ok=%d hold_ms=%d threshold=%.2f state=%s",
                 s_is_face_detected ? 1 : 0,
                 (double)s_face_confidence,
                 raw_face_ok ? 1 : 0,
                 (int)FACE_DETECT_HOLD_MS,
                 (double)FACE_CONFIDENCE_TH,
                 state_to_string(s_system_state));
    }

    if (face_ok) {
        s_face_missing_started_ms = -1;
        if (s_system_state == SYSTEM_STATE_FAULT_INFERENCE && s_camera_ready) {
            set_system_state(SYSTEM_STATE_MONITORING);
        }
        return;
    }

    if (s_face_missing_started_ms < 0) {
        s_face_missing_started_ms = now_ms;
    }

    if ((now_ms - s_face_missing_started_ms) >= FACE_MISS_FAULT_MS) {
        s_inference_status = INFERENCE_STATUS_FAULT;
        if (s_system_state != SYSTEM_STATE_FAULT_CAMERA) {
            set_system_state(SYSTEM_STATE_FAULT_INFERENCE);
        }
    }
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

    const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t face_box_uri = {
        .uri = "/face_box",
        .method = HTTP_GET,
        .handler = face_box_get_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_http_server, &root_uri);
    httpd_register_uri_handler(s_http_server, &health_uri);
    httpd_register_uri_handler(s_http_server, &face_box_uri);

    ESP_LOGI(TAG, "HTTP サーバ開始");
    return ESP_OK;
}

static esp_err_t start_stream_http_server(void)
{
    if (s_stream_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.ctrl_port = 32769;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_stream_http_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ストリームサーバ開始失敗: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_stream_http_server, &stream_uri);
    ESP_LOGI(TAG, "ストリームサーバ開始 port=81");
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
    s_last_inference_ms = 0;

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

        esp_err_t infer_init_err = prone_inference_init();
        if (infer_init_err != ESP_OK) {
            s_inference_status = from_bridge_status(prone_inference_get_status());
            ESP_LOGW(TAG, "推論初期化未完了: %s", esp_err_to_name(infer_init_err));
        } else {
            s_inference_status = INFERENCE_STATUS_OK;
        }

        ESP_ERROR_CHECK(start_http_server());
        ESP_ERROR_CHECK(start_stream_http_server());
        if (s_camera_ready) {
            set_system_state(SYSTEM_STATE_MONITORING);
        }
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
