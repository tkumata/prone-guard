#include "jev_protocol.h"
#include <math.h>
#include <string.h>
#include "cJSON.h"

const char jev_request_json[] =
    "{\"model\":\"jev-1.14.0\","
    "\"state\":{\"photo\":\"7a67ca37-401c-4bb9-8bfa-20df33a4d437\"},"
    "\"assets\":{\"7a67ca37-401c-4bb9-8bfa-20df33a4d437\":{\"type\":\"image\",\"part\":\"frame\"}},"
    "\"questions\":{\"posture\":{\"type\":\"choice\","
    "\"instructions\":\"Classify the infant's torso posture in `photo` relative to the supporting surface. "
    "Judge the torso, not the direction the face points. Use only visible evidence.\","
    "\"criteria\":{"
    "\"prone\":\"The infant is lying with the chest and abdomen toward the supporting surface and the back "
    "facing away from it. A head turned sideways does not change this classification.\","
    "\"not_prone\":\"The infant is visibly lying on the back or side, or visibly in another posture that is not prone.\","
    "\"unjudgeable\":\"No infant is visible; the intended infant cannot be uniquely identified; or occlusion, "
    "darkness, blur, framing, or an ambiguous transitional posture prevents distinguishing prone from other torso postures.\"}}}}";

const char *jev_status_name(jev_status_t status)
{
    switch (status) {
    case JEV_OK: return "ok";
    case JEV_PENDING: return "pending";
    case JEV_NOT_CONFIGURED: return "not_configured";
    case JEV_NETWORK_ERROR: return "network_error";
    case JEV_TIMEOUT: return "timeout";
    case JEV_HTTP_ERROR: return "http_error";
    case JEV_UNJUDGEABLE: return "unjudgeable";
    case JEV_CAMERA_ERROR: return "camera_error";
    case JEV_CLOCK_NOT_READY: return "clock_not_ready";
    default: return "invalid_response";
    }
}

jev_result_t jev_parse_response(const char *json, float threshold)
{
    jev_result_t result = {.status = JEV_INVALID_RESPONSE};
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    cJSON *answers = cJSON_GetObjectItemCaseSensitive(root, "answers");
    cJSON *posture = cJSON_GetObjectItemCaseSensitive(answers, "posture");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(posture, "type");
    cJSON *choice = cJSON_GetObjectItemCaseSensitive(posture, "choice");
    cJSON *probabilities = cJSON_GetObjectItemCaseSensitive(posture, "probabilities");
    const char *labels[] = {"prone", "not_prone", "unjudgeable"};
    double sum = 0;
    bool valid = cJSON_IsObject(root) && cJSON_IsObject(answers) && cJSON_IsObject(posture) &&
        cJSON_IsString(type) && !strcmp(type->valuestring, "choice") && cJSON_IsString(choice) &&
        cJSON_IsObject(probabilities) && isfinite(threshold) && threshold >= 0 && threshold <= 1;
    bool known_choice = false;
    for (unsigned i = 0; i < 3 && valid; ++i) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(probabilities, labels[i]);
        valid = cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
            value->valuedouble >= 0 && value->valuedouble <= 1;
        if (valid) {
            sum += value->valuedouble;
            if (i == 0) result.prone_probability = (float)value->valuedouble;
            if (!strcmp(choice->valuestring, labels[i])) known_choice = true;
        }
    }
    if (valid && known_choice && fabs(sum - 1.0) <= 0.01) {
        result.status = !strcmp(choice->valuestring, "unjudgeable") ? JEV_UNJUDGEABLE : JEV_OK;
        result.is_prone = result.status == JEV_OK && result.prone_probability >= threshold;
    }
    cJSON_Delete(root);
    return result;
}

bool jev_schedule_due(int64_t now_ms, int64_t *next_ms)
{
    if (now_ms < *next_ms) return false;
    *next_ms += ((now_ms - *next_ms) / JEV_INTERVAL_MS + 1) * JEV_INTERVAL_MS;
    return true;
}
