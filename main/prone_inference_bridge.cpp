#include "prone_inference_bridge.h"

#include <math.h>
#include <map>
#include <string.h>
#include <string>
#include <vector>

#include "dl_image_define.hpp"
#include "dl_image_jpeg.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "prone_inference";

extern const uint8_t _binary_prone_espdl_start[] asm("_binary_prone_espdl_start");
extern const uint8_t _binary_prone_espdl_end[] asm("_binary_prone_espdl_end");

static dl::Model *s_model;
static dl::image::ImagePreprocessor *s_preprocessor;
static prone_inference_status_t s_status = PRONE_INFERENCE_STATUS_NOT_READY;
static std::string s_input_name;
static std::string s_output_name;

static dl::TensorBase *get_first_input(dl::Model *model, std::string *name)
{
    std::map<std::string, dl::TensorBase *> &inputs = model->get_inputs();
    if (inputs.empty()) {
        return nullptr;
    }
    auto it = inputs.begin();
    if (name != nullptr) {
        *name = it->first;
    }
    return it->second;
}

static dl::TensorBase *get_first_output(dl::Model *model, std::string *name)
{
    std::map<std::string, dl::TensorBase *> &outputs = model->get_outputs();
    if (outputs.empty()) {
        return nullptr;
    }
    auto it = outputs.begin();
    if (name != nullptr) {
        *name = it->first;
    }
    return it->second;
}

static float read_tensor_value(dl::TensorBase *tensor, int index)
{
    if (index < 0 || index >= tensor->get_size()) {
        return 0.0f;
    }

    switch (tensor->get_dtype()) {
    case dl::DATA_TYPE_FLOAT:
        return tensor->get_element_ptr<float>()[index];
    case dl::DATA_TYPE_INT8:
        return dl::dequantize<int8_t, float>(tensor->get_element_ptr<int8_t>()[index], DL_SCALE(tensor->get_exponent()));
    case dl::DATA_TYPE_INT16:
        return dl::dequantize<int16_t, float>(tensor->get_element_ptr<int16_t>()[index],
                                              DL_SCALE(tensor->get_exponent()));
    case dl::DATA_TYPE_UINT8:
        return (float)tensor->get_element_ptr<uint8_t>()[index];
    case dl::DATA_TYPE_UINT16:
        return (float)tensor->get_element_ptr<uint16_t>()[index];
    case dl::DATA_TYPE_INT32:
        return (float)tensor->get_element_ptr<int32_t>()[index];
    default:
        return 0.0f;
    }
}

static void decode_output(dl::TensorBase *output, bool *is_prone, float *confidence)
{
    int output_size = output->get_size();
    if (output_size >= 2) {
        float non_prone_logit = read_tensor_value(output, 0);
        float prone_logit = read_tensor_value(output, 1);
        float max_logit = (non_prone_logit > prone_logit) ? non_prone_logit : prone_logit;
        float exp0 = expf(non_prone_logit - max_logit);
        float exp1 = expf(prone_logit - max_logit);
        float denom = exp0 + exp1;
        *confidence = (denom > 0.0f) ? (exp1 / denom) : 0.0f;
        *is_prone = prone_logit >= non_prone_logit;
        return;
    }

    float score = read_tensor_value(output, 0);
    *confidence = 1.0f / (1.0f + expf(-score));
    *is_prone = *confidence >= 0.5f;
}

esp_err_t prone_inference_init(void)
{
    if (s_model != nullptr && s_preprocessor != nullptr) {
        s_status = PRONE_INFERENCE_STATUS_OK;
        return ESP_OK;
    }

    const uint8_t *model_start = &_binary_prone_espdl_start[0];
    const uint8_t *model_end = &_binary_prone_espdl_end[0];
    if (model_end <= model_start) {
        s_status = PRONE_INFERENCE_STATUS_MODEL_MISSING;
        ESP_LOGE(TAG, "prone.espdl が埋め込まれていません");
        return ESP_ERR_NOT_FOUND;
    }

    s_model = new dl::Model((const char *)model_start);
    if (s_model == nullptr) {
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        return ESP_ERR_NO_MEM;
    }

    dl::TensorBase *input = get_first_input(s_model, &s_input_name);
    if (input == nullptr || input->shape.size() < 4) {
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        ESP_LOGE(TAG, "モデル入力形状が不正です");
        return ESP_FAIL;
    }

    int channels = input->shape[3];
    if (channels <= 0) {
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        ESP_LOGE(TAG, "モデル入力チャネル数が不正です");
        return ESP_FAIL;
    }

    std::vector<float> mean(channels, 0.0f);
    std::vector<float> std(channels, 255.0f);
    s_preprocessor = new dl::image::ImagePreprocessor(s_model, mean, std, 0, s_input_name);
    if (s_preprocessor == nullptr) {
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        return ESP_ERR_NO_MEM;
    }

    dl::TensorBase *output = get_first_output(s_model, &s_output_name);
    if (output == nullptr || output->get_size() <= 0) {
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        ESP_LOGE(TAG, "モデル出力が取得できません");
        return ESP_FAIL;
    }

    s_status = PRONE_INFERENCE_STATUS_OK;
    ESP_LOGI(TAG, "推論モデル読み込み完了 input=%s output=%s", s_input_name.c_str(), s_output_name.c_str());
    return ESP_OK;
}

esp_err_t prone_inference_run_jpeg(const uint8_t *jpeg_data, size_t jpeg_len, bool *is_prone, float *confidence)
{
    if (jpeg_data == nullptr || jpeg_len == 0 || is_prone == nullptr || confidence == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_model == nullptr || s_preprocessor == nullptr) {
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

    s_preprocessor->preprocess(rgb);
    s_model->run();

    dl::TensorBase *output = s_model->get_output(s_output_name);
    if (output == nullptr || output->get_size() <= 0) {
        heap_caps_free(rgb.data);
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        return ESP_FAIL;
    }

    decode_output(output, is_prone, confidence);
    heap_caps_free(rgb.data);
    s_status = PRONE_INFERENCE_STATUS_OK;
    return ESP_OK;
}

prone_inference_status_t prone_inference_get_status(void)
{
    return s_status;
}
