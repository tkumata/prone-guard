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
    "darkness, blur, framing, or an ambiguous transitional posture prevents distinguishing prone from other torso postures.\"}},"
    "\"face_present\":{\"type\":\"noul\",\"instructions\":\"Is the face of the infant being monitored visible in `photo`? "
    "Answer no if the intended infant cannot be uniquely identified. Ignore other people's faces.\"},"
    "\"face_box\":{\"type\":\"box\",\"instructions\":\"Locate the visible face of the infant being monitored in `photo`. "
    "Enclose that infant's face, not the whole head or body, and ignore other people's faces.\"}}}";

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

static bool is_unit_number(const cJSON *value)
{
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
        value->valuedouble >= 0 && value->valuedouble <= 1;
}

static void parse_face(const cJSON *answers, jev_result_t *result)
{
    cJSON *presence = cJSON_GetObjectItemCaseSensitive(answers, "face_present");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(presence, "type");
    cJSON *probability = cJSON_GetObjectItemCaseSensitive(presence, "noul");
    if (!cJSON_IsObject(presence) || !cJSON_IsString(type) ||
        strcmp(type->valuestring, "noul") || !is_unit_number(probability)) return;
    if (probability->valuedouble < 0.5) {
        result->face_status = JEV_OK;
        return;
    }

    cJSON *answer = cJSON_GetObjectItemCaseSensitive(answers, "face_box");
    type = cJSON_GetObjectItemCaseSensitive(answer, "type");
    cJSON *box = cJSON_GetObjectItemCaseSensitive(answer, "box");
    cJSON *confidence = cJSON_GetObjectItemCaseSensitive(answer, "confidence");
    cJSON *left = cJSON_GetObjectItemCaseSensitive(box, "left");
    cJSON *right = cJSON_GetObjectItemCaseSensitive(box, "right");
    cJSON *top = cJSON_GetObjectItemCaseSensitive(box, "top");
    cJSON *bottom = cJSON_GetObjectItemCaseSensitive(box, "bottom");
    if (!cJSON_IsObject(answer) || !cJSON_IsString(type) || strcmp(type->valuestring, "box") ||
        !cJSON_IsObject(box) || !is_unit_number(confidence) || !is_unit_number(left) ||
        !is_unit_number(right) || !is_unit_number(top) || !is_unit_number(bottom) ||
        left->valuedouble >= right->valuedouble || top->valuedouble >= bottom->valuedouble) return;

    jev_face_box_t face = {
        .x0 = (int)fmin(left->valuedouble * 320, 319),
        .y0 = (int)fmin(top->valuedouble * 240, 239),
        .x1 = (int)fmin(right->valuedouble * 320, 319),
        .y1 = (int)fmin(bottom->valuedouble * 240, 239),
        .confidence = (float)confidence->valuedouble,
    };
    if (face.x1 <= face.x0 || face.y1 <= face.y0) return;
    face.detected = true;
    result->face_box = face;
    result->face_status = JEV_OK;
}

jev_result_t jev_parse_response(const char *json, float threshold)
{
    jev_result_t result = {.status = JEV_INVALID_RESPONSE, .face_status = JEV_INVALID_RESPONSE};
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
        parse_face(answers, &result);
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
