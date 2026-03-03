#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 推論ステータス
typedef enum {
    POSE_INFERENCE_STATUS_NOT_READY = 0,
    POSE_INFERENCE_STATUS_OK,
    POSE_INFERENCE_STATUS_MODEL_MISSING,
    POSE_INFERENCE_STATUS_FAULT,
} pose_inference_status_t;

// 推論結果
typedef struct {
    int keypoints[34];   // 17 キーポイント x 2 (x, y)
    int bbox[4];         // [x1, y1, x2, y2]
    float score;         // 検出信頼度
    bool detected;       // 人物検出フラグ
} pose_result_t;

// 初期化 (COCOPose モデルロード)
esp_err_t pose_inference_init(void);

// JPEG → RGB888 デコード → 推論実行
esp_err_t pose_inference_run_jpeg(
    const uint8_t *jpeg_data,
    size_t jpeg_len,
    pose_result_t *result
);

// 現在のステータス取得
pose_inference_status_t pose_inference_get_status(void);

#ifdef __cplusplus
}
#endif
