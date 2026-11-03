/* Stream one firmware image from an HTTP transport into a firmware sink (docs/wifi-ota-plan.md). */
#include <string.h>

#include "epager/upd.h"

static void sink_restart(const upd_download_params_t *p, proto_sha256_t *h, uint32_t *pos, proto_status_t *st)
{
    p->sink->abort(p->sink->ctx);
    *st = p->sink->begin(p->sink->ctx, p->size);
    proto_sha256_init(h);
    *pos = 0;
}

upd_dl_result_t upd_download(const upd_download_params_t *p, proto_status_t *sink_status)
{
    proto_status_t dummy, st;
    sink_status = sink_status != NULL ? sink_status : &dummy;
    *sink_status = PROTO_OK;
    const xfer_fw_sink_t *k = p->sink;
    if (p->size == 0 || p->buf == NULL || p->buf_len == 0 || p->size > k->slot_size) {
        *sink_status = PROTO_NO_SPACE;
        return UPD_DL_SINK;
    }
    st = k->begin(k->ctx, p->size);
    if (st != PROTO_OK) {
        *sink_status = st;
        return UPD_DL_SINK;
    }
    proto_sha256_t hash;
    proto_sha256_init(&hash);
    uint32_t pos = 0;
    unsigned resumes = 0;
    if (p->progress != NULL) {
        p->progress(p->ctx, 0, p->size);
    }
    for (;;) {
        upd_http_head_t head = {0};
        bool failed = p->http.open(p->http.ctx, pos, &head) != 0;
        if (!failed) {
            if (head.status >= 400 && head.status < 500) { /* not found, forbidden...: asking again will not help */
                p->http.close(p->http.ctx);
                k->abort(k->ctx);
                return UPD_DL_NETWORK;
            }
            if (pos > 0 && head.status == 200) { /* the server ignored the Range header: start over */
                sink_restart(p, &hash, &pos, &st);
                if (st != PROTO_OK) {
                    p->http.close(p->http.ctx);
                    *sink_status = st;
                    return UPD_DL_SINK;
                }
                failed = head.content_len != UINT32_MAX && head.content_len != p->size;
            } else if (pos > 0 && head.status == 206) {
                failed = head.range_first != pos || (head.range_total != UINT32_MAX && head.range_total != p->size);
            } else if (pos == 0 && head.status == 200) {
                if (head.content_len != UINT32_MAX && head.content_len != p->size) {
                    p->http.close(p->http.ctx);
                    k->abort(k->ctx);
                    return UPD_DL_BAD_SIZE;
                }
            } else {
                failed = true; /* 5xx, 3xx left over, 206 to a plain request */
            }
        }
        bool complete = false;
        while (!failed) {
            if (p->cancelled != NULL && p->cancelled(p->ctx)) {
                p->http.close(p->http.ctx);
                k->abort(k->ctx);
                return UPD_DL_CANCELLED;
            }
            /* one byte more than is due once the image is complete: the body must end there */
            const size_t due = (size_t)(p->size - pos) + 1u;
            const int n = p->http.read(p->http.ctx, p->buf, due < p->buf_len ? due : p->buf_len);
            if (n < 0 || (n == 0 && pos < p->size)) {
                failed = true;
            } else if (n == 0) {
                complete = true;
                break;
            } else if ((uint32_t)n > p->size - pos) {
                p->http.close(p->http.ctx);
                k->abort(k->ctx);
                return UPD_DL_BAD_SIZE;
            } else {
                st = k->write(k->ctx, pos, p->buf, (size_t)n);
                if (st != PROTO_OK) {
                    p->http.close(p->http.ctx);
                    k->abort(k->ctx);
                    *sink_status = st;
                    return UPD_DL_SINK;
                }
                proto_sha256_update(&hash, p->buf, (size_t)n);
                pos += (uint32_t)n;
                if (p->progress != NULL) {
                    p->progress(p->ctx, pos, p->size);
                }
            }
        }
        p->http.close(p->http.ctx);
        if (complete) {
            break;
        }
        if (resumes++ >= p->max_resumes) {
            k->abort(k->ctx);
            return UPD_DL_NETWORK;
        }
    }
    uint8_t sha[PROTO_SHA256_LEN];
    proto_sha256_final(&hash, sha);
    if (memcmp(sha, p->sha256, sizeof(sha)) != 0) {
        k->abort(k->ctx);
        return UPD_DL_HASH;
    }
    st = k->finish(k->ctx, p->size);
    *sink_status = st;
    if (st == PROTO_OK) {
        return UPD_DL_OK;
    }
    return st == PROTO_IMAGE_INVALID || st == PROTO_SIGNATURE_INVALID ? UPD_DL_REJECTED : UPD_DL_SINK;
}
