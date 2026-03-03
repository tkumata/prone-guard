#include "pose_inference_bridge.h"

#include <cstring>
#include <list>

#include "coco_pose.hpp"
#include "dl_detect_define.hpp"
#include "dl_image_define.hpp"
#include "dl_image_jpeg.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "pose_inference";

static COCOPose *s_pose_model = nullptr;
static pose_inference_status_t s_status = POSE_INFERENCE_STATUS_NOT_READY;
static int64_t s_last_log_ms = 0;

esp_err_t pose_inference_init(void)
{
    if (s_pose_model != nullptr) {
        s_status = POSE_INFERENCE_STATUS_OK;
        return ESP_OK;
    }

    // COCOPose モデルを生成 (coco_pose コンポーネントが Flash パーティションから自動ロード)
    s_pose_model = new COCOPose();
    if (s_pose_model == nullptr) {
        s_status = POSE_INFERENCE_STATUS_FAULT;
        ESP_LOGE(TAG, "COCOPose 初期化失敗: メモリ不足");
        return ESP_ERR_NO_MEM;
    }

    s_status = POSE_INFERENCE_STATUS_OK;
    ESP_LOGI(TAG, "COCOPose モデルロード完了");
    return ESP_OK;
}

esp_err_t pose_inference_run_jpeg(
    const uint8_t *jpeg_data,
    size_t jpeg_len,
    pose_result_t *result)
{
    if (jpeg_data == nullptr || jpeg_len == 0 || result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_pose_model == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // デフォルト値をセット
    memset(result, 0, sizeof(pose_result_t));
    result->detected = false;

    // JPEG → RGB888 デコード
    dl::image::jpeg_img_t jpeg = {
        .data = (void *)jpeg_data,
        .data_len = jpeg_len,
    };
    dl::image::img_t rgb = dl::image::sw_decode_jpeg(jpeg, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (rgb.data == nullptr) {
        s_status = POSE_INFERENCE_STATUS_FAULT;
        ESP_LOGE(TAG, "JPEG デコード失敗");
        return ESP_FAIL;
    }

    // 推論実行
    std::list<dl::detect::result_t> &results = s_pose_model->run(rgb);

    // スコア最大の 1 人を選択
    float best_score = 0.0f;
    const dl::detect::result_t *best_result = nullptr;
    for (const auto &r : results) {
        if (r.score > best_score) {
            best_score = r.score;
            best_result = &r;
        }
    }

    if (best_result != nullptr && best_score > 0.0f) {
        result->detected = true;
        result->score = best_score;

        // バウンディングボックスをコピー
        if (best_result->box.size() >= 4) {
            result->bbox[0] = best_result->box[0];
            result->bbox[1] = best_result->box[1];
            result->bbox[2] = best_result->box[2];
            result->bbox[3] = best_result->box[3];
        }

        // キーポイントをコピー (17 x 2 = 34)
        size_t kp_count = best_result->keypoint.size();
        if (kp_count > 34) {
            kp_count = 34;
        }
        for (size_t i = 0; i < kp_count; i++) {
            result->keypoints[i] = best_result->keypoint[i];
        }
    }

    // 定期ログ (1 秒間隔)
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_log_ms >= 1000) {
        s_last_log_ms = now_ms;
        ESP_LOGI(TAG,
                 "pose: candidates=%d best=%.3f detected=%d bbox=[%d,%d,%d,%d]",
                 (int)results.size(),
                 (double)best_score,
                 result->detected ? 1 : 0,
                 result->bbox[0], result->bbox[1],
                 result->bbox[2], result->bbox[3]);
    }

    heap_caps_free(rgb.data);
    s_status = POSE_INFERENCE_STATUS_OK;
    return ESP_OK;
}

pose_inference_status_t pose_inference_get_status(void)
{
    return s_status;
}
