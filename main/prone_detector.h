#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// うつ伏せ判定ステータス
typedef enum {
    PRONE_STATUS_UNKNOWN = 0,
    PRONE_STATUS_NOT_PRONE,
    PRONE_STATUS_PRONE,
} prone_status_t;

// うつ伏せ判定結果
typedef struct {
    prone_status_t status;
    float score;          // 0.0 ~ 1.0
    int64_t held_ms;      // 現在のホールド継続時間 (ms)
} prone_result_t;

// 定数定義 (SPECIFICATIONS.md 4.2 章)
#define PRONE_SCORE_THRESHOLD    0.6f
#define PRONE_CLEAR_THRESHOLD    0.4f
#define PRONE_HOLD_MS            1500
#define PRONE_CLEAR_MS           1000
#define PERSON_MISSING_FAULT_MS  3000
#define INFERENCE_INTERVAL_MS    500
#define FACE_INVISIBLE_SCORE     0.4f
#define EAR_SHOULDER_SCORE       0.3f
#define EAR_SHOULDER_HALF_SCORE  0.15f
#define SHOULDER_HIP_SCORE       0.3f
#define SHOULDER_HIP_RATIO       0.15f

// キーポイントインデックス (COCO Keypoints)
#define KP_NOSE            0
#define KP_LEFT_EYE        1
#define KP_RIGHT_EYE       2
#define KP_LEFT_EAR        3
#define KP_RIGHT_EAR       4
#define KP_LEFT_SHOULDER   5
#define KP_RIGHT_SHOULDER  6
#define KP_LEFT_ELBOW      7
#define KP_RIGHT_ELBOW     8
#define KP_LEFT_WRIST      9
#define KP_RIGHT_WRIST     10
#define KP_LEFT_HIP        11
#define KP_RIGHT_HIP       12
#define KP_LEFT_KNEE       13
#define KP_RIGHT_KNEE      14
#define KP_LEFT_ANKLE      15
#define KP_RIGHT_ANKLE     16

// キーポイント座標取得マクロ (keypoints[34] から x, y を取得)
#define KP_X(kp, idx) ((kp)[2 * (idx)])
#define KP_Y(kp, idx) ((kp)[2 * (idx) + 1])

/**
 * うつ伏せ判定を実行する。
 * 内部 static 変数でホールドタイマーの状態を保持する。
 *
 * @param keypoints 17 キーポイント x 2 座標 (34 要素)
 * @param bbox      バウンディングボックス [x1, y1, x2, y2]
 * @param image_width  画像幅 (ピクセル)
 * @param image_height 画像高さ (ピクセル)
 * @return prone_result_t 判定結果
 */
prone_result_t prone_check(
    const int keypoints[34],
    const int bbox[4],
    int image_width,
    int image_height
);

/**
 * うつ伏せ判定の内部状態をリセットする。
 */
void prone_detector_reset(void);

#ifdef __cplusplus
}
#endif
