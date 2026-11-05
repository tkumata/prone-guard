#include "face_rotation.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(PRONE_FACE_INPUT_WIDTH * 3 == PRONE_FACE_INPUT_HEIGHT * 4);
    assert(PRONE_FACE_PAD_X * 2 + PRONE_CAMERA_HEIGHT == PRONE_FACE_INPUT_WIDTH);
    assert(PRONE_FACE_PAD_Y * 2 + PRONE_CAMERA_WIDTH == PRONE_FACE_INPUT_HEIGHT);
    prone_face_box_t box = {
        .x0 = 188, .y0 = 1, .x1 = 667, .y1 = 640,
        .confidence = 0.9f, .valid = true, .landmarks_valid = true,
        .landmarks = {188, 1, 667, 640, 288, 201, 588, 501, 428, 321},
    };
    prone_face_box_restore_camera_orientation(&box);
    assert(box.x0 == 0 && box.y0 == 0 && box.x1 == 319 && box.y1 == 239);
    const int expected[] = {319, 0, 0, 239, 219, 50, 69, 200, 159, 120};
    assert(!memcmp(box.landmarks, expected, sizeof(expected)));
    assert(box.valid && box.landmarks_valid && box.confidence == 0.9f);

    box = (prone_face_box_t){.x0 = 228, .y0 = 81, .x1 = 348, .y1 = 281, .valid = true};
    prone_face_box_restore_camera_orientation(&box);
    assert(box.x0 == 179 && box.y0 == 20 && box.x1 == 279 && box.y1 == 80);
    assert((box.x1 - box.x0) * (box.y1 - box.y0) == 6000);
    assert(!box.landmarks_valid && box.landmarks[0] == 0 && box.landmarks[1] == 0);

    box = (prone_face_box_t){.x0 = 664, .y0 = 637, .x1 = 666, .y1 = 639, .valid = true};
    prone_face_box_restore_camera_orientation(&box);
    assert(box.x0 == 0 && box.y0 == 238 && box.x1 == 1 && box.y1 == 239);

    box = (prone_face_box_t){.x0 = 0, .y0 = 0, .x1 = 855, .y1 = 641, .valid = true};
    prone_face_box_restore_camera_orientation(&box);
    assert(box.x0 == 0 && box.y0 == 0 && box.x1 == 319 && box.y1 == 239);

    box = (prone_face_box_t){.x0 = 0, .y0 = 0, .x1 = 100, .y1 = 200,
                            .valid = true, .landmarks_valid = true};
    prone_face_box_restore_camera_orientation(&box);
    assert(!box.valid && !box.landmarks_valid);

    box = (prone_face_box_t){.x0 = -1, .y0 = -1, .x1 = -1, .y1 = -1};
    prone_face_box_restore_camera_orientation(&box);
    assert(!box.valid && box.x0 == -1 && box.y0 == -1 && box.x1 == -1 && box.y1 == -1);
    puts("Face rotation camera coordinates, area, landmarks and no-face tests passed");
}
