/* The faces linked into the firmware (components/ui_common/faces/<name>/face.json, compiled by tools/clock_face into
 * ui_face_builtin.c; written in the face source format and read by the same parser as an installed face). The
 * firmware's notification badge shows on these faces only. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *id;
    const uint8_t *data;
    size_t len;
} ui_builtin_face_t;

extern const ui_builtin_face_t ui_builtin_faces_mono[];
extern const size_t ui_builtin_faces_mono_count;
extern const ui_builtin_face_t ui_builtin_faces_colour[];
extern const size_t ui_builtin_faces_colour_count;
