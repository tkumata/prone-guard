#include "cJSON.h"
#include "monitor_settings.h"
#include "telemetry.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static monitor_settings_t settings(void)
{
    monitor_settings_t value = {.wifi_ssid = "test-network",
                                .wifi_password = "test-password",
                                .bed_id = "bed-01",
                                .destination_url = "https://edge.example/v1/measurements"};
    memset(value.device_key, 'a', 64);
    return value;
}

static sensor_observation_t observation(void)
{
    sensor_observation_t value = {.sequence = 0,
                                  .timestamp = 1793142000,
                                  .camera_timestamp = 1793142000,
                                  .camera_ok = true,
                                  .face_detected = true,
                                  .face_area_px2 = 1200,
                                  .depth_timestamp = 1793142000,
                                  .depth_ok = true};
    for (int i = 0; i < 64; ++i)
    {
        value.distance[i] = 1000;
        value.target_status[i] = 5;
    }
    return value;
}

int main(int argc, char **argv)
{
    sensor_observation_t sample = observation();
    char *encoded = telemetry_encode(&sample, "sample-boot", "bed-01");
    assert(encoded);
    if (argc == 2 && !strcmp(argv[1], "--fixture"))
    {
        fputs(encoded, stdout);
        cJSON_free(encoded);
        return 0;
    }
    assert(encoded[strlen(encoded) - 1] == '\n');
    cJSON *json = cJSON_Parse(encoded);
    assert(json);
    cJSON *depth = cJSON_GetObjectItem(json, "depth");
    cJSON *distance = cJSON_GetObjectItem(depth, "distance");
    assert(cJSON_GetArraySize(distance) == 8);
    assert(cJSON_GetArraySize(cJSON_GetArrayItem(distance, 7)) == 8);
    assert(cJSON_GetArrayItem(cJSON_GetArrayItem(distance, 7), 7)->valueint == 1000);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "camera"), "face_detected")->valueint ==
           1);
    cJSON_Delete(json);
    cJSON_free(encoded);
    sample.face_confidence = 0.85;
    sample.face_box[0] = 10;
    sample.face_box[1] = 20;
    sample.face_box[2] = 50;
    sample.face_box[3] = 50;
    encoded = telemetry_encode(&sample, "sample-boot", "bed-01");
    assert(encoded);
    json = cJSON_Parse(encoded);
    assert(json);
    cJSON *camera = cJSON_GetObjectItem(json, "camera");
    assert(cJSON_GetObjectItem(camera, "face_box") != NULL);
    assert(cJSON_GetObjectItem(camera, "face_confidence")->valuedouble > 0.8);
    cJSON_Delete(json);
    cJSON_free(encoded);
    sample = observation();
    sample.camera_ok = false;
    sample.depth_ok = false;
    encoded = telemetry_encode(&sample, "sample-boot", "bed-01");
    json = cJSON_Parse(encoded);
    assert(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "camera"), "face_detected")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "depth"), "distance")));
    cJSON_Delete(json);
    cJSON_free(encoded);
    assert(!telemetry_encode(&sample, "boot", ""));
    sample = observation();
    sample.face_area_px2 = NAN;
    assert(!telemetry_encode(&sample, "boot", "bed-01"));
    assert(telemetry_face_area(-5, -5, 330, 250) == 76800);
    assert(telemetry_face_area(40, 50, 30, 60) == 0);
    assert(telemetry_face_area(10, 20, 50, 70) == 2000);

    monitor_settings_t current = settings(), updated;
    assert(monitor_settings_ready(&current));
    monitor_destination_t target;
    assert(monitor_destination_parse("https://edge.example:8443/v1/measurements", &target));
    assert(!strcmp(target.hostname, "edge.example") && target.port == 8443);
    assert(monitor_destination_parse("https://[2001:db8::1]/v1/measurements", &target));
    assert(!strcmp(target.hostname, "2001:db8::1") && target.port == 443);
    assert(!monitor_destination_parse("https://edge.example:0/v1/measurements", &target));
    const char *update =
        "{\"wifi_ssid\":\"new-network\",\"wifi_password\":\"\",\"bed_id\":\"bed-02\",\"destination_"
        "url\":\"https://edge.example:8443/v1/measurements\",\"device_key\":\"\"}";
    assert(monitor_settings_decode(update, &current, &updated, NULL) == ESP_OK);
    assert(!strcmp(updated.wifi_password, current.wifi_password));
    assert(!strcmp(updated.device_key, current.device_key));
    assert(!strcmp(updated.bed_id, "bed-02"));
    assert(!strcmp(current.bed_id, "bed-01"));
    assert(updated.tft_mirror_x == 0);
    current.tft_mirror_x = 1;
    current.face_model = MONITOR_FACE_ESPDET_PICO_416;
    bool restart = true;
    assert(monitor_settings_decode(update, &current, &updated, &restart) == ESP_OK);
    assert(updated.tft_mirror_x == 1 && updated.face_model == MONITOR_FACE_ESPDET_PICO_416);
    assert(!restart);
    for (unsigned mirror = 0; mirror <= 1; ++mirror)
    {
        cJSON *request = cJSON_Parse(update);
        assert(request && cJSON_AddBoolToObject(request, "tft_mirror_x", mirror));
        char *body = cJSON_PrintUnformatted(request);
        assert(body && monitor_settings_decode(body, &current, &updated, NULL) == ESP_OK);
        assert(updated.tft_mirror_x == mirror && current.tft_mirror_x == 1);
        cJSON_free(body);
        cJSON_Delete(request);
    }
    const char *bad_mirrors[] = {"\"true\"", "0", "1", "null", "[]", "{}",
                                "true,\"tft_mirror_x\":false"};
    for (size_t i = 0; i < sizeof(bad_mirrors) / sizeof(bad_mirrors[0]); ++i)
    {
        char body[512];
        snprintf(body, sizeof(body), "%.*s,\"tft_mirror_x\":%s}",
                 (int)strlen(update) - 1, update, bad_mirrors[i]);
        assert(monitor_settings_decode(body, &current, &updated, NULL) == ESP_ERR_INVALID_ARG);
        assert(current.tft_mirror_x == 1);
    }
    char model_request[512];
    snprintf(model_request, sizeof(model_request),
             "%.*s,\"face_model\":\"msrmnp\",\"restart_after_save\":true}",
             (int)strlen(update) - 1, update);
    assert(monitor_settings_decode(model_request, &current, &updated, &restart) == ESP_OK);
    assert(updated.face_model == MONITOR_FACE_MSRMNP && restart);
    assert(monitor_settings_decode(model_request, &current, &updated, NULL) == ESP_ERR_INVALID_ARG);
    snprintf(model_request, sizeof(model_request),
             "%.*s,\"face_model\":\"espdet_pico_416\",\"restart_after_save\":false}",
             (int)strlen(update) - 1, update);
    assert(monitor_settings_decode(model_request, &current, &updated, &restart) == ESP_OK);
    assert(updated.face_model == MONITOR_FACE_ESPDET_PICO_416 && !restart);
    const char *bad_options[] = {"\"face_model\":\"unknown\"", "\"face_model\":1",
                                 "\"face_model\":\"msrmnp\",\"face_model\":\"espdet_pico_416\"",
                                 "\"restart_after_save\":\"true\"",
                                 "\"restart_after_save\":true,\"restart_after_save\":false"};
    for (size_t i = 0; i < sizeof(bad_options) / sizeof(bad_options[0]); ++i)
    {
        snprintf(model_request, sizeof(model_request), "%.*s,%s}",
                 (int)strlen(update) - 1, update, bad_options[i]);
        assert(monitor_settings_decode(model_request, &current, &updated, &restart) ==
               ESP_ERR_INVALID_ARG);
        assert(!restart);
    }
    monitor_settings_t empty = {0};
    const char *provision = "{\"wifi_ssid\":\"network\",\"wifi_password\":\"password\","
                            "\"bed_id\":\"bed-01\",\"destination_url\":\"\",\"device_key\":\"\"}";
    assert(monitor_settings_decode(provision, &empty, &updated, NULL) == ESP_OK);
    assert(updated.tft_mirror_x == 0 && updated.face_model == MONITOR_FACE_MSRMNP);
    char clear[512];
    snprintf(clear, sizeof(clear), "%.*s,\"clear_device_key\":true}",
             (int)strlen(update) - 1, update);
    assert(monitor_settings_decode(clear, &current, &updated, NULL) == ESP_OK);
    assert(updated.bed_id[0] && updated.destination_url[0]);
    assert(!monitor_settings_ready(&updated));
    const char empty_key[sizeof(updated.device_key)] = {0};
    assert(!memcmp(updated.device_key, empty_key, sizeof(empty_key)));

    monitor_settings_t previously_cleared = current;
    previously_cleared.device_key[0] = 0;
    assert(monitor_settings_decode(update, &previously_cleared, &updated, NULL) == ESP_OK);
    assert(!memcmp(updated.device_key, empty_key, sizeof(empty_key)));
    const char *invalid[] = {"http://edge.example/v1/measurements",
                             "https://user:secret@edge.example/v1/measurements",
                             "https://edge.example:bad/v1/measurements",
                             "https://edge.example:99999/v1/measurements",
                             "https://edge.example/v1/measurements?key=secret",
                             "https://edge.example/other"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        updated = current;
        strcpy(updated.destination_url, invalid[i]);
        assert(!monitor_settings_valid(&updated));
    }
    assert(monitor_settings_decode("{\"mode\":\"jev\"}", &current, &updated, NULL) != ESP_OK);
    puts("Settings and telemetry behavior tests passed");
}
