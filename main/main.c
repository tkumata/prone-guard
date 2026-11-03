#include "cJSON.h"
#include "depth_map.h"
#include "depth_sensor.h"
#include "device_security.h"
#include "edge_sender.h"
#include "esp_camera.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "monitor_settings.h"
#include "oled_display.h"
#include "prone_inference_bridge.h"
#include "telemetry.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW) || !defined(CONFIG_SCCB_HARDWARE_I2C_PORT0)
#error "Select the new SCCB I2C driver on I2C0 in menuconfig; I2C1 is reserved for depth/OLED."
#endif

static const char *TAG = "prone_guard";
static SemaphoreHandle_t state_mutex;
static monitor_settings_t settings;
static char active_ssid[33], active_password[65];
static device_security_t security;
static sensor_observation_t latest;
static depth_map_t depth_map;
static char boot_id[33];
static uint32_t settings_generation;
static bool wifi_connected, clock_synced, observation_ready;
static const char *send_status = "pending";
static esp_timer_handle_t wifi_retry;
extern const char index_html_start[] asm("_binary_index_html_start");

static void serial_provision_task(void *arg)
{
    (void)arg;
    char *line = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line)
    {
        ESP_LOGE(TAG, "provisioning buffer allocation failed");
        vTaskDelete(NULL);
        return;
    }
    size_t length = 0;
    bool overflow = false;
    while (true)
    {
        int c = getchar();
        if (c == EOF)
        {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\n')
        {
            line[length] = 0;
            esp_err_t err = overflow ? ESP_ERR_INVALID_SIZE : device_provision(line);
            mbedtls_platform_zeroize(line, 32768);
            length = 0;
            overflow = false;
            if (err == ESP_OK)
            {
                puts("PROVISION_OK");
                fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
            else
                ESP_LOGE(TAG, "PROVISION_FAILED: %s", esp_err_to_name(err));
        }
        else if (c != '\r')
        {
            if (length < 32767)
                line[length++] = (char)c;
            else
                overflow = true;
        }
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    if (!json)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "allocation failed");
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "allocation failed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!device_authorize(req, &security))
        return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(
        req, "Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src "
        "'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    return httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    if (!device_authorize(req, &security))
        return ESP_OK;
    cJSON *json = cJSON_CreateObject();
    if (!json)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "allocation failed");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    bool ok = cJSON_AddStringToObject(json, "wifi_ssid", settings.wifi_ssid) &&
              cJSON_AddStringToObject(json, "bed_id", settings.bed_id) &&
              cJSON_AddStringToObject(json, "destination_url", settings.destination_url) &&
              cJSON_AddBoolToObject(json, "key_configured", settings.device_key[0] != 0) &&
              cJSON_AddBoolToObject(json, "restart_required",
                                    strcmp(settings.wifi_ssid, active_ssid) != 0 ||
                                        strcmp(settings.wifi_password, active_password) != 0);
    xSemaphoreGive(state_mutex);
    if (!ok)
    {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "allocation failed");
    }
    return send_json(req, json);
}

static bool same_origin(httpd_req_t *req)
{
    char host[128], origin[160], expected[160];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK)
        return false;
    int length = snprintf(expected, sizeof(expected), "https://%s", host);
    return length > 0 && length < sizeof(expected) && strcmp(origin, expected) == 0;
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (!device_authorize(req, &security))
        return ESP_OK;
    char content_type[64];
    if (!same_origin(req) ||
        httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) !=
            ESP_OK ||
        strcmp(content_type, "application/json"))
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "invalid origin or content type");
    char body[2048];
    if (req->content_len <= 0 || req->content_len >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid settings size");
    size_t received = 0;
    while (received < req->content_len)
    {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0)
        {
            mbedtls_platform_zeroize(body, sizeof(body));
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete settings");
        }
        received += count;
    }
    body[received] = 0;
    if (memchr(body, 0, received))
    {
        mbedtls_platform_zeroize(body, sizeof(body));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid settings");
    }
    monitor_settings_t updated;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    esp_err_t err = monitor_settings_decode(body, &settings, &updated);
    if (err == ESP_OK)
        err = monitor_settings_save(&updated);
    if (err == ESP_OK)
    {
        settings = updated;
        ++settings_generation;
        send_status = "pending";
    }
    xSemaphoreGive(state_mutex);
    mbedtls_platform_zeroize(body, sizeof(body));
    mbedtls_platform_zeroize(&updated, sizeof(updated));
    if (err != ESP_OK)
        return httpd_resp_send_err(req,
                                   err == ESP_ERR_INVALID_ARG ? HTTPD_400_BAD_REQUEST
                                                              : HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "settings save failed");
    esp_err_t cleanup = monitor_settings_cleanup_legacy();
    if (cleanup != ESP_OK)
        ESP_LOGE(TAG, "legacy credential cleanup failed: %s", esp_err_to_name(cleanup));
    return settings_get_handler(req);
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    if (!device_authorize(req, &security))
        return ESP_OK;
    cJSON *json = cJSON_CreateObject();
    if (!json)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "allocation failed");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    bool ok =
        cJSON_AddStringToObject(json, "wifi", wifi_connected ? "connected" : "disconnected") &&
        cJSON_AddStringToObject(json, "clock", clock_synced ? "synchronized" : "pending") &&
        cJSON_AddStringToObject(json, "camera", latest.camera_ok ? "ok" : "error") &&
        cJSON_AddStringToObject(json, "depth", latest.depth_ok ? "ok" : "error") &&
        cJSON_AddStringToObject(json, "sender", send_status);
    xSemaphoreGive(state_mutex);
    if (!ok)
    {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "allocation failed");
    }
    return send_json(req, json);
}

static esp_err_t start_management_server(void)
{
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.httpd.stack_size = 12288;
    config.httpd.lru_purge_enable = true;
    config.httpd.recv_wait_timeout = 5;
    config.httpd.send_wait_timeout = 5;
    config.servercert = (const uint8_t *)security.certificate;
    config.servercert_len = strlen(security.certificate) + 1;
    config.prvtkey_pem = (const uint8_t *)security.private_key;
    config.prvtkey_len = strlen(security.private_key) + 1;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_ssl_start(&server, &config);
    if (err != ESP_OK)
        return err;
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get_handler},
        {.uri = "/health", .method = HTTP_GET, .handler = health_get_handler},
        {.uri = "/settings", .method = HTTP_GET, .handler = settings_get_handler},
        {.uri = "/settings", .method = HTTP_POST, .handler = settings_post_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
    {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK)
            return err;
    }
    return ESP_OK;
}

static void retry_wifi(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Wi-Fi retry failed: %s", esp_err_to_name(err));
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        retry_wifi(NULL);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        wifi_connected = false;
        xSemaphoreGive(state_mutex);
        esp_err_t err = esp_timer_stop(wifi_retry);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            ESP_LOGW(TAG, "Wi-Fi retry timer stop failed");
        err = esp_timer_start_once(wifi_retry, 5000000);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "Wi-Fi retry timer failed: %s", esp_err_to_name(err));
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        wifi_connected = true;
        xSemaphoreGive(state_mutex);
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "management=https://" IPSTR "/", IP2STR(&event->ip_info.ip));
    }
}

static void synchronized(struct timeval *time)
{
    (void)time;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    clock_synced = true;
    xSemaphoreGive(state_mutex);
    ESP_LOGI(TAG, "NTP synchronized");
}

static esp_err_t init_camera(void)
{
    const camera_config_t config = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = 15,
        .pin_sccb_sda = 4,
        .pin_sccb_scl = 5,
        .pin_d7 = 16,
        .pin_d6 = 17,
        .pin_d5 = 18,
        .pin_d4 = 12,
        .pin_d3 = 10,
        .pin_d2 = 8,
        .pin_d1 = 9,
        .pin_d0 = 11,
        .pin_vsync = 6,
        .pin_href = 7,
        .pin_pclk = 13,
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
    return esp_camera_init(&config);
}

static void sensor_task(void *arg)
{
    i2c_master_dev_handle_t depth_device = arg;
    bool camera_ready = false, depth_ready = false;
    int64_t next_camera = 0, next_depth = 0;
    uint64_t sequence = 0;
    TickType_t tick = xTaskGetTickCount();
    while (true)
    {
        sensor_observation_t observation = {.sequence = sequence++};
        int64_t now = esp_timer_get_time();
        if (!camera_ready && now >= next_camera)
        {
            camera_ready = init_camera() == ESP_OK;
            next_camera = now + 5000000;
        }
        if (depth_device && !depth_ready && now >= next_depth)
        {
            depth_ready = depth_sensor_init(depth_device) == ESP_OK;
            next_depth = esp_timer_get_time() + 5000000;
            if (!depth_ready)
            {
                ESP_LOGW(TAG, "depth sensor initialization failed");
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                depth_map.error = true;
                xSemaphoreGive(state_mutex);
            }
        }
        if (depth_ready)
        {
            esp_err_t err = depth_sensor_read(&observation);
            int64_t acquired_us = esp_timer_get_time();
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            if (err == ESP_OK)
            {
                depth_map.available = true;
                depth_map.error = false;
                depth_map.acquired_us = acquired_us;
                memcpy(depth_map.distance, observation.distance, sizeof(depth_map.distance));
                memcpy(depth_map.target_status, observation.target_status,
                       sizeof(depth_map.target_status));
            }
            else if (err != ESP_ERR_NOT_FINISHED)
                depth_map.error = true;
            xSemaphoreGive(state_mutex);
            if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED)
                depth_ready = false;
        }
        if (camera_ready)
        {
            camera_fb_t *frame = esp_camera_fb_get();
            bool detected = false;
            float confidence = 0;
            prone_face_box_t box = {0};
            esp_err_t err = frame ? prone_inference_init() : ESP_FAIL;
            if (err == ESP_OK)
                err = prone_inference_run_jpeg(frame->buf, frame->len, &detected, &confidence);
            if (frame)
                esp_camera_fb_return(frame);
            if (err == ESP_OK)
                err = prone_inference_get_last_face_box(&box);
            if (err == ESP_OK)
            {
                observation.camera_ok = true;
                observation.camera_timestamp = time(NULL);
                observation.face_area_px2 =
                    detected && box.valid ? telemetry_face_area(box.x0, box.y0, box.x1, box.y1)
                                          : 0.0;
                observation.face_detected = observation.face_area_px2 > 0.0;
                observation.camera_ok = !detected || observation.face_detected;
            }
            else if (!frame)
            {
                esp_camera_deinit();
                camera_ready = false;
            }
        }
        observation.timestamp = time(NULL);
        if (observation.depth_ok && observation.timestamp - observation.depth_timestamp > 1)
            observation.depth_ok = false;
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        latest = observation;
        observation_ready = true;
        xSemaphoreGive(state_mutex);
        if (xTaskDelayUntil(&tick, pdMS_TO_TICKS(1000)) == pdFALSE)
            tick = xTaskGetTickCount();
    }
}

static void oled_task(void *arg)
{
    i2c_master_dev_handle_t device = arg;
    bool ready = false;
    int64_t next_init = 0;
    uint8_t frame[DEPTH_MAP_FRAME_BYTES];
    TickType_t tick = xTaskGetTickCount();
    while (true)
    {
        int64_t now = esp_timer_get_time();
        if (ready || now >= next_init)
        {
            esp_err_t err = ready ? ESP_OK : oled_display_init(device);
            if (err == ESP_OK)
            {
                depth_map_t map;
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                map = depth_map;
                xSemaphoreGive(state_mutex);
                depth_map_render(&map, esp_timer_get_time(), frame);
                err = oled_display_write(device, frame);
            }
            ready = err == ESP_OK;
            if (!ready)
            {
                ESP_LOGW(TAG, "OLED update failed: %s", esp_err_to_name(err));
                next_init = esp_timer_get_time() + 5000000;
            }
        }
        if (xTaskDelayUntil(&tick, pdMS_TO_TICKS(1000)) == pdFALSE)
            tick = xTaskGetTickCount();
    }
}

static void sender_task(void *arg)
{
    (void)arg;
    uint64_t previous = UINT64_MAX;
    TickType_t tick = xTaskGetTickCount();
    while (true)
    {
        monitor_settings_t current;
        sensor_observation_t observation;
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        current = settings;
        observation = latest;
        uint32_t generation = settings_generation;
        bool connected = wifi_connected, synced = clock_synced, available = observation_ready;
        xSemaphoreGive(state_mutex);
        const char *status = "pending";
        if (!monitor_settings_ready(&current))
            status = "not_configured";
        else if (!connected)
            status = "disconnected";
        else if (!synced || time(NULL) < 1704067200)
            status = "clock_not_ready";
        else if (available && observation.sequence != previous &&
                 time(NULL) - observation.timestamp <= 5)
        {
            previous = observation.sequence;
            char *json = telemetry_encode(&observation, boot_id, current.bed_id);
            esp_err_t err = json ? edge_sender_send(current.destination_url, current.device_key,
                                                    json, security.edge_ca)
                                 : ESP_ERR_NO_MEM;
            cJSON_free(json);
            status = err == ESP_OK ? "ok" : err == ESP_ERR_TIMEOUT ? "timeout" : "send_error";
            if (err != ESP_OK)
                ESP_LOGW(TAG, "sensor send failed: %s", esp_err_to_name(err));
        }
        else
            status = "waiting_for_observation";
        mbedtls_platform_zeroize(&current, sizeof(current));
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        if (generation == settings_generation)
            send_status = status;
        xSemaphoreGive(state_mutex);
        if (xTaskDelayUntil(&tick, pdMS_TO_TICKS(1000)) == pdFALSE)
            tick = xTaskGetTickCount();
    }
}

static void init_sensor_devices(i2c_master_dev_handle_t *depth, i2c_master_dev_handle_t *oled)
{
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_NUM_14,
        .scl_io_num = GPIO_NUM_21,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7};
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "sensor I2C initialization failed: %s", esp_err_to_name(err));
        return;
    }
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x29, .scl_speed_hz = 400000};
    err = i2c_master_bus_add_device(bus, &config, depth);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "depth I2C registration failed: %s", esp_err_to_name(err));
    config.device_address = 0x3c;
    err = i2c_master_bus_add_device(bus, &config, oled);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "OLED I2C registration failed: %s", esp_err_to_name(err));
}

void app_main(void)
{
    esp_err_t err = device_storage_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "encrypted storage unavailable; provision the HMAC eFuse key and secure_nvs "
                 "partition: %s",
                 esp_err_to_name(err));
        return;
    }
    state_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(state_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(serial_provision_task, "usb_provision", 12288, NULL,
                                tskIDLE_PRIORITY + 1, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    if (device_security_load(&security) != ESP_OK || monitor_settings_load(&settings) != ESP_OK)
    {
        ESP_LOGW(TAG, "USB provisioning required; waiting for a provisioning JSON line");
        return;
    }
    strlcpy(active_ssid, settings.wifi_ssid, sizeof(active_ssid));
    strlcpy(active_password, settings.wifi_password, sizeof(active_password));
    unsigned char random[16];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i)
        snprintf(boot_id + i * 2, 3, "%02x", random[i]);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    const esp_timer_create_args_t timer = {.callback = retry_wifi, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &wifi_retry));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t wifi = {
        .sta = {.threshold.authmode = WIFI_AUTH_WPA2_PSK, .pmf_cfg = {.capable = true}}};
    strlcpy((char *)wifi.sta.ssid, active_ssid, sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, active_password, sizeof(wifi.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    mbedtls_platform_zeroize(&wifi, sizeof(wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_sync_interval(86400000);
    esp_sntp_set_time_sync_notification_cb(synchronized);
    esp_sntp_init();
    ESP_ERROR_CHECK(start_management_server());
    i2c_master_dev_handle_t depth_device = NULL, oled_device = NULL;
    init_sensor_devices(&depth_device, &oled_device);
    depth_map.error = !depth_device;
    ESP_ERROR_CHECK(
        xTaskCreate(sensor_task, "sensors", 16384, depth_device, tskIDLE_PRIORITY + 1, NULL) == pdPASS
            ? ESP_OK
            : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(
        xTaskCreate(sender_task, "edge_sender", 12288, NULL, tskIDLE_PRIORITY + 1, NULL) == pdPASS
            ? ESP_OK
            : ESP_ERR_NO_MEM);
    if (oled_device &&
        xTaskCreate(oled_task, "oled", 4096, oled_device, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
        ESP_LOGE(TAG, "OLED task allocation failed");
}
