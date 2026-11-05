#include "prone_inference_bridge.h"
#include "face_rotation.h"

#include <stdint.h>
#include <new>

#include "dl_image_define.hpp"
#include "dl_image_jpeg.hpp"
#include "dl_image_process.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "human_face_detect.hpp"

#if !CONFIG_FLASH_HUMAN_FACE_DETECT_MSRMNP_S8_V1 || !CONFIG_FLASH_ESPDET_PICO_416_416_FACE
#error "Enable MSR + MNP and ESPDet-Pico 416 in sdkconfig; both models are required."
#endif

static const char *TAG = "prone_inference";
static constexpr float DETECTOR_MSR_SCORE_TH = 0.30f;
static constexpr float DETECTOR_MSR_NMS_TH = 0.45f;
static constexpr float DETECTOR_MNP_SCORE_TH = 0.35f;
static constexpr float DETECTOR_MNP_NMS_TH = 0.45f;
static constexpr float FACE_DETECTED_SCORE_TH = 0.35f;

static dl::detect::Detect *s_detector;
static bool s_use_espdet_pico_416;
static prone_inference_status_t s_status = PRONE_INFERENCE_STATUS_NOT_READY;
static prone_face_box_t s_last_face_box = {
    .x0 = -1,
    .y0 = -1,
    .x1 = -1,
    .y1 = -1,
    .confidence = 0.0f,
    .valid = false,
    .landmarks = {0},
    .landmarks_valid = false,
};

esp_err_t prone_inference_init(bool use_espdet_pico_416)
{
    if (s_detector != nullptr) {
        return use_espdet_pico_416 == s_use_espdet_pico_416 ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    const char *name = use_espdet_pico_416 ? "ESPDet-Pico 416" : "MSRMNP";
    if (use_espdet_pico_416) {
        s_detector = new (std::nothrow) human_face_detect::ESPDet(
            "espdet_pico_416_416_face.espdl",
            human_face_detect::ESPDet::default_score_thr,
            human_face_detect::ESPDet::default_nms_thr);
    } else {
        s_detector = new (std::nothrow) human_face_detect::MSRMNP(
            "human_face_detect_msr_s8_v1.espdl", DETECTOR_MSR_SCORE_TH, DETECTOR_MSR_NMS_TH,
            "human_face_detect_mnp_s8_v1.espdl", DETECTOR_MNP_SCORE_TH, DETECTOR_MNP_NMS_TH);
    }
    if (s_detector == nullptr) {
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        ESP_LOGE(TAG, "推論モデル初期化失敗 detector=%s", name);
        return ESP_ERR_NO_MEM;
    }
    s_use_espdet_pico_416 = use_espdet_pico_416;
    s_status = PRONE_INFERENCE_STATUS_OK;
    ESP_LOGI(TAG, "推論モデル読み込み完了 detector=%s rotation=ccw90", name);
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

    // Keep native VGA detail; restore public coordinates after inference.
    if (rgb.width != PRONE_CAMERA_WIDTH || rgb.height != PRONE_CAMERA_HEIGHT) {
        dl::image::img_t vga = {
            .data = heap_caps_malloc(PRONE_CAMERA_WIDTH * PRONE_CAMERA_HEIGHT * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
            .width = PRONE_CAMERA_WIDTH,
            .height = PRONE_CAMERA_HEIGHT,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
        };
        if (vga.data == nullptr) {
            heap_caps_free(rgb.data);
            s_status = PRONE_INFERENCE_STATUS_FAULT;
            return ESP_ERR_NO_MEM;
        }
        dl::image::ImageTransformer transformer;
        esp_err_t err = transformer.set_src_img(rgb).set_dst_img(vga).transform();
        heap_caps_free(rgb.data);
        if (err != ESP_OK) {
            heap_caps_free(vga.data);
            s_status = PRONE_INFERENCE_STATUS_FAULT;
            return err;
        }
        rgb = vga;
    }

    // ESPDet letterboxes the portrait image internally.
    const int pad_x = s_use_espdet_pico_416 ? 0 : PRONE_FACE_PAD_X;
    const int pad_y = s_use_espdet_pico_416 ? 0 : PRONE_FACE_PAD_Y;
    const uint16_t width = PRONE_CAMERA_HEIGHT + 2 * pad_x;
    const uint16_t height = PRONE_CAMERA_WIDTH + 2 * pad_y;
    dl::image::img_t upright = {
        .data = heap_caps_malloc(width * height * 3,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .width = width,
        .height = height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
    };
    if (upright.data == nullptr) {
        heap_caps_free(rgb.data);
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    if (s_use_espdet_pico_416) {
        prone_face_rotate_rgb888(static_cast<const uint8_t *>(rgb.data),
                                 static_cast<uint8_t *>(upright.data));
    } else {
        dl::math::Matrix<float> inverse(2, 3, 0.0f);
        inverse.array[0][1] = -1;
        inverse.array[0][2] = PRONE_CAMERA_WIDTH - 1 + pad_y;
        inverse.array[1][0] = 1;
        inverse.array[1][2] = -pad_x;
        dl::image::ImageTransformer transformer;
        err = transformer.set_src_img(rgb).set_dst_img(upright)
            .set_warp_affine_matrix(inverse, true)
            .set_bg_value(std::array<uint8_t, 3>{0, 0, 0}).transform();
    }
    heap_caps_free(rgb.data);
    if (err != ESP_OK) {
        heap_caps_free(upright.data);
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        return err;
    }
    rgb = upright;

    std::list<dl::detect::result_t> &result = s_detector->run(rgb);

    float best = 0.0f;
    int best_x0 = -1;
    int best_y0 = -1;
    int best_x1 = -1;
    int best_y1 = -1;
    std::vector<int> best_keypoint;
    for (const auto &r : result) {
        if (r.score > best) {
            best = r.score;
            if (r.box.size() >= 4) {
                best_x0 = r.box[0];
                best_y0 = r.box[1];
                best_x1 = r.box[2];
                best_y1 = r.box[3];
            }
            best_keypoint = r.keypoint;
        }
    }

    *confidence = best;
    *is_face_detected = best >= (s_use_espdet_pico_416 ? human_face_detect::ESPDet::default_score_thr
                                                    : FACE_DETECTED_SCORE_TH);
    s_last_face_box.x0 = best_x0;
    s_last_face_box.y0 = best_y0;
    s_last_face_box.x1 = best_x1;
    s_last_face_box.y1 = best_y1;
    s_last_face_box.confidence = best;
    s_last_face_box.valid = (*is_face_detected) && (best_x0 >= 0) && (best_y0 >= 0) &&
                            (best_x1 > best_x0) && (best_y1 > best_y0);

    // ランドマーク座標を抽出 (5点 x 2座標 = 10要素)
    if (best_keypoint.size() >= PRONE_LANDMARK_COUNT * 2) {
        for (int i = 0; i < PRONE_LANDMARK_COUNT * 2; i++) {
            s_last_face_box.landmarks[i] = best_keypoint[i];
        }
        s_last_face_box.landmarks_valid = s_last_face_box.valid;
    } else {
        memset(s_last_face_box.landmarks, 0, sizeof(s_last_face_box.landmarks));
        s_last_face_box.landmarks_valid = false;
    }

    prone_face_box_restore_camera_orientation(&s_last_face_box, pad_x, pad_y);
    heap_caps_free(rgb.data);
    s_status = PRONE_INFERENCE_STATUS_OK;
    return ESP_OK;
}

prone_inference_status_t prone_inference_get_status(void)
{
    return s_status;
}

esp_err_t prone_inference_get_last_face_box(prone_face_box_t *out_box)
{
    if (out_box == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_box = s_last_face_box;
    return ESP_OK;
}
