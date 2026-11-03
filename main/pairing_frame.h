#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAIRING_FRAME_LIMIT 32768
#define PAIRING_BODY_LIMIT (PAIRING_FRAME_LIMIT - 4)

typedef struct
{
    uint8_t header[4];
    size_t header_used;
    uint32_t expected;
    size_t used;
    bool failed;
    uint8_t body[PAIRING_BODY_LIMIT];
} pairing_frame_t;

typedef bool (*pairing_frame_handler_t)(uint8_t endpoint, const uint8_t *payload, size_t size,
                                        void *context);

bool pairing_frame_feed(pairing_frame_t *frame, const uint8_t *data, size_t size,
                        pairing_frame_handler_t handler, void *context);
