#include "pairing_frame.h"
#include <string.h>

bool pairing_frame_feed(pairing_frame_t *frame, const uint8_t *data, size_t size,
                        pairing_frame_handler_t handler, void *context)
{
    if (frame->failed || !handler || (!data && size))
        return false;
    while (size)
    {
        if (frame->header_used < 4)
        {
            frame->header[frame->header_used++] = *data++;
            --size;
            if (frame->header_used != 4)
                continue;
            frame->expected = ((uint32_t)frame->header[0] << 24) |
                              ((uint32_t)frame->header[1] << 16) |
                              ((uint32_t)frame->header[2] << 8) | frame->header[3];
            if (!frame->expected || frame->expected > PAIRING_BODY_LIMIT)
            {
                frame->failed = true;
                return false;
            }
        }
        size_t count = frame->expected - frame->used;
        if (count > size)
            count = size;
        memcpy(frame->body + frame->used, data, count);
        frame->used += count;
        data += count;
        size -= count;
        if (frame->used == frame->expected)
        {
            bool accepted = frame->body[0] <= 1 &&
                            handler(frame->body[0], frame->body + 1, frame->used - 1, context);
            memset(frame->body, 0, frame->used);
            frame->used = frame->header_used = frame->expected = 0;
            if (!accepted)
            {
                frame->failed = true;
                return false;
            }
        }
    }
    return true;
}
