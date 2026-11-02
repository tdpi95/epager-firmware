/* Host test helpers: vector loading, hex, and logical JSON <-> TLV via the descriptors. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cJSON.h"
#include "epager/proto.h"

/* Load contracts/test-vectors/<name>. Aborts the test run if missing or invalid. */
cJSON *load_vectors(const char *name);

/* Hex helpers. hex_decode returns the byte count, or SIZE_MAX on bad input/overflow. */
size_t hex_decode(const char *hex, uint8_t *out, size_t cap);
void hex_encode(const uint8_t *data, size_t len, char *out); /* out: 2*len+1 */

/* Logical form (protocol.md 2.3) */
cJSON *body_to_json(const proto_struct_desc_t *d, const uint8_t *body, size_t len);
cJSON *msg_to_json(const proto_msg_t *m);
/* Encode {ver?, type, req_id?, fields} canonically. Returns length, 0 on error. */
size_t json_to_msg(const cJSON *logical, uint8_t *out, size_t cap);

/* Readable message tests: encode a logical JSON message (protocol.md 2.3 logical form; "ver" and
 * "req_id" optional) and render a message back as compact JSON. Both abort on invalid input. */
size_t msg_from_json(const char *json, uint8_t *out, size_t cap);
/* Parsed request (body points into buf). */
proto_msg_t parse_json_msg(const char *json, uint8_t *buf, size_t cap);
/* Compact JSON of a valid message; malloc'd, caller frees. */
char *msg_to_json_str(const uint8_t *buf, size_t len);

/* Failure accounting: report every failing case, then fail the test once. */
extern int g_case_failures;
#define CASE_FAIL(name, ...)                                    \
    do {                                                        \
        g_case_failures++;                                      \
        fprintf(stderr, "  FAIL [%s] ", (name));                \
        fprintf(stderr, __VA_ARGS__);                           \
        fputc('\n', stderr);                                    \
    } while (0)

const char *jstr(const cJSON *obj, const char *key);    /* NULL if absent */
double jnum(const cJSON *obj, const char *key, double dflt);
proto_status_t status_by_name(const char *name);        /* PROTO_STATUS_COUNT if unknown */
