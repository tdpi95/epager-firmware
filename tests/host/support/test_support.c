#include "test_support.h"

#include <stdlib.h>
#include <string.h>

#ifndef VECTORS_DIR
#error "VECTORS_DIR must be defined by the build"
#endif

int g_case_failures;

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = '\0';
    }
    return buf;
}

cJSON *load_vectors(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", VECTORS_DIR, name);
    char *text = read_file(path);
    if (!text) {
        fprintf(stderr, "cannot read %s\n", path);
        abort();
    }
    cJSON *doc = cJSON_Parse(text);
    free(text);
    if (!doc) {
        fprintf(stderr, "invalid JSON in %s\n", path);
        abort();
    }
    return doc;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t hex_decode(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > cap) {
        return SIZE_MAX;
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return SIZE_MAX;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return n / 2;
}

void hex_encode(const uint8_t *data, size_t len, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = d[data[i] >> 4];
        out[2 * i + 1] = d[data[i] & 15];
    }
    out[2 * len] = '\0';
}

const char *jstr(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

double jnum(const cJSON *obj, const char *key, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

proto_status_t status_by_name(const char *name)
{
    for (int i = 0; i < PROTO_STATUS_COUNT; i++) {
        if (proto_status_names[i] && strcmp(proto_status_names[i], name) == 0) {
            return (proto_status_t)i;
        }
    }
    return PROTO_STATUS_COUNT;
}

/* ---------------------------------------------------------------- TLV -> JSON */

static cJSON *value_to_json(const proto_field_desc_t *f, const uint8_t *v, uint16_t n)
{
    switch (f->type) {
    case PROTO_FT_U8: return cJSON_CreateNumber(v[0]);
    case PROTO_FT_U16: return cJSON_CreateNumber(proto_rd16(v));
    case PROTO_FT_U32: return cJSON_CreateNumber(proto_rd32(v));
    case PROTO_FT_I16: return cJSON_CreateNumber((int16_t)proto_rd16(v));
    case PROTO_FT_I32: return cJSON_CreateNumber((int32_t)proto_rd32(v));
    case PROTO_FT_BOOL: return cJSON_CreateBool(v[0] != 0);
    case PROTO_FT_STRUCT: return body_to_json(f->nested, v, n);
    case PROTO_FT_BYTES: {
        char *hex = malloc(2u * n + 1);
        hex_encode(v, n, hex);
        cJSON *j = cJSON_CreateString(hex);
        free(hex);
        return j;
    }
    default: { /* str, id, text */
        char *s = malloc((size_t)n + 1);
        memcpy(s, v, n);
        s[n] = '\0';
        cJSON *j = cJSON_CreateString(s);
        free(s);
        return j;
    }
    }
}

cJSON *body_to_json(const proto_struct_desc_t *d, const uint8_t *body, size_t len)
{
    cJSON *obj = cJSON_CreateObject();
    proto_tlv_reader_t r;
    proto_tlv_t t;
    proto_status_t err;
    proto_tlv_reader_init(&r, body, len);
    while (proto_tlv_next(&r, &t, &err)) {
        const proto_field_desc_t *f = NULL;
        for (unsigned i = 0; i < d->n_fields; i++) {
            if (d->fields[i].tag == t.tag) {
                f = &d->fields[i];
            }
        }
        if (!f) {
            continue;
        }
        cJSON *val = value_to_json(f, t.val, t.len);
        if (f->flags & PROTO_F_REPEATED) {
            cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, f->name);
            if (!arr) {
                arr = cJSON_AddArrayToObject(obj, f->name);
            }
            cJSON_AddItemToArray(arr, val);
        } else {
            cJSON_AddItemToObject(obj, f->name, val);
        }
    }
    return obj;
}

cJSON *msg_to_json(const proto_msg_t *m)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "ver", m->ver);
    cJSON_AddStringToObject(obj, "type", m->desc->body.name);
    cJSON_AddNumberToObject(obj, "req_id", m->req_id);
    cJSON_AddItemToObject(obj, "fields", body_to_json(&m->desc->body, m->body, m->body_len));
    return obj;
}

/* ---------------------------------------------------------------- JSON -> TLV */

static bool json_to_fields(const proto_struct_desc_t *d, const cJSON *fields, proto_writer_t *w);

static bool value_from_json(const proto_field_desc_t *f, const cJSON *j, proto_writer_t *w)
{
    switch (f->type) {
    case PROTO_FT_U8: proto_w_u8(w, f->tag, (uint8_t)j->valuedouble); return cJSON_IsNumber(j);
    case PROTO_FT_U16: proto_w_u16(w, f->tag, (uint16_t)j->valuedouble); return cJSON_IsNumber(j);
    case PROTO_FT_U32: proto_w_u32(w, f->tag, (uint32_t)j->valuedouble); return cJSON_IsNumber(j);
    case PROTO_FT_I16: proto_w_i16(w, f->tag, (int16_t)j->valuedouble); return cJSON_IsNumber(j);
    case PROTO_FT_I32: proto_w_i32(w, f->tag, (int32_t)j->valuedouble); return cJSON_IsNumber(j);
    case PROTO_FT_BOOL: proto_w_bool(w, f->tag, cJSON_IsTrue(j)); return cJSON_IsBool(j);
    case PROTO_FT_STRUCT:
        proto_w_struct_begin(w, f->tag);
        if (!json_to_fields(f->nested, j, w)) {
            return false;
        }
        proto_w_struct_end(w);
        return true;
    case PROTO_FT_BYTES: {
        if (!cJSON_IsString(j)) {
            return false;
        }
        uint8_t buf[2048];
        size_t n = hex_decode(j->valuestring, buf, sizeof(buf));
        if (n == SIZE_MAX) {
            return false;
        }
        proto_w_bytes(w, f->tag, buf, n);
        return true;
    }
    default:
        if (!cJSON_IsString(j)) {
            return false;
        }
        proto_w_str(w, f->tag, j->valuestring);
        return true;
    }
}

static bool json_to_fields(const proto_struct_desc_t *d, const cJSON *fields, proto_writer_t *w)
{
    for (const cJSON *it = fields ? fields->child : NULL; it; it = it->next) { /* reject unknown names */
        bool known = false;
        for (unsigned i = 0; i < d->n_fields; i++) {
            known |= strcmp(d->fields[i].name, it->string) == 0;
        }
        if (!known) {
            fprintf(stderr, "unknown field %s in %s\n", it->string, d->name);
            return false;
        }
    }
    for (unsigned i = 0; i < d->n_fields; i++) { /* canonical: ascending tag order */
        const proto_field_desc_t *f = &d->fields[i];
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(fields, f->name);
        if (!j) {
            continue;
        }
        if (f->flags & PROTO_F_REPEATED) {
            if (!cJSON_IsArray(j)) {
                return false;
            }
            for (const cJSON *e = j->child; e; e = e->next) {
                if (!value_from_json(f, e, w)) {
                    return false;
                }
            }
        } else if (!value_from_json(f, j, w)) {
            return false;
        }
    }
    return true;
}

size_t json_to_msg(const cJSON *logical, uint8_t *out, size_t cap)
{
    const char *type = jstr(logical, "type");
    const proto_msg_desc_t *d = type ? proto_msg_desc_by_name(type) : NULL;
    if (!d) {
        return 0;
    }
    proto_writer_t w;
    proto_writer_init(&w, out, cap);
    proto_w_header(&w, d->id, (uint16_t)jnum(logical, "req_id", 0));
    if (!json_to_fields(&d->body, cJSON_GetObjectItemCaseSensitive(logical, "fields"), &w) || !proto_writer_ok(&w)) {
        return 0;
    }
    return w.len;
}

size_t msg_from_json(const char *json, uint8_t *out, size_t cap)
{
    cJSON *j = cJSON_Parse(json);
    size_t n = j ? json_to_msg(j, out, cap) : 0;
    cJSON_Delete(j);
    if (n == 0) {
        fprintf(stderr, "cannot encode %s\n", json);
        abort();
    }
    return n;
}

proto_msg_t parse_json_msg(const char *json, uint8_t *buf, size_t cap)
{
    proto_msg_t m;
    size_t n = msg_from_json(json, buf, cap);
    proto_status_t st = proto_msg_parse(buf, n, &m);
    if (st != PROTO_OK) {
        fprintf(stderr, "invalid message %s: %s\n", json, proto_status_name(st));
        abort();
    }
    return m;
}

char *msg_to_json_str(const uint8_t *buf, size_t len)
{
    proto_msg_t m;
    proto_status_t st = proto_msg_parse(buf, len, &m);
    if (st != PROTO_OK) {
        fprintf(stderr, "invalid message from the code under test: %s\n", proto_status_name(st));
        abort();
    }
    cJSON *j = msg_to_json(&m);
    cJSON_DeleteItemFromObject(j, "ver");
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}
