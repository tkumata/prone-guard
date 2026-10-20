#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Source ESP-IDF export.sh first}"
test_binary=$(mktemp "${TMPDIR:-/tmp}/prone-monitor-test.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -fsanitize=address,undefined \
  -I"${IDF_BUILD_DIR:-build}/config" -Imain -Imanaged_components/espressif__cjson/cJSON \
  -I"$IDF_PATH/components/esp_common/include" \
  main/jev_protocol.c main/monitor_settings_json.c \
  managed_components/espressif__cjson/cJSON/cJSON.c tests/test_monitor.c \
  -lm -o "$test_binary"
"$test_binary"
