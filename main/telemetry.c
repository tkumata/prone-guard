#include "telemetry.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

double telemetry_face_area(int x0, int y0, int x1, int y1)
{
    x0 = x0 < 0 ? 0 : x0 > 320 ? 320 : x0;
    x1 = x1 < 0 ? 0 : x1 > 320 ? 320 : x1;
    y0 = y0 < 0 ? 0 : y0 > 240 ? 240 : y0;
    y1 = y1 < 0 ? 0 : y1 > 240 ? 240 : y1;
    return x1 > x0 && y1 > y0 ? (double)(x1 - x0) * (y1 - y0) : 0.0;
}

static cJSON *map(const sensor_observation_t *observation, bool status)
{
    cJSON *rows = cJSON_CreateArray();
    if (!rows)
        return NULL;
    for (int y = 0; y < 8; ++y)
    {
        cJSON *row = cJSON_CreateArray();
        if (!row || !cJSON_AddItemToArray(rows, row))
        {
            cJSON_Delete(row);
            cJSON_Delete(rows);
            return NULL;
        }
        for (int x = 0; x < 8; ++x)
        {
            cJSON *value = cJSON_CreateNumber(status ? observation->target_status[y * 8 + x]
                                                     : observation->distance[y * 8 + x]);
            if (!value || !cJSON_AddItemToArray(row, value))
            {
                cJSON_Delete(value);
                cJSON_Delete(rows);
                return NULL;
            }
        }
    }
    return rows;
}

static bool add(cJSON *parent, const char *name, cJSON *value)
{
    if (value && cJSON_AddItemToObject(parent, name, value))
        return true;
    cJSON_Delete(value);
    return false;
}

char *telemetry_encode(const sensor_observation_t *o, const char *boot_id, const char *bed_id)
{
    if (!boot_id[0] || !bed_id[0] || !monitor_id_valid(boot_id) || !monitor_id_valid(bed_id) ||
        o->sequence > INT64_MAX ||
        (o->camera_ok &&
         (!isfinite(o->face_area_px2) || o->face_area_px2 < 0 || o->face_area_px2 > 76800)))
        return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;
    cJSON *camera = cJSON_AddObjectToObject(root, "camera");
    cJSON *depth = cJSON_AddObjectToObject(root, "depth");
    if (!camera || !depth)
    {
        cJSON_Delete(root);
        return NULL;
    }
    bool ok = true;
    char number[32];
    snprintf(number, sizeof(number), "%llu", (unsigned long long)o->sequence);
    ok = ok && add(root, "sequence", cJSON_CreateRaw(number)) &&
         cJSON_AddStringToObject(root, "boot_id", boot_id) &&
         cJSON_AddStringToObject(root, "bed_id", bed_id) &&
         cJSON_AddNumberToObject(root, "timestamp", o->timestamp) &&
         cJSON_AddStringToObject(camera, "status", o->camera_ok ? "ok" : "error") &&
         add(camera, "timestamp",
             o->camera_ok ? cJSON_CreateNumber(o->camera_timestamp) : cJSON_CreateNull()) &&
         add(camera, "face_detected",
             o->camera_ok ? cJSON_CreateNumber(o->face_detected ? 1 : 0) : cJSON_CreateNull());
    if (o->camera_ok && o->face_detected)
    {
        bool box_valid = isfinite(o->face_confidence) &&
                         o->face_confidence >= 0.0 && o->face_confidence <= 1.0 &&
                         o->face_box[0] >= 0 && o->face_box[1] >= 0 &&
                         o->face_box[2] <= 320 && o->face_box[3] <= 240 &&
                         o->face_box[2] > o->face_box[0] &&
                         o->face_box[3] > o->face_box[1];
        if (box_valid)
            ok = ok && cJSON_AddNumberToObject(camera, "face_confidence", o->face_confidence) &&
             add(camera, "face_box", cJSON_CreateIntArray(o->face_box, 4));
        if (box_valid && o->face_landmarks_valid)
        {
            cJSON *points = cJSON_CreateArray();
            if (!points)
                ok = false;
            for (int i = 0; i < 5 && points; ++i)
            {
                int point[2] = {o->face_landmarks[2 * i], o->face_landmarks[2 * i + 1]};
                if (point[0] < 0 || point[0] > 320 || point[1] < 0 || point[1] > 240 ||
                    !cJSON_AddItemToArray(points, cJSON_CreateIntArray(point, 2)))
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
                ok = add(camera, "face_landmarks", points);
            else
                cJSON_Delete(points);
        }
    }
    snprintf(number, sizeof(number), "%.2f", o->face_detected ? o->face_area_px2 : 0.0);
    ok =
        ok &&
        add(camera, "face_area_px2", o->camera_ok ? cJSON_CreateRaw(number) : cJSON_CreateNull()) &&
        cJSON_AddStringToObject(depth, "status", o->depth_ok ? "ok" : "error") &&
        add(depth, "timestamp",
            o->depth_ok ? cJSON_CreateNumber(o->depth_timestamp) : cJSON_CreateNull()) &&
        cJSON_AddStringToObject(depth, "unit", "mm");
    const int resolution[] = {8, 8};
    ok = ok && add(depth, "resolution", cJSON_CreateIntArray(resolution, 2)) &&
         add(depth, "distance", o->depth_ok ? map(o, false) : cJSON_CreateNull()) &&
         add(depth, "target_status", o->depth_ok ? map(o, true) : cJSON_CreateNull());
    char *json = NULL;
    if (ok)
    {
        json = cJSON_malloc(TELEMETRY_MAX_BYTES);
        if (json && !cJSON_PrintPreallocated(root, json, TELEMETRY_MAX_BYTES - 2, false))
        {
            cJSON_free(json);
            json = NULL;
        }
        if (json)
            strcat(json, "\n");
    }
    cJSON_Delete(root);
    return json;
}
