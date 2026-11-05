#pragma once

#include "prone_inference_bridge.h"

enum {
    PRONE_CAMERA_WIDTH = 640,
    PRONE_CAMERA_HEIGHT = 480,
    // The smallest centered 4:3 canvas that fits a 480x640 portrait image.
    PRONE_FACE_INPUT_HEIGHT = 642,
    PRONE_FACE_INPUT_WIDTH = PRONE_FACE_INPUT_HEIGHT * 4 / 3,
    PRONE_FACE_PAD_X = (PRONE_FACE_INPUT_WIDTH - PRONE_CAMERA_HEIGHT) / 2,
    PRONE_FACE_PAD_Y = (PRONE_FACE_INPUT_HEIGHT - PRONE_CAMERA_WIDTH) / 2,
};

static inline void prone_face_rotate_rgb888(const uint8_t *source, uint8_t *portrait)
{
    // Read short strips so adjacent columns can reuse the PSRAM cache lines.
    const int rows = 16;
    for (int start = 0; start < PRONE_CAMERA_HEIGHT; start += rows)
    {
        int end = start + rows < PRONE_CAMERA_HEIGHT ? start + rows : PRONE_CAMERA_HEIGHT;
        for (int x = PRONE_CAMERA_WIDTH - 1; x >= 0; --x)
        {
            uint8_t *target = portrait + ((PRONE_CAMERA_WIDTH - 1 - x) * PRONE_CAMERA_HEIGHT + start) * 3;
            for (int y = start; y < end; ++y)
            {
                const uint8_t *pixel = source + (y * PRONE_CAMERA_WIDTH + x) * 3;
                *target++ = pixel[0];
                *target++ = pixel[1];
                *target++ = pixel[2];
            }
        }
    }
}

static inline int face_rotation_clip(int coordinate, int maximum)
{
    return coordinate < 0 ? 0 : coordinate > maximum ? maximum : coordinate;
}

// Undo padding and CCW90 rotation, then restore the public 320x240 coordinates.
static inline void prone_face_box_restore_camera_orientation(prone_face_box_t *box,
                                                             int pad_x, int pad_y)
{
    if (!box->valid)
        return;

    int right = PRONE_CAMERA_WIDTH - 1 + pad_y;
    int x0 = box->x0, x1 = box->x1;
    box->x0 = face_rotation_clip((right - box->y1) / 2, 319);
    box->x1 = face_rotation_clip((right - box->y0) / 2, 319);
    box->y0 = face_rotation_clip((x0 - pad_x) / 2, 239);
    box->y1 = face_rotation_clip((x1 - pad_x) / 2, 239);
    box->valid = box->x1 > box->x0 && box->y1 > box->y0;
    box->landmarks_valid = box->landmarks_valid && box->valid;
    if (box->landmarks_valid)
        for (int i = 0; i < PRONE_LANDMARK_COUNT; ++i)
        {
            int x = box->landmarks[i * 2];
            box->landmarks[i * 2] =
                face_rotation_clip((right - box->landmarks[i * 2 + 1]) / 2, 319);
            box->landmarks[i * 2 + 1] =
                face_rotation_clip((x - pad_x) / 2, 239);
        }
}
