#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "jev_protocol.h"
#include "monitor_settings.h"

static void test_schedule(void)
{
    int64_t next = 0;
    assert(jev_schedule_due(0, &next) && next == 10000);
    assert(!jev_schedule_due(8000, &next));
    assert(!jev_schedule_due(9999, &next));
    assert(jev_schedule_due(10000, &next) && next == 20000);
    // A delayed task skips missed slots instead of sending a backlog.
    assert(jev_schedule_due(45000, &next) && next == 50000);
    assert(!jev_schedule_due(45001, &next));
    next = 0;
    assert(jev_schedule_due(0, &next));
    // Discard the slot that elapsed while a request was pending.
    (void)jev_schedule_due(17000, &next);
    assert(next == 20000 && !jev_schedule_due(17000, &next));
    assert(jev_schedule_due(20000, &next));
}

static void test_classification(void)
{
    const char *response = "{\"answers\":{\"posture\":{\"type\":\"choice\",\"choice\":\"prone\",\"probabilities\":{\"prone\":0.5,\"not_prone\":0.4,\"unjudgeable\":0.1}}}}";
    jev_result_t result = jev_parse_response(response, 0.5f);
    assert(result.status == JEV_OK && result.is_prone);
    assert(result.prone_probability == 0.5f);
    assert(!jev_parse_response(response, 0.51f).is_prone);
    const char *unknown = "{\"answers\":{\"posture\":{\"type\":\"choice\",\"choice\":\"unjudgeable\",\"probabilities\":{\"prone\":0.2,\"not_prone\":0.1,\"unjudgeable\":0.7}}}}";
    result = jev_parse_response(unknown, 0.1f);
    assert(result.status == JEV_UNJUDGEABLE && !result.is_prone);
    const char *negative = "{\"answers\":{\"posture\":{\"type\":\"choice\",\"choice\":\"not_prone\",\"probabilities\":{\"prone\":0.01,\"not_prone\":0.98,\"unjudgeable\":0.01}}}}";
    result = jev_parse_response(negative, 0.5f);
    assert(result.status == JEV_OK && !result.is_prone);
    const char *invalid[] = {"", "{}", "{", "null", "[]", "{} trailing",
        "{\"answers\":{\"posture\":{\"type\":\"choice\",\"choice\":\"prone\",\"probabilities\":{\"prone\":\"0.8\"}}}}",
        "{\"answers\":{\"posture\":{\"type\":\"choice\",\"choice\":\"prone\",\"probabilities\":{\"prone\":2,\"not_prone\":0,\"unjudgeable\":0}}}}",
        "{\"answers\":{\"posture\":{\"type\":\"choice\",\"choice\":\"other\",\"probabilities\":{\"prone\":0.5,\"not_prone\":0.4,\"unjudgeable\":0.1}}}}"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i)
        assert(jev_parse_response(invalid[i], 0.5f).status == JEV_INVALID_RESPONSE);
    assert(jev_parse_response(response, NAN).status == JEV_INVALID_RESPONSE);
    assert(jev_parse_response(response, -0.1f).status == JEV_INVALID_RESPONSE);
}

static void test_settings(void)
{
    monitor_settings_t current = {.threshold = 0.5f, .api_key = "saved-secret"};
    monitor_settings_t updated;
    assert(monitor_settings_decode("{\"mode\":\"jev\",\"threshold\":0.7,\"api_key\":\"\"}", &current, &updated) == ESP_OK);
    assert(updated.use_jev && fabsf(updated.threshold - 0.7f) < 0.001f);
    assert(!strcmp(updated.api_key, "saved-secret"));
    assert(monitor_settings_decode("{\"mode\":\"local\",\"threshold\":0.5,\"api_key\":\"new-secret\"}", &updated, &current) == ESP_OK);
    assert(!current.use_jev && !strcmp(current.api_key, "new-secret"));
    current.api_key[0] = 0;
    assert(monitor_settings_decode("{\"mode\":\"jev\",\"threshold\":0.5,\"api_key\":\"\"}", &current, &updated) == ESP_OK);
    assert(!updated.api_key[0]);
    const char *invalid[] = {"{}", "null", "[]", "{", "{} trailing",
        "{\"mode\":\"unknown\",\"threshold\":0.5,\"api_key\":\"\"}",
        "{\"mode\":\"jev\",\"threshold\":1.01,\"api_key\":\"\"}",
        "{\"mode\":\"jev\",\"threshold\":-0.1,\"api_key\":\"\"}",
        "{\"mode\":\"jev\",\"threshold\":0.5,\"api_key\":\"bad\\r\\nkey\"}",
        "{\"mode\":\"jev\",\"threshold\":\"0.5\",\"api_key\":\"\"}"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i)
        assert(monitor_settings_decode(invalid[i], &current, &updated) == ESP_ERR_INVALID_ARG);
    current.threshold = NAN;
    assert(!monitor_settings_valid(&current));
    current.threshold = 0.5f;
    memset(current.api_key, 'x', sizeof(current.api_key));
    assert(!monitor_settings_valid(&current));
}

static jev_result_t parse_face_fixture(const char *presence, const char *box, const char *choice)
{
    char json[2048];
    int len = snprintf(json, sizeof(json),
        "{\"answers\":{\"posture\":{\"type\":\"choice\",\"choice\":\"%s\","
        "\"probabilities\":{\"prone\":0.7,\"not_prone\":0.2,\"unjudgeable\":0.1}},"
        "\"face_present\":%s,\"face_box\":%s}}", choice, presence, box);
    assert(len > 0 && len < (int)sizeof(json));
    return jev_parse_response(json, 0.5f);
}

static void test_face(void)
{
    const char *present = "{\"type\":\"noul\",\"noul\":0.5}";
    const char *box = "{\"type\":\"box\",\"box\":{\"left\":0.25,\"right\":0.75,\"top\":0.125,\"bottom\":0.875},\"confidence\":0.9}";
    jev_result_t result = parse_face_fixture(present, box, "prone");
    assert(result.status == JEV_OK && result.is_prone);
    assert(result.face_status == JEV_OK && result.face_box.detected);
    assert(result.face_box.x0 == 80 && result.face_box.x1 == 240);
    assert(result.face_box.y0 == 30 && result.face_box.y1 == 210);
    assert(fabsf(result.face_box.confidence - 0.9f) < 0.001f);
    result = parse_face_fixture(present, box, "unjudgeable");
    assert(result.status == JEV_UNJUDGEABLE && result.face_box.detected);

    result = parse_face_fixture("{\"type\":\"noul\",\"noul\":0.499999999}", "null", "prone");
    assert(result.face_status == JEV_OK && !result.face_box.detected && result.is_prone);
    result = parse_face_fixture(present,
        "{\"type\":\"box\",\"box\":{\"left\":0,\"right\":1,\"top\":0,\"bottom\":1},\"confidence\":0}", "prone");
    assert(result.face_status == JEV_OK && result.face_box.detected);
    assert(result.face_box.x0 == 0 && result.face_box.y0 == 0);
    assert(result.face_box.x1 == 319 && result.face_box.y1 == 239);

    const char *invalid_presence[] = {"null", "{}", "{\"type\":\"box\",\"noul\":1}",
        "{\"type\":\"noul\",\"noul\":\"1\"}", "{\"type\":\"noul\",\"noul\":-0.1}",
        "{\"type\":\"noul\",\"noul\":1.1}", "{\"type\":\"noul\",\"noul\":1e999}"};
    for (unsigned i = 0; i < sizeof(invalid_presence)/sizeof(*invalid_presence); ++i) {
        result = parse_face_fixture(invalid_presence[i], box, "prone");
        assert(result.status == JEV_OK && result.is_prone);
        assert(result.face_status == JEV_INVALID_RESPONSE && !result.face_box.detected);
    }
    const char *invalid_boxes[] = {"null", "{}", "{\"type\":\"noul\"}"};
    for (unsigned i = 0; i < sizeof(invalid_boxes)/sizeof(*invalid_boxes); ++i) {
        result = parse_face_fixture(present, invalid_boxes[i], "prone");
        assert(result.status == JEV_OK && result.face_status == JEV_INVALID_RESPONSE);
        assert(!result.face_box.detected);
    }
    struct { const char *field; const char *value; } invalid_fields[] = {
        {"left", "-0.1"}, {"right", "1.1"}, {"top", "null"}, {"bottom", "\"0.8\""},
        {"left", "0.75"}, {"right", "0.1"}, {"top", "0.875"}, {"bottom", "0.1"},
        {"confidence", "null"}, {"confidence", "-0.1"}, {"confidence", "1.1"},
        {"bottom", "1e999"}, {"confidence", "1e999"},
    };
    for (unsigned i = 0; i < sizeof(invalid_fields)/sizeof(*invalid_fields); ++i) {
        cJSON *answer = cJSON_Parse(box);
        cJSON *target = !strcmp(invalid_fields[i].field, "confidence") ? answer :
            cJSON_GetObjectItemCaseSensitive(answer, "box");
        assert(cJSON_ReplaceItemInObjectCaseSensitive(target, invalid_fields[i].field,
            cJSON_Parse(invalid_fields[i].value)));
        char *json = cJSON_PrintUnformatted(answer);
        result = parse_face_fixture(present, json, "prone");
        assert(result.status == JEV_OK && result.is_prone);
        assert(result.face_status == JEV_INVALID_RESPONSE && !result.face_box.detected);
        cJSON_free(json);
        cJSON_Delete(answer);
    }
    result = parse_face_fixture(present,
        "{\"type\":\"box\",\"box\":{\"left\":0.25,\"right\":0.2501,\"top\":0.1,\"bottom\":0.9},\"confidence\":1}", "prone");
    assert(result.face_status == JEV_INVALID_RESPONSE && !result.face_box.detected);
}

static void test_request(void)
{
    cJSON *request = cJSON_Parse(jev_request_json);
    cJSON *questions = cJSON_GetObjectItemCaseSensitive(request, "questions");
    const char *ids[] = {"posture", "face_present", "face_box"};
    const char *types[] = {"choice", "noul", "box"};
    for (unsigned i = 0; i < 3; ++i) {
        cJSON *question = cJSON_GetObjectItemCaseSensitive(questions, ids[i]);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(question, "type");
        assert(cJSON_IsString(type) && !strcmp(type->valuestring, types[i]));
    }
    cJSON *photo = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(request, "state"), "photo");
    assert(cJSON_IsString(photo));
    cJSON *asset = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(request, "assets"), photo->valuestring);
    cJSON *part = cJSON_GetObjectItemCaseSensitive(asset, "part");
    assert(cJSON_IsString(part) && !strcmp(part->valuestring, "frame"));
    cJSON_Delete(request);
}

int main(void)
{
    test_schedule();
    test_classification();
    test_settings();
    test_face();
    test_request();
    puts("monitor tests passed");
}
