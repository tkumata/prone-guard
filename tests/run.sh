#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Source ESP-IDF export.sh first}"
test_binary=$(mktemp "${TMPDIR:-/tmp}/prone-monitor-test.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -fsanitize=address,undefined -DCJSON_NESTING_LIMIT=16 \
  -I"${IDF_BUILD_DIR:-build}/config" -Imain -Imanaged_components/espressif__cjson/cJSON \
  -I"$IDF_PATH/components/esp_common/include" -I"$IDF_PATH/components/http_parser" \
  main/telemetry.c main/monitor_settings_json.c "$IDF_PATH/components/http_parser/http_parser.c" \
  managed_components/espressif__cjson/cJSON/cJSON.c tests/test_monitor.c \
  -lm -o "$test_binary"
"$test_binary" "$@"
cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -fsanitize=address,undefined -DCJSON_NESTING_LIMIT=16 \
  -Itests/fakes -I"${IDF_BUILD_DIR:-build}/config" -Imain -Imanaged_components/espressif__cjson/cJSON \
  -I"$IDF_PATH/components/esp_common/include" -I"$IDF_PATH/components/http_parser" \
  main/monitor_settings.c main/monitor_settings_json.c "$IDF_PATH/components/http_parser/http_parser.c" \
  managed_components/espressif__cjson/cJSON/cJSON.c tests/test_monitor_settings.c \
  -lm -o "$test_binary"
"$test_binary"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Imain \
  main/pairing_frame.c tests/test_pairing_frame.c -o "$test_binary"
"$test_binary"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Imain \
  main/depth_map.c tests/test_depth_map.c -o "$test_binary"
"$test_binary"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Itests/fakes -Imain -I"${IDF_BUILD_DIR:-build}/config" \
  -I"$IDF_PATH/components/esp_common/include" \
  main/tft_display.c tests/test_tft_display.c -o "$test_binary"
"$test_binary"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"${IDF_BUILD_DIR:-build}/config" -Imain -I"$IDF_PATH/components/esp_common/include" \
  tests/test_face_rotation.c -o "$test_binary"
"$test_binary"
