idf_component_register(
    SRCS "main.c" "prone_inference_bridge.cpp" "monitor_settings.c" "monitor_settings_json.c" "jev_protocol.c" "jev_client.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html" "gts_root_r4.pem"
)
