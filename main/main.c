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
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "pose_inference_bridge.h"
#include "prone_detector.h"

// Wi-Fi 設定 (sdkconfig / menuconfig で設定)
#define WIFI_SSID     CONFIG_WIFI_SSID
#define WIFI_PASSWORD CONFIG_WIFI_PASSWORD

// ネットワーク設定 (sdkconfig / menuconfig で設定)
#define STATIC_IP_ADDR    CONFIG_STATIC_IP_ADDR
#define STATIC_GW_ADDR    CONFIG_STATIC_GW_ADDR
#define STATIC_NETMASK    CONFIG_STATIC_NETMASK_ADDR

// システム定数
#define WIFI_RETRY_INTERVAL_MS 5000
#define WIFI_CONNECTED_BIT     BIT0

// 推論タスク設定
#define INFERENCE_TASK_STACK_SIZE  (16 * 1024)
#define INFERENCE_TASK_PRIORITY    5
#define INFERENCE_TASK_CORE        1

// Freenove ESP32-S3 WROOM CAM (OV2640) ピン定義
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  15
#define CAM_PIN_SIOD  4
#define CAM_PIN_SIOC  5
#define CAM_PIN_D7    16
#define CAM_PIN_D6    17
#define CAM_PIN_D5    18
#define CAM_PIN_D4    12
#define CAM_PIN_D3    10
#define CAM_PIN_D2    8
#define CAM_PIN_D1    9
#define CAM_PIN_D0    11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF  7
#define CAM_PIN_PCLK  13

// カメラ解像度
#define IMAGE_WIDTH   320
#define IMAGE_HEIGHT  240

static const char *TAG = "prone_guard";

// キーポイント名テーブル
static const char *kp_names[17] = {
    "nose", "left_eye", "right_eye", "left_ear", "right_ear",
    "left_shoulder", "right_shoulder", "left_elbow", "right_elbow",
    "left_wrist", "right_wrist", "left_hip", "right_hip",
    "left_knee", "right_knee", "left_ankle", "right_ankle"
};

// システム状態
typedef enum {
    SYSTEM_STATE_BOOT = 0,
    SYSTEM_STATE_WIFI_CONNECTING,
    SYSTEM_STATE_READY,
    SYSTEM_STATE_MONITORING,
    SYSTEM_STATE_ALERT,
    SYSTEM_STATE_FAULT_CAMERA,
    SYSTEM_STATE_FAULT_INFERENCE,
} system_state_t;

// グローバル変数
static EventGroupHandle_t s_wifi_event_group;
static httpd_handle_t s_http_server;
static system_state_t s_system_state = SYSTEM_STATE_BOOT;
static bool s_wifi_connected;
static int64_t s_last_wifi_retry_ms;
static esp_timer_handle_t s_wifi_retry_timer;
static bool s_camera_ready;
static pose_inference_status_t s_inference_status = POSE_INFERENCE_STATUS_NOT_READY;

// 最新の推論・判定結果 (推論タスクが書き込み、httpd が読み取り)
static SemaphoreHandle_t s_result_mutex;
static pose_result_t s_last_pose;
static prone_result_t s_last_prone;
static int64_t s_person_missing_started_ms = -1;



// ---- 状態管理 ----

static const char *state_to_string(system_state_t state)
{
    switch (state) {
    case SYSTEM_STATE_BOOT:             return "BOOT";
    case SYSTEM_STATE_WIFI_CONNECTING:  return "WIFI_CONNECTING";
    case SYSTEM_STATE_READY:            return "READY";
    case SYSTEM_STATE_MONITORING:       return "MONITORING";
    case SYSTEM_STATE_ALERT:            return "ALERT";
    case SYSTEM_STATE_FAULT_CAMERA:     return "FAULT_CAMERA";
    case SYSTEM_STATE_FAULT_INFERENCE:  return "FAULT_INFERENCE";
    default:                            return "UNKNOWN";
    }
}

static const char *inference_status_to_string(pose_inference_status_t status)
{
    switch (status) {
    case POSE_INFERENCE_STATUS_NOT_READY:    return "not_ready";
    case POSE_INFERENCE_STATUS_OK:           return "ok";
    case POSE_INFERENCE_STATUS_MODEL_MISSING: return "model_missing";
    case POSE_INFERENCE_STATUS_FAULT:        return "fault";
    default:                                 return "unknown";
    }
}

static const char *prone_status_to_string(prone_status_t status)
{
    switch (status) {
    case PRONE_STATUS_UNKNOWN:    return "unknown";
    case PRONE_STATUS_NOT_PRONE:  return "not_prone";
    case PRONE_STATUS_PRONE:      return "prone";
    default:                      return "unknown";
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

// ---- 推論タスク (専用スレッド) ----

static void inference_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "推論タスク開始 (CPU %d)", xPortGetCoreID());

    while (true) {
        if (!s_camera_ready) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // カメラフレーム取得
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGW(TAG, "カメラフレーム取得失敗");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }



        // 推論実行
        pose_result_t pose = {0};
        esp_err_t err = pose_inference_run_jpeg(fb->buf, fb->len, &pose);

        // フレームバッファを即座に返却 (推論結果はコピー済み)
        esp_camera_fb_return(fb);

        // WDT 対策: 推論後に明示的に譲る
        vTaskDelay(pdMS_TO_TICKS(10));

        if (err != ESP_OK) {
            s_inference_status = POSE_INFERENCE_STATUS_FAULT;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        s_inference_status = POSE_INFERENCE_STATUS_OK;
        int64_t now_ms = esp_timer_get_time() / 1000;

        // 結果を排他的に更新
        if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_last_pose = pose;

            if (pose.detected) {
                s_person_missing_started_ms = -1;

                // うつ伏せ判定
                prone_result_t prone = prone_check(pose.keypoints, pose.bbox, IMAGE_WIDTH, IMAGE_HEIGHT);
                s_last_prone = prone;

                // 状態遷移
                if (prone.status == PRONE_STATUS_PRONE) {
                    if (s_system_state == SYSTEM_STATE_MONITORING) {
                        set_system_state(SYSTEM_STATE_ALERT);
                    }
                } else if (prone.status == PRONE_STATUS_NOT_PRONE) {
                    if (s_system_state == SYSTEM_STATE_ALERT) {
                        set_system_state(SYSTEM_STATE_MONITORING);
                    }
                }

                // FAULT_INFERENCE からの復帰
                if (s_system_state == SYSTEM_STATE_FAULT_INFERENCE && s_camera_ready) {
                    set_system_state(SYSTEM_STATE_MONITORING);
                }
            } else {
                // 人物未検出
                s_last_prone.status = PRONE_STATUS_UNKNOWN;
                s_last_prone.score = 0.0f;
                s_last_prone.held_ms = 0;

                if (s_person_missing_started_ms < 0) {
                    s_person_missing_started_ms = now_ms;
                }
                if ((now_ms - s_person_missing_started_ms) >= PERSON_MISSING_FAULT_MS) {
                    if (s_system_state != SYSTEM_STATE_FAULT_CAMERA) {
                        set_system_state(SYSTEM_STATE_FAULT_INFERENCE);
                    }
                }
            }

            xSemaphoreGive(s_result_mutex);
        }

        // 定期ログ (推論結果)
        static int64_t s_last_log_ms = 0;
        if (now_ms - s_last_log_ms >= 2000) {
            s_last_log_ms = now_ms;
            ESP_LOGI(TAG,
                     "inference: detected=%d score=%.2f prone=%.2f status=%s state=%s",
                     pose.detected ? 1 : 0,
                     (double)pose.score,
                     (double)s_last_prone.score,
                     prone_status_to_string(s_last_prone.status),
                     state_to_string(s_system_state));
        }

        // 次のフレームまで少し待機 (TWDT 対策)
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---- HTTP ハンドラ ----

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char html[] =
        "<!doctype html>"
        "<html><head><meta charset=\"utf-8\"><title>うつ伏せ検知</title>"
        "<style>"
        "body{background:#1a1a2e;color:#eee;font-family:'Segoe UI',system-ui,sans-serif;margin:0;padding:20px;}"
        "h1{color:#e94560;margin-bottom:10px;}"
        "#wrap{position:relative;width:320px;height:240px;display:inline-block;border:2px solid #0f3460;border-radius:8px;overflow:hidden;background:#000;}"
        "#snap{width:320px;height:240px;display:block;}"
        "canvas{position:absolute;top:0;left:0;pointer-events:none;}"
        "#status{margin-top:12px;padding:10px;background:#16213e;border-radius:8px;font-size:14px;max-width:320px;}"
        "#status.alert{border:2px solid #e94560;animation:pulse 1s infinite;}"
        "@keyframes pulse{0%,100%{opacity:1;}50%{opacity:0.6;}}"
        ".badge{display:inline-block;padding:2px 8px;border-radius:4px;font-weight:bold;}"
        ".badge-ok{background:#0a8754;}.badge-alert{background:#e94560;}.badge-warn{background:#f0a500;}"
        "a{color:#4ea8de;}"
        "</style>"
        "</head>"
        "<body>"
        "<h1>うつ伏せ検知モニター</h1>"
        "<div id=\"wrap\">"
        "<img id=\"snap\" alt=\"snapshot\" width=\"320\" height=\"240\">"
        "<canvas id=\"cv\" width=\"320\" height=\"240\"></canvas>"
        "</div>"
        "<div id=\"status\">読み込み中...</div>"
        "<p><a href=\"/health\">/health</a> | <a href=\"/keypoints\">/keypoints</a></p>"
        "<script>"
        "const img=document.getElementById('snap');"
        "const cv=document.getElementById('cv');"
        "const ctx=cv.getContext('2d');"
        "const st=document.getElementById('status');"
        /* キーポイントの描画色 */
        "const colors=['#e94560','#f0a500','#f0a500','#4ea8de','#4ea8de',"
        "'#0a8754','#0a8754','#2ec4b6','#2ec4b6','#e9c46a','#e9c46a',"
        "'#e76f51','#e76f51','#264653','#264653','#6a0572','#6a0572'];"
        /* スケルトン接続定義 */
        "const skel=[[0,1],[0,2],[1,3],[2,4],[5,6],[5,7],[7,9],[6,8],[8,10],[5,11],[6,12],[11,12],[11,13],[13,15],[12,14],[14,16]];"
        /* 定期更新: スナップショット + キーポイント */
        "async function update(){"
        "try{"
        /* スナップショット更新 (キャッシュ回避) */
        "img.src='/snapshot?t='+Date.now();"
        /* キーポイント取得 */
        "const r=await fetch('/keypoints',{cache:'no-store'});"
        "if(!r.ok)return;"
        "const d=await r.json();"
        "ctx.clearRect(0,0,320,240);"
        "if(!d.detected){st.textContent='人物未検出';st.className='';return;}"
        /* バウンディングボックス描画 */
        "ctx.strokeStyle=d.prone.status==='prone'?'#e94560':'#0a8754';"
        "ctx.lineWidth=2;"
        "const b=d.bbox;"
        "ctx.strokeRect(b[0],b[1],b[2]-b[0],b[3]-b[1]);"
        /* スケルトン描画 */
        "const kp=d.keypoints;"
        "ctx.lineWidth=1.5;ctx.strokeStyle='rgba(255,255,255,0.5)';"
        "for(const[i,j]of skel){"
        "const a=kp[i],bb=kp[j];"
        "if((a.x||a.y)&&(bb.x||bb.y)){"
        "ctx.beginPath();ctx.moveTo(a.x,a.y);ctx.lineTo(bb.x,bb.y);ctx.stroke();"
        "}}"
        /* キーポイント描画 */
        "for(let i=0;i<kp.length;i++){"
        "const p=kp[i];"
        "if(!p.x&&!p.y)continue;"
        "ctx.fillStyle=colors[i]||'#fff';"
        "ctx.beginPath();ctx.arc(p.x,p.y,3,0,Math.PI*2);ctx.fill();"
        "}"
        /* ステータス表示 */
        "let badge='';"
        "if(d.prone.status==='prone'){"
        "badge='<span class=\"badge badge-alert\">ALERT</span>';"
        "st.className='status alert';"
        "}else{"
        "badge='<span class=\"badge badge-ok\">OK</span>';"
        "st.className='status';"
        "}"
        "st.innerHTML=badge+' スコア: '+d.prone.score.toFixed(2)+' | ホールド: '+d.prone.held_ms+'ms | 信頼度: '+d.score.toFixed(2);"
        "}catch(e){}"
        "}"
        "setInterval(update,3000);"
        "update();"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t snapshot_get_handler(httpd_req_t *req)
{
    if (!s_camera_ready) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "camera not ready", HTTPD_RESP_USE_STRLEN);
    }

    // リクエスト時にカメラから直接フレームを取得 (推論とは独立)
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "no frame", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    esp_err_t err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return err;
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    char json[256];

    bool prone_detected = false;
    float prone_score = 0.0f;

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        prone_detected = (s_last_prone.status == PRONE_STATUS_PRONE);
        prone_score = s_last_prone.score;
        xSemaphoreGive(s_result_mutex);
    }

    int written = snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"wifi\":\"%s\",\"camera\":\"%s\",\"inference\":\"%s\","
        "\"prone_detected\":%s,\"prone_score\":%.3f}",
        state_to_string(s_system_state),
        s_wifi_connected ? "connected" : "disconnected",
        s_camera_ready ? "ok" : "fault",
        inference_status_to_string(s_inference_status),
        prone_detected ? "true" : "false",
        (double)prone_score);

    if (written < 0 || written >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t keypoints_get_handler(httpd_req_t *req)
{
    char json[1500];
    int offset = 0;

    pose_result_t pose = {0};
    prone_result_t prone = {0};

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pose = s_last_pose;
        prone = s_last_prone;
        xSemaphoreGive(s_result_mutex);
    }

    offset += snprintf(json + offset, sizeof(json) - offset,
        "{\"detected\":%s,\"score\":%.3f,\"bbox\":[%d,%d,%d,%d],\"keypoints\":[",
        pose.detected ? "true" : "false",
        (double)pose.score,
        pose.bbox[0], pose.bbox[1],
        pose.bbox[2], pose.bbox[3]);

    for (int i = 0; i < 17; i++) {
        if (i > 0) {
            offset += snprintf(json + offset, sizeof(json) - offset, ",");
        }
        offset += snprintf(json + offset, sizeof(json) - offset,
            "{\"name\":\"%s\",\"x\":%d,\"y\":%d}",
            kp_names[i],
            pose.keypoints[2 * i],
            pose.keypoints[2 * i + 1]);

        if (offset >= (int)sizeof(json) - 1) {
            return ESP_FAIL;
        }
    }

    offset += snprintf(json + offset, sizeof(json) - offset,
        "],\"prone\":{\"status\":\"%s\",\"score\":%.3f,\"held_ms\":%lld}}",
        prone_status_to_string(prone.status),
        (double)prone.score,
        (long long)prone.held_ms);

    if (offset < 0 || offset >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, offset);
}

// ---- HTTP サーバ (ポート 80 のみ) ----

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_open_sockets = 4;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP サーバ起動失敗: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t snapshot_uri = {
        .uri = "/snapshot",
        .method = HTTP_GET,
        .handler = snapshot_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t keypoints_uri = {
        .uri = "/keypoints",
        .method = HTTP_GET,
        .handler = keypoints_get_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_http_server, &root_uri);
    httpd_register_uri_handler(s_http_server, &snapshot_uri);
    httpd_register_uri_handler(s_http_server, &health_uri);
    httpd_register_uri_handler(s_http_server, &keypoints_uri);

    ESP_LOGI(TAG, "HTTP サーバ起動 (port 80)");
    return ESP_OK;
}

// ---- Wi-Fi ----

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
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    // 固定 IP 設定
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_str_to_ip4(STATIC_IP_ADDR, &ip_info.ip);
    esp_netif_str_to_ip4(STATIC_GW_ADDR, &ip_info.gw);
    esp_netif_str_to_ip4(STATIC_NETMASK, &ip_info.netmask);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &ip_info));
    ESP_LOGI(TAG, "固定 IP 設定: %s", STATIC_IP_ADDR);

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

// ---- カメラ ----

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
    ESP_LOGI(TAG, "カメラ初期化完了 (QVGA 320x240)");
    return ESP_OK;
}

// ---- メインエントリ ----

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());
    set_system_state(SYSTEM_STATE_BOOT);

    // ミューテックス初期化
    s_result_mutex = xSemaphoreCreateMutex();
    assert(s_result_mutex != NULL);

    ESP_ERROR_CHECK(start_wifi_sta());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        esp_err_t cam_err = init_camera();
        if (cam_err != ESP_OK) {
            ESP_LOGW(TAG, "カメラ未準備");
        }

        esp_err_t infer_err = pose_inference_init();
        if (infer_err != ESP_OK) {
            s_inference_status = pose_inference_get_status();
            ESP_LOGW(TAG, "推論初期化未完了: %s", esp_err_to_name(infer_err));
        } else {
            s_inference_status = POSE_INFERENCE_STATUS_OK;
        }

        ESP_ERROR_CHECK(start_http_server());

        if (s_camera_ready) {
            set_system_state(SYSTEM_STATE_MONITORING);
        }

        // 推論タスクを専用コアで起動
        xTaskCreatePinnedToCore(
            inference_task,
            "inference",
            INFERENCE_TASK_STACK_SIZE,
            NULL,
            INFERENCE_TASK_PRIORITY,
            NULL,
            INFERENCE_TASK_CORE
        );
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
