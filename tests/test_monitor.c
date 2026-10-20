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

int main(void)
{
    test_schedule();
    test_classification();
    test_settings();
    puts("monitor tests passed");
}
