#include "prone_detector.h"

#include <stdbool.h>

#include "esp_timer.h"

// 内部状態
static prone_status_t s_current_status = PRONE_STATUS_UNKNOWN;
static int64_t s_prone_start_ms  = -1;  // うつ伏せ判定開始時刻
static int64_t s_clear_start_ms  = -1;  // 解除判定開始時刻

/**
 * キーポイントが不可視かどうかを判定する。
 * 座標が (0, 0) または bbox 外の場合に不可視とする。
 */
static bool is_keypoint_invisible(const int keypoints[34], int kp_idx, const int bbox[4])
{
    int x = KP_X(keypoints, kp_idx);
    int y = KP_Y(keypoints, kp_idx);

    // (0, 0) は ESP-DL が未検出のキーポイントに割り当てるデフォルト値
    if (x == 0 && y == 0) {
        return true;
    }

    // bbox 外
    if (x < bbox[0] || x > bbox[2] || y < bbox[1] || y > bbox[3]) {
        return true;
    }

    return false;
}

/**
 * 条件 A: 顔キーポイントの不可視性
 * 鼻・左目・右目のうち 2 個以上見えない場合: +0.4
 */
static float evaluate_condition_a(const int keypoints[34], const int bbox[4])
{
    int invisible_count = 0;

    if (is_keypoint_invisible(keypoints, KP_NOSE, bbox)) {
        invisible_count++;
    }
    if (is_keypoint_invisible(keypoints, KP_LEFT_EYE, bbox)) {
        invisible_count++;
    }
    if (is_keypoint_invisible(keypoints, KP_RIGHT_EYE, bbox)) {
        invisible_count++;
    }

    if (invisible_count >= 2) {
        return FACE_INVISIBLE_SCORE;
    }
    return 0.0f;
}

/**
 * 条件 B: 耳と肩の位置関係
 * 両耳の Y が肩中点 Y より上: +0.3 / 片耳のみ: +0.15
 */
static float evaluate_condition_b(const int keypoints[34], const int bbox[4])
{
    // 肩の Y 座標中点
    int shoulder_mid_y = (KP_Y(keypoints, KP_LEFT_SHOULDER) + KP_Y(keypoints, KP_RIGHT_SHOULDER)) / 2;

    // 肩が未検出 (両方 0,0) の場合は判定不能
    if (KP_X(keypoints, KP_LEFT_SHOULDER) == 0 && KP_Y(keypoints, KP_LEFT_SHOULDER) == 0 &&
        KP_X(keypoints, KP_RIGHT_SHOULDER) == 0 && KP_Y(keypoints, KP_RIGHT_SHOULDER) == 0) {
        return 0.0f;
    }

    bool left_ear_above = false;
    bool right_ear_above = false;

    // 左耳が可視かつ肩中点より上 (Y 値が小さい)
    if (!is_keypoint_invisible(keypoints, KP_LEFT_EAR, bbox)) {
        if (KP_Y(keypoints, KP_LEFT_EAR) < shoulder_mid_y) {
            left_ear_above = true;
        }
    }

    // 右耳が可視かつ肩中点より上
    if (!is_keypoint_invisible(keypoints, KP_RIGHT_EAR, bbox)) {
        if (KP_Y(keypoints, KP_RIGHT_EAR) < shoulder_mid_y) {
            right_ear_above = true;
        }
    }

    if (left_ear_above && right_ear_above) {
        return EAR_SHOULDER_SCORE;
    }
    if (left_ear_above || right_ear_above) {
        return EAR_SHOULDER_HALF_SCORE;
    }
    return 0.0f;
}

/**
 * 条件 C: 肩と腰の Y 座標差
 * 肩中点と腰中点の Y 差が画像高さの 15% 以下: +0.3
 */
static float evaluate_condition_c(const int keypoints[34], int image_height)
{
    // 肩が未検出の場合は判定不能
    if (KP_X(keypoints, KP_LEFT_SHOULDER) == 0 && KP_Y(keypoints, KP_LEFT_SHOULDER) == 0 &&
        KP_X(keypoints, KP_RIGHT_SHOULDER) == 0 && KP_Y(keypoints, KP_RIGHT_SHOULDER) == 0) {
        return 0.0f;
    }

    // 腰が未検出の場合は判定不能
    if (KP_X(keypoints, KP_LEFT_HIP) == 0 && KP_Y(keypoints, KP_LEFT_HIP) == 0 &&
        KP_X(keypoints, KP_RIGHT_HIP) == 0 && KP_Y(keypoints, KP_RIGHT_HIP) == 0) {
        return 0.0f;
    }

    int shoulder_mid_y = (KP_Y(keypoints, KP_LEFT_SHOULDER) + KP_Y(keypoints, KP_RIGHT_SHOULDER)) / 2;
    int hip_mid_y = (KP_Y(keypoints, KP_LEFT_HIP) + KP_Y(keypoints, KP_RIGHT_HIP)) / 2;

    int diff = shoulder_mid_y - hip_mid_y;
    if (diff < 0) {
        diff = -diff;
    }

    float threshold = (float)image_height * SHOULDER_HIP_RATIO;
    if ((float)diff <= threshold) {
        return SHOULDER_HIP_SCORE;
    }
    return 0.0f;
}

prone_result_t prone_check(
    const int keypoints[34],
    const int bbox[4],
    int image_width,
    int image_height)
{
    prone_result_t result = {
        .status = PRONE_STATUS_UNKNOWN,
        .score = 0.0f,
        .held_ms = 0,
    };

    (void)image_width;  // 現在の判定ロジックでは未使用

    int64_t now_ms = esp_timer_get_time() / 1000;

    // 条件 A + B + C の合計スコア算出
    float score_a = evaluate_condition_a(keypoints, bbox);
    float score_b = evaluate_condition_b(keypoints, bbox);
    float score_c = evaluate_condition_c(keypoints, image_height);
    float total_score = score_a + score_b + score_c;

    // スコアを 0.0 ~ 1.0 にクランプ
    if (total_score > 1.0f) {
        total_score = 1.0f;
    }
    result.score = total_score;

    // ホールドロジック
    if (total_score >= PRONE_SCORE_THRESHOLD) {
        // うつ伏せスコアが閾値以上
        s_clear_start_ms = -1;

        if (s_prone_start_ms < 0) {
            s_prone_start_ms = now_ms;
        }

        int64_t held = now_ms - s_prone_start_ms;
        result.held_ms = held;

        if (held >= PRONE_HOLD_MS) {
            result.status = PRONE_STATUS_PRONE;
            s_current_status = PRONE_STATUS_PRONE;
        } else {
            // ホールド時間未到達: 前回の状態を維持
            result.status = s_current_status;
        }
    } else if (total_score <= PRONE_CLEAR_THRESHOLD) {
        // 解除条件
        s_prone_start_ms = -1;

        if (s_current_status == PRONE_STATUS_PRONE) {
            // ALERT 状態からの解除: CLEAR タイマーで判定
            if (s_clear_start_ms < 0) {
                s_clear_start_ms = now_ms;
            }

            int64_t clear_held = now_ms - s_clear_start_ms;
            if (clear_held >= PRONE_CLEAR_MS) {
                result.status = PRONE_STATUS_NOT_PRONE;
                s_current_status = PRONE_STATUS_NOT_PRONE;
                s_clear_start_ms = -1;
            } else {
                // 解除待ち: PRONE 維持
                result.status = PRONE_STATUS_PRONE;
                result.held_ms = clear_held;
            }
        } else {
            result.status = PRONE_STATUS_NOT_PRONE;
            s_current_status = PRONE_STATUS_NOT_PRONE;
            s_clear_start_ms = -1;
        }
    } else {
        // 閾値の間 (0.4 < score < 0.6): 前回の状態を維持
        result.status = s_current_status;
        if (s_current_status == PRONE_STATUS_PRONE && s_prone_start_ms >= 0) {
            result.held_ms = now_ms - s_prone_start_ms;
        }
        // タイマーはリセットしない
    }

    return result;
}

void prone_detector_reset(void)
{
    s_current_status = PRONE_STATUS_UNKNOWN;
    s_prone_start_ms = -1;
    s_clear_start_ms = -1;
}
