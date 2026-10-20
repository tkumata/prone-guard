#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
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
#include "prone_inference_bridge.h"
#include "monitor_settings.h"
#include "jev_client.h"
#include "esp_sntp.h"

#if !defined(CONFIG_WIFI_SSID) || !defined(CONFIG_WIFI_PASSWORD)
#error "WiFi credentials must be configured via menuconfig (Prone Guard Configuration)"
#endif

// 空文字列チェック。sizeof("") は終端文字を含むため 1
_Static_assert(sizeof(CONFIG_WIFI_SSID) > 1, "CONFIG_WIFI_SSID must not be empty");

// 文字数チェック。WPA要件である8文字以上。sizeof("12345678") は 9
_Static_assert(sizeof(CONFIG_WIFI_PASSWORD) >= 9, "CONFIG_WIFI_PASSWORD must be at least 8 characters long");

#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASSWORD CONFIG_WIFI_PASSWORD

#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

#define WIFI_RETRY_INTERVAL_MS 5000
#define WIFI_CONNECTED_BIT BIT0
#define INFERENCE_INTERVAL_MS 500
#define STREAM_PACING_DELAY_MS 5
#define UI_POLL_INTERVAL_MS 500
#define INFERENCE_JPEG_MAX_BYTES (256 * 1024)
#define FACE_OBSERVATION_HISTORY_SIZE 6
#define FACE_CONFIDENCE_TH 0.35f
#define FACE_DETECT_OK_STREAK_REQUIRED 3
#define FACE_LOST_GRACE_MS (1200)
#define PRONE_LIKE_MISSING_MS_TH (2000)
#define PRONE_LIKE_LONG_MISSING_MS_TH (7000)
#define PRONE_LIKE_PRE_DISAPPEAR_MOVE_TH (24.0f)
#define PRONE_LIKE_AREA_DROP_TH (0.20f)
#define PRONE_LIKE_SCORE_TH (0.70f)
#define PRE_DISAPPEAR_SAMPLE_MAX_GAP_MS (1500)

// ランドマーク方向メトリクスの閾値
#define LANDMARK_MIN_DIST (3)
#define YA_ROTATION_TH (0.30f)
#define VP_DEVIATION_TH (0.12f)
#define FR_FORESHORTEN_TH (0.40f)
#define VP_FRONTAL_CENTER (0.40f)

// prone_like_score の重み (合計 1.2 に緩める: 複数兆候があれば容易に 0.70 を超える)
#define W_MISSING (0.35f)
#define W_MOVE    (0.15f)
#define W_AREA    (0.15f)
#define W_YAW     (0.25f)
#define W_VP      (0.15f)
#define W_FR      (0.15f)

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

typedef enum {
    MONITOR_STATUS_UNKNOWN = 0,
    MONITOR_STATUS_OK,
    MONITOR_STATUS_NG,
} monitor_status_t;

typedef struct {
    float yaw_asym;   // ヨー非対称度 [0,1], -1 = 無効
    float vert_prop;  // 縦比率 [0,1], -1 = 無効
    float fore_ratio; // 短縮比 (0,+inf), -1 = 無効
    bool valid;
} face_orientation_t;

typedef struct {
    int center_x;
    int center_y;
    int area;
    int64_t timestamp_ms;
    bool valid;
    face_orientation_t orientation;
} face_observation_t;

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
static bool s_raw_face_detected;
static bool s_face_detect_confirmed;
static int s_face_detect_streak;
static float s_face_confidence;
static int64_t s_last_face_detected_ms = -1;
static int64_t s_face_missing_started_ms = -1;
static int64_t s_last_face_log_ms;
static prone_face_box_t s_last_face_box;
static monitor_status_t s_monitor_status = MONITOR_STATUS_UNKNOWN;
static bool s_prone_like_detected;
static float s_prone_like_score;
static int s_face_missing_ms;
static float s_pre_disappear_move_px;
static float s_pre_disappear_area_drop;
static float s_pre_disappear_yaw_trend;
static float s_pre_disappear_vp_trend;
static float s_pre_disappear_fr_trend;
static face_observation_t s_face_observation_history[FACE_OBSERVATION_HISTORY_SIZE];
static size_t s_face_observation_count;
static size_t s_face_observation_next_index;
static SemaphoreHandle_t s_inference_frame_mutex;
static uint8_t *s_inference_frame_buf;
static size_t s_inference_frame_len;
static uint32_t s_frame_sequence;
static SemaphoreHandle_t s_state_mutex;
static monitor_settings_t s_settings;
static uint32_t s_settings_generation;
static jev_result_t s_jev_result = {.status = JEV_PENDING};
static TaskHandle_t s_inference_task_handle;

static void update_face_monitor(bool is_face_detected, float confidence);
static esp_err_t face_box_get_handler(httpd_req_t *req);
static void capture_task(void *arg);
static void inference_task(void *arg);
static esp_err_t start_inference_task(void);
static face_orientation_t compute_face_orientation(const prone_face_box_t *box);

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

static const char *monitor_status_to_string(monitor_status_t status)
{
    switch (status) {
    case MONITOR_STATUS_OK:
        return "ok";
    case MONITOR_STATUS_NG:
        return "ng";
    case MONITOR_STATUS_UNKNOWN:
    default:
        return "unknown";
    }
}

// ランドマーク座標から顔の方向メトリクスを算出する (O(1))
static face_orientation_t compute_face_orientation(const prone_face_box_t *box)
{
    face_orientation_t orient = { .yaw_asym = -1.0f, .vert_prop = -1.0f, .fore_ratio = -1.0f, .valid = false };

    if (box == NULL || !box->landmarks_valid) {
        return orient;
    }

    // ランドマーク座標の取得
    float lx = (float)box->landmarks[PRONE_LM_LEFT_EYE * 2];
    float ly = (float)box->landmarks[PRONE_LM_LEFT_EYE * 2 + 1];
    float rx = (float)box->landmarks[PRONE_LM_RIGHT_EYE * 2];
    float ry = (float)box->landmarks[PRONE_LM_RIGHT_EYE * 2 + 1];
    float nx = (float)box->landmarks[PRONE_LM_NOSE * 2];
    float ny = (float)box->landmarks[PRONE_LM_NOSE * 2 + 1];
    float lmx = (float)box->landmarks[PRONE_LM_LEFT_MOUTH * 2];
    float lmy = (float)box->landmarks[PRONE_LM_LEFT_MOUTH * 2 + 1];
    float rmx = (float)box->landmarks[PRONE_LM_RIGHT_MOUTH * 2];
    float rmy = (float)box->landmarks[PRONE_LM_RIGHT_MOUTH * 2 + 1];

    // 今後の拡張（口角の傾きによる回転推定など）で使用するため予約
    (void)lmx;
    (void)rmx;

    // 1. ヨー非対称度 (YA): 鼻から左目/右目までの距離比
    float d_left = sqrtf((nx - lx) * (nx - lx) + (ny - ly) * (ny - ly));
    float d_right = sqrtf((rx - nx) * (rx - nx) + (ry - ny) * (ry - ny));
    float d_max = (d_left > d_right) ? d_left : d_right;
    if (d_max >= (float)LANDMARK_MIN_DIST) {
        float d_diff = (d_left > d_right) ? (d_left - d_right) : (d_right - d_left);
        orient.yaw_asym = d_diff / d_max;
    }

    // 2. 縦比率 (VP): 鼻が目と口の間のどこにあるか
    float eye_mid_y = (ly + ry) / 2.0f;
    float mouth_mid_y = (lmy + rmy) / 2.0f;
    float upper = ny - eye_mid_y;
    float lower = mouth_mid_y - ny;
    float total_height = upper + lower;
    if (total_height >= (float)LANDMARK_MIN_DIST) {
        orient.vert_prop = upper / total_height;
    } else {
        orient.vert_prop = 0.5f; // 縮退ケース: 不定
    }

    // 3. 短縮比 (FR): 目の間隔と顔の縦幅の比率
    float inter_eye = sqrtf((rx - lx) * (rx - lx) + (ry - ly) * (ry - ly));
    float face_height = mouth_mid_y - eye_mid_y;
    if (face_height >= (float)LANDMARK_MIN_DIST) {
        orient.fore_ratio = inter_eye / face_height;
    } else if (face_height > 0.0f) {
        orient.fore_ratio = 2.0f; // 前傾扱い
    } else {
        orient.fore_ratio = 2.0f; // 前傾扱い
    }

    orient.valid = (orient.yaw_asym >= 0.0f);
    return orient;
}

static void update_face_observation(const prone_face_box_t *box, int64_t now_ms)
{
    if (box == NULL || !box->valid) {
        return;
    }

    int width = box->x1 - box->x0;
    int height = box->y1 - box->y0;
    if (width <= 0 || height <= 0) {
        return;
    }

    face_observation_t *observation = &s_face_observation_history[s_face_observation_next_index];
    observation->center_x = (box->x0 + box->x1) / 2;
    observation->center_y = (box->y0 + box->y1) / 2;
    observation->area = width * height;
    observation->timestamp_ms = now_ms;
    observation->valid = true;
    observation->orientation = compute_face_orientation(box);

    s_face_observation_next_index = (s_face_observation_next_index + 1) % FACE_OBSERVATION_HISTORY_SIZE;
    if (s_face_observation_count < FACE_OBSERVATION_HISTORY_SIZE) {
        s_face_observation_count++;
    }
}

static void capture_pre_disappear_metrics(void)
{
    s_pre_disappear_move_px = 0.0f;
    s_pre_disappear_area_drop = 0.0f;
    s_pre_disappear_yaw_trend = 0.0f;
    s_pre_disappear_vp_trend = 0.0f;
    s_pre_disappear_fr_trend = 0.0f;

    if (s_face_observation_count < 2) {
        return;
    }

    size_t latest_index =
        (s_face_observation_next_index + FACE_OBSERVATION_HISTORY_SIZE - 1) % FACE_OBSERVATION_HISTORY_SIZE;
    face_observation_t latest = s_face_observation_history[latest_index];
    if (!latest.valid) {
        return;
    }

    int max_area = latest.area;
    // ランドマーク回転傾向算出用: 有効なサンプル中で最も古い観測を取得
    face_observation_t *oldest_valid = NULL;

    for (size_t offset = 1; offset < s_face_observation_count; ++offset) {
        size_t index =
            (s_face_observation_next_index + FACE_OBSERVATION_HISTORY_SIZE - 1 - offset) % FACE_OBSERVATION_HISTORY_SIZE;
        face_observation_t *observation = &s_face_observation_history[index];
        if (!observation->valid) {
            continue;
        }

        int64_t gap_ms = latest.timestamp_ms - observation->timestamp_ms;
        if (gap_ms <= 0) {
            continue;
        }
        if (gap_ms > PRE_DISAPPEAR_SAMPLE_MAX_GAP_MS) {
            break;
        }

        int dx = latest.center_x - observation->center_x;
        int dy = latest.center_y - observation->center_y;
        float move_px = sqrtf((float)(dx * dx + dy * dy));
        if (move_px > s_pre_disappear_move_px) {
            s_pre_disappear_move_px = move_px;
        }

        if (observation->area > max_area) {
            max_area = observation->area;
        }

        // 最も古い有効観測を記録
        if (observation->orientation.valid) {
            oldest_valid = observation;
        }
    }

    if (max_area > 0 && latest.area < max_area) {
        s_pre_disappear_area_drop = ((float)(max_area - latest.area)) / (float)max_area;
    }

    // ランドマーク回転傾向: 直近 - 最古
    if (oldest_valid != NULL && latest.orientation.valid && oldest_valid->orientation.valid) {
        if (latest.orientation.yaw_asym >= 0.0f && oldest_valid->orientation.yaw_asym >= 0.0f) {
            s_pre_disappear_yaw_trend = latest.orientation.yaw_asym - oldest_valid->orientation.yaw_asym;
        }
        if (latest.orientation.vert_prop >= 0.0f && oldest_valid->orientation.vert_prop >= 0.0f) {
            s_pre_disappear_vp_trend = latest.orientation.vert_prop - oldest_valid->orientation.vert_prop;
        }
        if (latest.orientation.fore_ratio >= 0.0f && oldest_valid->orientation.fore_ratio >= 0.0f) {
            s_pre_disappear_fr_trend = latest.orientation.fore_ratio - oldest_valid->orientation.fore_ratio;
        }
    }
}

// [0,1] にクランプするヘルパー
static inline float clamp01(float x)
{
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static float compute_prone_like_score(int missing_ms, float pre_disappear_move_px, float pre_disappear_area_drop)
{
    // 消失時間: 線形補間 [T_min, T_max] -> [0, 1]
    float f_missing = clamp01(
        ((float)missing_ms - (float)PRONE_LIKE_MISSING_MS_TH)
        / ((float)PRONE_LIKE_LONG_MISSING_MS_TH - (float)PRONE_LIKE_MISSING_MS_TH));

    // 消失前の移動量
    float f_move = clamp01(pre_disappear_move_px / PRONE_LIKE_PRE_DISAPPEAR_MOVE_TH);

    // 消失前の面積縮小率
    float f_area = clamp01(pre_disappear_area_drop / PRONE_LIKE_AREA_DROP_TH);

    // ランドマーク回転傾向（ヨー変化 = 左右どちらかを向いた。絶対値をとる）
    float abs_yaw = s_pre_disappear_yaw_trend < 0 ? -s_pre_disappear_yaw_trend : s_pre_disappear_yaw_trend;
    float f_yaw = clamp01(abs_yaw / YA_ROTATION_TH);

    // ランドマーク回転傾向（VP 偏差 = うつ伏せ方向または仰向け方向への大きな変化。絶対値をとる）
    float abs_vp = s_pre_disappear_vp_trend < 0 ? -s_pre_disappear_vp_trend : s_pre_disappear_vp_trend;
    float f_vp = clamp01(abs_vp / VP_DEVIATION_TH);

    // ランドマーク回転傾向（FR 変化 = 前後への倒れ込み。絶対値をとる）
    float abs_fr = s_pre_disappear_fr_trend < 0 ? -s_pre_disappear_fr_trend : s_pre_disappear_fr_trend;
    float f_fr = clamp01(abs_fr / FR_FORESHORTEN_TH);

    float score = W_MISSING * f_missing
               + W_MOVE * f_move
               + W_AREA * f_area
               + W_YAW * f_yaw
               + W_VP * f_vp
               + W_FR * f_fr;

    return score;
}

static void set_system_state(system_state_t next_state)
{
    if (s_system_state == next_state) {
        return;
    }

    ESP_LOGI(TAG, "状態遷移: %s -> %s", state_to_string(s_system_state), state_to_string(next_state));
    s_system_state = next_state;
}

static bool get_reportable_face_box(prone_face_box_t *box)
{
    if (box == NULL) {
        return false;
    }

    *box = s_last_face_box;
    bool detected = s_is_face_detected && box->valid;
    if (!detected) {
        return false;
    }

    if (box->x0 < 0) {
        box->x0 = 0;
    }
    if (box->y0 < 0) {
        box->y0 = 0;
    }
    if (box->x1 > 319) {
        box->x1 = 319;
    }
    if (box->y1 > 239) {
        box->y1 = 239;
    }

    return box->x1 > box->x0 && box->y1 > box->y0;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    extern const char index_start[] asm("_binary_index_html_start");
    extern const char index_end[] asm("_binary_index_html_end");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, index_start, index_end - index_start - 1);
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    char json[1536];
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const char *wifi_status = s_wifi_connected ? "connected" : "disconnected";
    const char *camera_status = s_camera_ready ? "ok" : "fault";
    const char *inference_status = inference_status_to_string(s_inference_status);
    prone_face_box_t box;
    bool detected = get_reportable_face_box(&box);
    face_orientation_t orient = compute_face_orientation(&box);

    int written = snprintf(json,
                           sizeof(json),
                           "{\"state\":\"%s\",\"wifi\":\"%s\",\"camera\":\"%s\",\"inference\":\"%s\","
                           "\"mode\":\"%s\",\"jev_status\":\"%s\",\"jev_probability\":%.4f,\"jev_is_prone\":%s,"
                           "\"face_detected\":%s,\"face_confidence\":%.3f,"
                           "\"face_raw_detected\":%s,\"face_detect_streak\":%d,\"face_detect_confirmed\":%s,"
                           "\"monitor_status\":\"%s\",\"prone_like\":%s,\"prone_like_score\":%.3f,"
                           "\"face_missing_ms\":%d,\"pre_disappear_move_px\":%.1f,\"pre_disappear_area_drop\":%.3f,"
                           "\"pre_disappear_yaw_trend\":%.3f,\"pre_disappear_vp_trend\":%.3f,\"pre_disappear_fr_trend\":%.3f,"
                           "\"orientation\":{\"yaw_asym\":%.3f,\"vert_prop\":%.3f,\"fore_ratio\":%.3f,\"valid\":%s},"
                           "\"face_box\":{\"detected\":%s,\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d,\"confidence\":%.3f}}",
                           state_to_string(s_system_state),
                           wifi_status,
                           camera_status,
                           inference_status,
                           s_settings.use_jev ? "jev" : "local",
                           jev_status_name(s_jev_result.status),
                           (double)(s_jev_result.status == JEV_OK ? s_jev_result.prone_probability : -1.0f),
                           s_settings.use_jev && s_jev_result.status == JEV_OK ?
                               (s_jev_result.is_prone ? "true" : "false") : "null",
                           s_is_face_detected ? "true" : "false",
                           (double)s_face_confidence,
                           s_raw_face_detected ? "true" : "false",
                           s_face_detect_streak,
                           s_face_detect_confirmed ? "true" : "false",
                           monitor_status_to_string(s_monitor_status),
                           s_prone_like_detected ? "true" : "false",
                           (double)s_prone_like_score,
                           s_face_missing_ms,
                           (double)s_pre_disappear_move_px,
                           (double)s_pre_disappear_area_drop,
                           (double)s_pre_disappear_yaw_trend,
                           (double)s_pre_disappear_vp_trend,
                           (double)s_pre_disappear_fr_trend,
                           (double)(orient.valid ? orient.yaw_asym : -1.0f),
                           (double)(orient.valid ? orient.vert_prop : -1.0f),
                           (double)(orient.valid ? orient.fore_ratio : -1.0f),
                           orient.valid ? "true" : "false",
                           detected ? "true" : "false",
                           detected ? box.x0 : -1,
                           detected ? box.y0 : -1,
                           detected ? box.x1 : -1,
                           detected ? box.y1 : -1,
                           (double)(detected ? box.confidence : 0.0f));
    xSemaphoreGive(s_state_mutex);
    if (written < 0 || written >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t face_box_get_handler(httpd_req_t *req)
{
    char json[192];
    prone_face_box_t box;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool detected = get_reportable_face_box(&box);

    int written = snprintf(json,
                           sizeof(json),
                           "{\"detected\":%s,\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d,\"confidence\":%.3f}",
                           detected ? "true" : "false",
                           detected ? box.x0 : -1,
                           detected ? box.y0 : -1,
                           detected ? box.x1 : -1,
                           detected ? box.y1 : -1,
                           (double)(detected ? box.confidence : 0.0f));
    xSemaphoreGive(s_state_mutex);
    if (written < 0 || written >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static void reset_monitor(void)
{
    s_last_face_box = (prone_face_box_t){0};
    s_is_face_detected = s_raw_face_detected = s_face_detect_confirmed = false;
    s_face_detect_streak = 0;
    s_face_confidence = 0;
    s_last_face_detected_ms = s_face_missing_started_ms = -1;
    s_face_missing_ms = 0;
    s_prone_like_detected = false;
    s_prone_like_score = 0;
    s_pre_disappear_move_px = s_pre_disappear_area_drop = 0;
    s_pre_disappear_yaw_trend = s_pre_disappear_vp_trend = s_pre_disappear_fr_trend = 0;
    s_face_observation_count = s_face_observation_next_index = 0;
    memset(s_face_observation_history, 0, sizeof(s_face_observation_history));
    s_monitor_status = MONITOR_STATUS_UNKNOWN;
    s_inference_status = INFERENCE_STATUS_NOT_READY;
    s_jev_result = (jev_result_t){.status = JEV_PENDING};
    if (s_camera_ready && s_wifi_connected) set_system_state(SYSTEM_STATE_MONITORING);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char json[128];
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snprintf(json, sizeof(json), "{\"mode\":\"%s\",\"threshold\":%.9g,\"key_configured\":%s}",
             s_settings.use_jev ? "jev" : "local", (double)s_settings.threshold,
             s_settings.api_key[0] ? "true" : "false");
    xSemaphoreGive(s_state_mutex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char body[2048];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid settings size");
    }
    // Browsers must use JSON; ordinary cross-origin forms cannot change credentials.
    char content_type[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "application/json")) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected application/json");
    }
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete settings");
        received += count;
    }
    body[received] = 0;
    if (memchr(body, 0, received)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid settings");
    }
    monitor_settings_t updated;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    esp_err_t err = monitor_settings_decode(body, &s_settings, &updated);
    memset(body, 0, sizeof(body));
    if (err == ESP_OK) {
        err = monitor_settings_save(&updated);
        if (err == ESP_OK) {
            s_settings = updated;
            ++s_settings_generation;
            reset_monitor();
        }
    }
    xSemaphoreGive(s_state_mutex);
    memset(&updated, 0, sizeof(updated));
    if (err == ESP_ERR_INVALID_ARG) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid settings");
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "settings save failed");
    return settings_get_handler(req);
}

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    uint8_t *work = heap_caps_malloc(INFERENCE_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no stream buffer");
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    uint32_t previous = 0;
    esp_err_t err = ESP_OK;
    while (true) {
        size_t len = 0;
        xSemaphoreTake(s_inference_frame_mutex, portMAX_DELAY);
        if (s_inference_frame_len && previous != s_frame_sequence) {
            len = s_inference_frame_len;
            previous = s_frame_sequence;
            memcpy(work, s_inference_frame_buf, len);
        }
        xSemaphoreGive(s_inference_frame_mutex);
        if (!len) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            bool ready = s_camera_ready;
            xSemaphoreGive(s_state_mutex);
            if (!ready) { err = ESP_FAIL; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        char header[96];
        int size = snprintf(header, sizeof(header), "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)len);
        err = httpd_resp_send_chunk(req, header, size);
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char *)work, len);
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, "\r\n", 2);
        if (err != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(STREAM_PACING_DELAY_MS));
    }
    heap_caps_free(work);
    return err;
}

static void capture_task(void *arg)
{
    (void)arg;
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        bool valid = fb && fb->len > 0 && fb->len <= INFERENCE_JPEG_MAX_BYTES;
        xSemaphoreTake(s_inference_frame_mutex, portMAX_DELAY);
        s_inference_frame_len = valid ? fb->len : 0;
        if (valid) {
            memcpy(s_inference_frame_buf, fb->buf, fb->len);
            ++s_frame_sequence;
        }
        xSemaphoreGive(s_inference_frame_mutex);
        if (fb) esp_camera_fb_return(fb);
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (!valid) {
            if (s_camera_ready) ESP_LOGW(TAG, "カメラ画像の取得失敗または JPEG 上限超過");
            s_camera_ready = false;
            s_monitor_status = MONITOR_STATUS_UNKNOWN;
            s_last_face_box.valid = false;
            s_is_face_detected = false;
            s_face_confidence = 0;
            s_inference_status = INFERENCE_STATUS_FAULT;
            s_jev_result = (jev_result_t){.status = JEV_CAMERA_ERROR};
            set_system_state(SYSTEM_STATE_FAULT_CAMERA);
        } else {
            if (!s_camera_ready && s_wifi_connected) set_system_state(SYSTEM_STATE_MONITORING);
            s_camera_ready = true;
        }
        xSemaphoreGive(s_state_mutex);
        vTaskDelay(pdMS_TO_TICKS(valid ? 50 : 200));
    }
}

static void inference_task(void *arg)
{
    (void)arg;
    uint8_t *work = heap_caps_malloc(INFERENCE_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_inference_status = INFERENCE_STATUS_FAULT;
        xSemaphoreGive(s_state_mutex);
        vTaskDelete(NULL);
        return;
    }
    uint32_t generation = UINT32_MAX;
    bool previous_jev = false;
    int64_t next_jev = 0, next_local = 0;
    while (true) {
        monitor_settings_t settings;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        settings = s_settings;
        uint32_t current_generation = s_settings_generation;
        xSemaphoreGive(s_state_mutex);
        int64_t now = esp_timer_get_time() / 1000;
        if (generation != current_generation) {
            if (generation == UINT32_MAX || (!previous_jev && settings.use_jev && now >= next_jev)) {
                next_jev = now;
            }
            next_local = now;
            generation = current_generation;
            previous_jev = settings.use_jev;
        }
        bool due = settings.use_jev ? jev_schedule_due(now, &next_jev) : now >= next_local;
        if (!due) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (!settings.use_jev) next_local = now + INFERENCE_INTERVAL_MS;
        size_t len;
        xSemaphoreTake(s_inference_frame_mutex, portMAX_DELAY);
        len = s_inference_frame_len;
        if (len) memcpy(work, s_inference_frame_buf, len);
        xSemaphoreGive(s_inference_frame_mutex);
        jev_result_t result = {.status = JEV_CAMERA_ERROR};
        bool detected = false;
        float confidence = 0;
        prone_face_box_t box = {0};
        esp_err_t err = ESP_FAIL;
        if (settings.use_jev) {
            if (len) {
                if (!settings.api_key[0]) result.status = JEV_NOT_CONFIGURED;
                else if (time(NULL) < 1704067200) result.status = JEV_CLOCK_NOT_READY;
                else result = jev_classify(work, len, settings.api_key, settings.threshold);
            }
        } else if (len) {
            err = prone_inference_init();
            if (err == ESP_OK) err = prone_inference_run_jpeg(work, len, &detected, &confidence);
            if (err == ESP_OK) prone_inference_get_last_face_box(&box);
        }
        // Skip slots that passed while the previous request was still running.
        if (settings.use_jev) (void)jev_schedule_due(esp_timer_get_time() / 1000, &next_jev);
        memset(settings.api_key, 0, sizeof(settings.api_key));
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        // A completed request belongs only to the settings used to start it.
        if (generation == s_settings_generation && s_camera_ready) {
            if (settings.use_jev) {
                s_jev_result = result;
                s_inference_status = result.status == JEV_OK ? INFERENCE_STATUS_OK : INFERENCE_STATUS_FAULT;
                s_monitor_status = result.status == JEV_OK ?
                    (result.is_prone ? MONITOR_STATUS_NG : MONITOR_STATUS_OK) : MONITOR_STATUS_UNKNOWN;
                set_system_state(result.status != JEV_OK ? SYSTEM_STATE_FAULT_INFERENCE :
                    (result.is_prone ? SYSTEM_STATE_ALERT : SYSTEM_STATE_MONITORING));
            } else {
                s_last_face_box = box;
                s_inference_status = err == ESP_OK ? INFERENCE_STATUS_OK : INFERENCE_STATUS_FAULT;
                update_face_monitor(detected, confidence);
            }
        }
        xSemaphoreGive(s_state_mutex);
    }
}

static esp_err_t start_inference_task(void)
{
    if (s_inference_task_handle != NULL) {
        return ESP_OK;
    }

    if (s_inference_frame_mutex == NULL) {
        s_inference_frame_mutex = xSemaphoreCreateMutex();
        if (s_inference_frame_mutex == NULL) {
            ESP_LOGE(TAG, "推論共有ミューテックス確保失敗");
            return ESP_ERR_NO_MEM;
        }
    }

    s_inference_frame_buf = (uint8_t *)heap_caps_malloc(INFERENCE_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_inference_frame_buf == NULL) {
        s_inference_frame_buf = (uint8_t *)heap_caps_malloc(INFERENCE_JPEG_MAX_BYTES, MALLOC_CAP_8BIT);
    }
    if (s_inference_frame_buf == NULL) {
        ESP_LOGE(TAG, "推論共有バッファ確保失敗");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreate(inference_task,
                                     "inference_task",
                                     12288,
                                     NULL,
                                     tskIDLE_PRIORITY + 1,
                                     &s_inference_task_handle);
    if (task_ok != pdPASS) {
        heap_caps_free(s_inference_frame_buf);
        s_inference_frame_buf = NULL;
        ESP_LOGE(TAG, "推論タスク作成失敗");
        return ESP_FAIL;
    }

    if (xTaskCreate(capture_task, "capture_task", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        vTaskDelete(s_inference_task_handle);
        s_inference_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "推論タスク開始");
    return ESP_OK;
}

static void update_face_monitor(bool is_face_detected, float confidence)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool raw_face_ok = is_face_detected && (confidence >= FACE_CONFIDENCE_TH);

    if (raw_face_ok) {
        if (s_face_detect_streak < FACE_DETECT_OK_STREAK_REQUIRED) {
            s_face_detect_streak++;
        }
    } else {
        s_face_detect_streak = 0;
    }

    s_raw_face_detected = raw_face_ok;
    s_face_detect_confirmed = raw_face_ok && (s_face_detect_streak >= FACE_DETECT_OK_STREAK_REQUIRED);
    if (raw_face_ok) {
        s_last_face_detected_ms = now_ms;
    }
    bool face_present =
        raw_face_ok || (s_last_face_detected_ms >= 0 && (now_ms - s_last_face_detected_ms) <= FACE_LOST_GRACE_MS);

    if (s_inference_status == INFERENCE_STATUS_FAULT) {
        s_is_face_detected = false;
        s_face_confidence = 0.0f;
        s_monitor_status = MONITOR_STATUS_UNKNOWN;
        s_prone_like_detected = false;
        s_prone_like_score = 0.0f;
        s_face_missing_started_ms = -1;
        s_face_missing_ms = 0;
        s_pre_disappear_move_px = 0.0f;
        s_pre_disappear_area_drop = 0.0f;
        s_pre_disappear_yaw_trend = 0.0f;
        s_pre_disappear_vp_trend = 0.0f;
        s_pre_disappear_fr_trend = 0.0f;
        if (s_system_state != SYSTEM_STATE_FAULT_CAMERA) {
            set_system_state(SYSTEM_STATE_FAULT_INFERENCE);
        }
        return;
    }

    if (face_present) {
        if (raw_face_ok) {
            update_face_observation(&s_last_face_box, now_ms);
        }
        s_face_missing_started_ms = -1;
        s_face_missing_ms = 0;
        s_prone_like_score = 0.0f;
        s_prone_like_detected = false;
        s_pre_disappear_move_px = 0.0f;
        s_pre_disappear_area_drop = 0.0f;
        s_pre_disappear_yaw_trend = 0.0f;
        s_pre_disappear_vp_trend = 0.0f;
        s_pre_disappear_fr_trend = 0.0f;
        s_monitor_status = MONITOR_STATUS_OK;
    } else {
        if (s_face_missing_started_ms < 0) {
            s_face_missing_started_ms = now_ms;
            capture_pre_disappear_metrics();
        }

        s_face_missing_ms = (int)(now_ms - s_face_missing_started_ms);
        s_prone_like_score =
            compute_prone_like_score(s_face_missing_ms, s_pre_disappear_move_px, s_pre_disappear_area_drop);
        s_prone_like_detected = (s_prone_like_score >= PRONE_LIKE_SCORE_TH);
        s_monitor_status = s_prone_like_detected ? MONITOR_STATUS_NG : MONITOR_STATUS_UNKNOWN;
    }

    s_is_face_detected = face_present;
    if (raw_face_ok) {
        s_face_confidence = confidence;
    } else if (!face_present) {
        s_face_confidence = 0.0f;
    }

    if (now_ms - s_last_face_log_ms >= 1000) {
        s_last_face_log_ms = now_ms;
        ESP_LOGI(TAG,
                 "monitor: face=%d confidence=%.3f raw_ok=%d streak=%d confirmed=%d missing_ms=%d move_px=%.1f area_drop=%.3f yaw_trend=%.3f vp_trend=%.3f fr_trend=%.3f prone_score=%.2f prone_like=%d label=%s state=%s",
                 s_is_face_detected ? 1 : 0,
                 (double)s_face_confidence,
                 raw_face_ok ? 1 : 0,
                 s_face_detect_streak,
                 s_face_detect_confirmed ? 1 : 0,
                 s_face_missing_ms,
                 (double)s_pre_disappear_move_px,
                 (double)s_pre_disappear_area_drop,
                 (double)s_pre_disappear_yaw_trend,
                 (double)s_pre_disappear_vp_trend,
                 (double)s_pre_disappear_fr_trend,
                 (double)s_prone_like_score,
                 s_prone_like_detected ? 1 : 0,
                 monitor_status_to_string(s_monitor_status),
                 state_to_string(s_system_state));
    }

    if (face_present) {
        if ((s_system_state == SYSTEM_STATE_ALERT || s_system_state == SYSTEM_STATE_FAULT_INFERENCE) && s_camera_ready) {
            set_system_state(SYSTEM_STATE_MONITORING);
        }
        return;
    }

    if (s_system_state == SYSTEM_STATE_FAULT_CAMERA) {
        return;
    }

    if (s_prone_like_detected) {
        set_system_state(SYSTEM_STATE_ALERT);
    } else if (s_camera_ready) {
        set_system_state(SYSTEM_STATE_MONITORING);
    }
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;

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

    const httpd_uri_t settings_get_uri = {.uri = "/settings", .method = HTTP_GET, .handler = settings_get_handler};
    const httpd_uri_t settings_post_uri = {.uri = "/settings", .method = HTTP_POST, .handler = settings_post_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &settings_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &settings_post_uri));
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
    config.keep_alive_enable = false;

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
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        set_system_state(SYSTEM_STATE_READY);
        ESP_LOGI(TAG,
                 "Wi-Fi 接続完了 ip=" IPSTR " root=http://" IPSTR "/ stream=http://" IPSTR ":81/stream",
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t init_nvs(void)
{
    // Settings now contain credentials; initialization failure must not erase them.
    return nvs_flash_init();
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
        .xclk_freq_hz = 10000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 10,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
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
    s_state_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_state_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(monitor_settings_load(&s_settings));
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

        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        if (s_camera_ready) {
            ESP_ERROR_CHECK(start_inference_task());
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
