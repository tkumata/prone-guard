#include "pairing_usb.h"
#include "pairing_frame.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/platform_util.h"
#include <stdlib.h>

typedef struct
{
    protocomm_t *protocol;
    uint32_t session_id;
    int64_t deadline;
    unsigned handshakes;
} session_t;

static bool write_bytes(const void *bytes, size_t size, int64_t deadline)
{
    const uint8_t *data = bytes;
    while (size && esp_timer_get_time() < deadline && usb_serial_jtag_is_connected())
    {
        int count = usb_serial_jtag_write_bytes(data, size, pdMS_TO_TICKS(20));
        if (count <= 0)
            return false;
        data += count;
        size -= count;
    }
    return !size && esp_timer_get_time() < deadline;
}

static bool handle_frame(uint8_t endpoint, const uint8_t *data, size_t size, void *context)
{
    session_t *session = context;
    if (esp_timer_get_time() >= session->deadline)
        return false;
    if ((endpoint == 0 && session->handshakes >= 2) || (endpoint == 1 && session->handshakes != 2))
        return false;
    uint8_t *response = NULL;
    ssize_t length = 0;
    esp_err_t err = protocomm_req_handle(session->protocol,
                                         endpoint == 0 ? "pairing-session" : "pairing-command",
                                         session->session_id, data, size, &response, &length);
    bool valid = err == ESP_OK && length >= 0 && length <= PAIRING_BODY_LIMIT - 1;
    if (valid && endpoint == 0 && ++session->handshakes == 2)
        session->deadline = esp_timer_get_time() + 180000000;
    if (valid)
    {
        uint32_t body_size = (uint32_t)length + 1;
        uint8_t header[5] = {body_size >> 24, body_size >> 16, body_size >> 8, body_size, endpoint};
        valid = write_bytes(header, sizeof(header), session->deadline) &&
                write_bytes(response, length, session->deadline);
    }
    if (response)
    {
        mbedtls_platform_zeroize(response, length > 0 ? length : 0);
        free(response);
    }
    return valid;
}

esp_err_t pairing_usb_init(void)
{
    usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 1024, .rx_buffer_size = 1024};
    return usb_serial_jtag_driver_install(&config);
}

esp_err_t pairing_usb_run(protocomm_t *protocol, uint32_t session_id, int64_t deadline_us)
{
    if (!protocol || deadline_us <= esp_timer_get_time())
        return ESP_ERR_INVALID_ARG;
    pairing_frame_t *frame = calloc(1, sizeof(*frame));
    if (!frame)
        return ESP_ERR_NO_MEM;
    session_t session = {.protocol = protocol, .session_id = session_id, .deadline = deadline_us};
    esp_err_t err = protocomm_open_session(protocol, session_id);
    if (err == ESP_OK)
    {
        while (usb_serial_jtag_is_connected() && esp_timer_get_time() < session.deadline)
        {
            uint8_t data[256];
            int count = usb_serial_jtag_read_bytes(data, sizeof(data), pdMS_TO_TICKS(20));
            if (count < 0 || (count > 0 && !pairing_frame_feed(frame, data, count, handle_frame, &session)))
            {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
        }
        esp_err_t closed = protocomm_close_session(protocol, session_id);
        if (err == ESP_OK)
            err = esp_timer_get_time() >= session.deadline ? ESP_ERR_TIMEOUT : closed;
    }
    mbedtls_platform_zeroize(frame, sizeof(*frame));
    free(frame);
    return err;
}
