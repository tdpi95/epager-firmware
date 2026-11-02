#include "xboard.h"

#include <stdlib.h>
#include <string.h>

#include "test_support.h"

static bool read_battery(void *ctx, uint8_t *pct, uint8_t *ext)
{
    xboard_t *b = ctx;
    *pct = b->battery_pct;
    *ext = b->external_power;
    return b->battery_readable;
}

static void on_ota_event(void *ctx, const xfer_ota_event_t *ev)
{
    xboard_t *b = ctx;
    b->ota_events[ev->kind]++;
}

static void emit(void *ctx, const uint8_t *msg, size_t len)
{
    xboard_t *b = ctx;
    if (b->n_out < XBOARD_MAX_OUT && len <= sizeof(b->out[0])) {
        memcpy(b->out[b->n_out], msg, len);
        b->out_len[b->n_out++] = len;
    }
}

cJSON *json_merge(const cJSON *base, const cJSON *over)
{
    cJSON *out = cJSON_Duplicate(base, true);
    const cJSON *it;
    cJSON_ArrayForEach(it, over) {
        cJSON *cur = cJSON_GetObjectItemCaseSensitive(out, it->string);
        cJSON *val = cJSON_IsObject(it) && cJSON_IsObject(cur) ? json_merge(cur, it) : cJSON_Duplicate(it, true);
        if (cur) {
            cJSON_ReplaceItemInObjectCaseSensitive(out, it->string, val);
        } else {
            cJSON_AddItemToObject(out, it->string, val);
        }
    }
    return out;
}

void xboard_set_battery(xboard_t *b, const cJSON *bat)
{
    b->has_reader = bat && !cJSON_IsNull(bat);
    const cJSON *pct = b->has_reader ? cJSON_GetObjectItemCaseSensitive(bat, "pct") : NULL;
    b->battery_readable = cJSON_IsNumber(pct);
    b->battery_pct = b->battery_readable ? (uint8_t)pct->valuedouble : 0;
    b->external_power = b->has_reader ? (uint8_t)jnum(bat, "external_power", PROTO_EXTERNAL_POWER_UNKNOWN) : 0;
    b->ota_params.read_battery = b->has_reader ? read_battery : NULL;
    b->ota.p.read_battery = b->ota_params.read_battery;
}

xboard_t *xboard_new(const cJSON *board)
{
    xboard_t *b = calloc(1, sizeof(*b));
    kv_mem_init(&b->kv);
    blob_mem_init(&b->blob, (uint32_t)jnum(board, "storage_total", 0));
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(board, "asset_support")) {
        b->assets[b->n_assets++] = (xfer_asset_support_t){(uint8_t)jnum(a, "asset_type", 0),
                                                          (uint8_t)jnum(a, "format", 0), (uint32_t)jnum(a, "max_size", 0),
                                                          (uint16_t)jnum(a, "width", 0), (uint16_t)jnum(a, "height", 0)};
    }
    const cJSON *disp = cJSON_GetObjectItemCaseSensitive(board, "display");
    const cJSON *fw = cJSON_GetObjectItemCaseSensitive(board, "firmware");
    b->has_fw = cJSON_IsObject(fw);
    if (b->has_fw) {
        uint32_t slot = (uint32_t)jnum(fw, "slot_size", 0);
        b->fw_buf = calloc(1, slot ? slot : 1);
        fw_mem_init(&b->fw, b->fw_buf, slot);
        const char *img = jstr(fw, "image_status");
        b->fw.image_status = img ? status_by_name(img) : PROTO_OK;
        snprintf(b->running_version, sizeof(b->running_version), "%s", jstr(fw, "running_version"));
        b->ota_params = (xfer_ota_params_t){.running_version = b->running_version,
                                            .allow_downgrade = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fw, "allow_downgrade")),
                                            .low_battery_pct = (uint8_t)jnum(fw, "low_battery_pct", 30),
                                            .kv = &b->kv.iface, .on_event = on_ota_event, .ctx = b};
        xboard_set_battery(b, cJSON_GetObjectItemCaseSensitive(fw, "battery"));
    }
    b->params = (xfer_params_t){.kv = &b->kv.iface, .blob = &b->blob.iface, .assets = b->assets,
                                .n_assets = b->n_assets, .display_w = (uint16_t)jnum(disp, "width", 0),
                                .display_h = (uint16_t)jnum(disp, "height", 0), .fw = b->has_fw ? &b->fw.iface : NULL,
                                .ota = b->has_fw ? &b->ota : NULL, .max_chunk = (uint16_t)jnum(board, "max_chunk", 0),
                                .ack_window = (uint32_t)jnum(board, "ack_window", 0),
                                .journal_every = (uint32_t)jnum(board, "journal_every", 0), .emit = emit, .ctx = b};
    xboard_reboot(b);
    return b;
}

void xboard_free(xboard_t *b)
{
    blob_mem_free(&b->blob);
    free(b->fw_buf);
    free(b);
}

void xboard_reboot(xboard_t *b)
{
    b->kv.power = b->blob.power = b->fw.power = NULL;
    if (b->has_fw) {
        xfer_ota_init(&b->ota, &b->ota_params);
    }
    xfer_init(&b->x, &b->params);
}

void xboard_power_limit(xboard_t *b, int32_t budget)
{
    b->power = budget;
    b->kv.power = b->blob.power = b->fw.power = &b->power;
}

void xboard_request(xboard_t *b, const uint8_t *msg, size_t len)
{
    b->n_out = 0;
    proto_msg_t m;
    proto_status_t st = proto_msg_parse(msg, len, &m);
    uint8_t rsp[PROTO_LIMIT_MAX_MESSAGE_TRANSFER];
    proto_writer_t w;
    proto_writer_init(&w, rsp, sizeof(rsp));
    if (st != PROTO_OK) {
        if (m.header_valid && m.type == PROTO_MSG_XFER_CHUNK) {
            xfer_chunk_invalid(&b->x, &m, st);
        } else if (m.desc && m.desc->kind == PROTO_KIND_REQUEST) {
            proto_w_response(&w, &m, st);
        }
    } else if (!xfer_handle(&b->x, &m, &w)) {
        return;
    }
    if (w.len) {
        emit(b, rsp, w.len);
    }
}

xboard_t *xboard_from_vectors(const char *overrides_json)
{
    cJSON *doc = load_vectors("transfer.json");
    cJSON *over = overrides_json ? cJSON_Parse(overrides_json) : cJSON_CreateObject();
    cJSON *board = json_merge(cJSON_GetObjectItemCaseSensitive(doc, "board"), over);
    xboard_t *b = xboard_new(board);
    cJSON_Delete(board);
    cJSON_Delete(over);
    cJSON_Delete(doc);
    return b;
}
