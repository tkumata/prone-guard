#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PRONE_INFERENCE_STATUS_NOT_READY = 0,
    PRONE_INFERENCE_STATUS_OK,
    PRONE_INFERENCE_STATUS_MODEL_MISSING,
    PRONE_INFERENCE_STATUS_FAULT,
} prone_inference_status_t;

typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
    float confidence;
    bool valid;
} prone_face_box_t;

esp_err_t prone_inference_init(void);
esp_err_t prone_inference_run_jpeg(const uint8_t *jpeg_data,
                                   size_t jpeg_len,
                                   bool *is_face_detected,
                                   float *confidence);
prone_inference_status_t prone_inference_get_status(void);
esp_err_t prone_inference_get_last_face_box(prone_face_box_t *out_box);

#ifdef __cplusplus
}
#endif
