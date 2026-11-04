/* Clock face helpers for the UI tests: a face file from contracts/test-vectors/clock_faces.json, installed as an asset. */
#pragma once

#include <stdio.h>
#include <string.h>

#include "epager/blob_storage.h"
#include "epager/ui.h"
#include "epager/xfer.h"
#include "test_support.h"
#include "unity.h"

#define FACE_FILE_MAX 32768u

/* The named face of the vectors; returns its length. */
static inline size_t vector_face(const char *name, uint8_t *buf, size_t cap)
{
    cJSON *doc = load_vectors("clock_faces.json");
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(doc, "faces"), name);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, name);
    const size_t n = hex_decode(jstr(f, "file_hex"), buf, cap);
    TEST_ASSERT_TRUE(n != SIZE_MAX);
    cJSON_Delete(doc);
    return n;
}

/* Store a face file the way the transfer layer does, and tell the UI. */
static inline void install_face(blob_mem_t *blob, const char *id, const uint8_t *file, size_t len)
{
    char name[BLOB_NAME_MAX + 1];
    xfer_asset_blob_name(PROTO_ASSET_TYPE_CLOCK_FACE, id, strlen(id), name);
    (void)blob->iface.remove(blob->iface.ctx, name); /* the transfer layer replaces the whole file */
    TEST_ASSERT_EQUAL_INT(BLOB_OK, blob->iface.write(blob->iface.ctx, name, 0, file, len));
    ui_assets_changed(PROTO_ASSET_TYPE_CLOCK_FACE);
}

static inline void remove_face(blob_mem_t *blob, const char *id)
{
    char name[BLOB_NAME_MAX + 1];
    xfer_asset_blob_name(PROTO_ASSET_TYPE_CLOCK_FACE, id, strlen(id), name);
    TEST_ASSERT_EQUAL_INT(BLOB_OK, blob->iface.remove(blob->iface.ctx, name));
    ui_assets_changed(PROTO_ASSET_TYPE_CLOCK_FACE);
}

/* Select a face and its option pairs in a copy of `c` and apply it. */
static inline void face_config(ui_config_t c, const char *id, const uint8_t *opts, size_t n)
{
    snprintf(c.clock_face_id, sizeof(c.clock_face_id), "%s", id);
    c.clock_face_opts_len = (uint8_t)n;
    if (n) {
        memcpy(c.clock_face_opts, opts, n);
    }
    ui_set_config(&c);
}
