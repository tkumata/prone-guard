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

// MNP モデルが出力する5点ランドマークのインデックス定義
// keypoint 配列は [x0,y0, x1,y1, x2,y2, x3,y3, x4,y4] の 10 要素
#define PRONE_LANDMARK_COUNT 5
#define PRONE_LM_LEFT_EYE   0
#define PRONE_LM_RIGHT_EYE  1
#define PRONE_LM_NOSE       2
#define PRONE_LM_LEFT_MOUTH 3
#define PRONE_LM_RIGHT_MOUTH 4

typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
    float confidence;
    bool valid;
    // ランドマーク座標 (5点 x 2座標 = 10要素)
    // [left_eye_x, left_eye_y, right_eye_x, right_eye_y,
    //  nose_x, nose_y, left_mouth_x, left_mouth_y,
    //  right_mouth_x, right_mouth_y]
    int landmarks[PRONE_LANDMARK_COUNT * 2];
    bool landmarks_valid;
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
