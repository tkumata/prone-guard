#include "pairing_frame.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static bool received(uint8_t endpoint, const uint8_t *payload, size_t size, void *context)
{
    unsigned *count = context;
    assert(endpoint == 1);
    assert(size == 4);
    assert(memcmp(payload, "\0\n\r\xff", 4) == 0);
    ++*count;
    return true;
}

int main(void)
{
    pairing_frame_t *frame = calloc(1, sizeof(*frame));
    assert(frame);
    uint8_t bytes[] = {0,0,0,5,1,0,10,13,255};
    unsigned count = 0;
    for (size_t i = 0; i < sizeof(bytes); ++i)
        assert(pairing_frame_feed(frame, bytes + i, 1, received, &count));
    assert(count == 1);
    assert(pairing_frame_feed(frame, bytes, sizeof(bytes), received, &count));
    assert(count == 2);
    uint8_t oversized[] = {0,0,0x80,0};
    assert(!pairing_frame_feed(frame, oversized, sizeof(oversized), received, &count));
    assert(!pairing_frame_feed(frame, bytes, sizeof(bytes), received, &count));
    memset(frame, 0, sizeof(*frame));
    uint8_t zero[] = {0,0,0,0};
    assert(!pairing_frame_feed(frame, zero, sizeof(zero), received, &count));
    memset(frame, 0, sizeof(*frame));
    assert(pairing_frame_feed(frame, bytes, sizeof(bytes)-1, received, &count));
    assert(count == 2);
    free(frame);
    return 0;
}
