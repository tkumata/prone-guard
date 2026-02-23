#include "prone_inference_bridge.h"

#include <stdint.h>

#include "dl_image_define.hpp"
#include "dl_image_jpeg.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "human_face_detect.hpp"

static const char *TAG = "prone_inference";

static HumanFaceDetect *s_detector;
static prone_inference_status_t s_status = PRONE_INFERENCE_STATUS_NOT_READY;
static int64_t s_last_decode_log_ms;

esp_err_t prone_inference_init(void)
{
    if (s_detector != nullptr) {
        s_status = PRONE_INFERENCE_STATUS_OK;
        return ESP_OK;
    }

    s_detector = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, true);
    if (s_detector == nullptr) {
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        ESP_LOGE(TAG, "HumanFaceDetect 初期化失敗");
        return ESP_ERR_NO_MEM;
    }

    // 検出率重視で初期値はデフォルト運用。必要に応じて現地ログで再調整する。
    s_detector->set_score_thr(0.50f, 0);
    s_detector->set_nms_thr(0.50f, 0);
    s_detector->set_score_thr(0.50f, 1);
    s_detector->set_nms_thr(0.50f, 1);

    s_status = PRONE_INFERENCE_STATUS_OK;
    ESP_LOGI(TAG, "推論モデル読み込み完了 detector=HumanFaceDetect(MSRMNP_S8_V1)");
    return ESP_OK;
}

esp_err_t prone_inference_run_jpeg(const uint8_t *jpeg_data, size_t jpeg_len, bool *is_face_detected, float *confidence)
{
    if (jpeg_data == nullptr || jpeg_len == 0 || is_face_detected == nullptr || confidence == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_detector == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    dl::image::jpeg_img_t jpeg = {
        .data = (void *)jpeg_data,
        .data_len = jpeg_len,
    };
    dl::image::img_t rgb = dl::image::sw_decode_jpeg(jpeg, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (rgb.data == nullptr) {
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        return ESP_FAIL;
    }

    std::list<dl::detect::result_t> &result = s_detector->run(rgb);

    float best = 0.0f;
    int best_x0 = -1;
    int best_y0 = -1;
    int best_x1 = -1;
    int best_y1 = -1;
    for (const auto &r : result) {
        if (r.score > best) {
            best = r.score;
            if (r.box.size() >= 4) {
                best_x0 = r.box[0];
                best_y0 = r.box[1];
                best_x1 = r.box[2];
                best_y1 = r.box[3];
            }
        }
    }

    *confidence = best;
    *is_face_detected = (best >= 0.50f);

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_decode_log_ms >= 1000) {
        s_last_decode_log_ms = now_ms;
        ESP_LOGI(TAG,
                 "cascade decode: candidates=%d best=%.3f detected=%d box=[%d,%d,%d,%d]",
                 (int)result.size(),
                 (double)best,
                 (*is_face_detected) ? 1 : 0,
                 best_x0,
                 best_y0,
                 best_x1,
                 best_y1);
    }

    heap_caps_free(rgb.data);
    s_status = PRONE_INFERENCE_STATUS_OK;
    return ESP_OK;
}

prone_inference_status_t prone_inference_get_status(void)
{
    return s_status;
}
