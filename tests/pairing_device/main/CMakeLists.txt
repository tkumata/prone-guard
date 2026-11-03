idf_component_register(
    SRCS "main.c" "../../../main/pairing_frame.c" "../../../main/pairing_usb.c"
    INCLUDE_DIRS "../../../main"
    REQUIRES protocomm esp_driver_gpio esp_driver_usb_serial_jtag esp_timer esp_event mbedtls cjson
)
