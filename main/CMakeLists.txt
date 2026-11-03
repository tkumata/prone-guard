idf_component_register(
    SRCS "main.c" "prone_inference_bridge.cpp" "monitor_settings.c" "monitor_settings_json.c"
         "device_security.c" "pairing_frame.c" "pairing_usb.c" "telemetry.c" "depth_sensor.c"
         "depth_map.c" "oled_display.c" "edge_sender.c" "vendor/vl53l5cx/vl53l5cx_api.c"
    INCLUDE_DIRS "." "vendor/vl53l5cx"
    REQUIRES esp_https_server esp_http_client esp_driver_i2c esp_driver_gpio nvs_flash nvs_sec_provider
             esp_wifi esp_event esp_netif esp_timer esp_system esp_driver_usb_serial_jtag protocomm mbedtls http_parser esp32-camera human_face_detect esp-dl cjson
    EMBED_TXTFILES "index.html"
)
