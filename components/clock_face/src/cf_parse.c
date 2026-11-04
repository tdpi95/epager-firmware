#include <string.h>

#include "epager/clock_face.h"
#include "epager/proto_msg.h"
#include "epager/proto_tlv.h"

#define FAIL(msg)                 \
    do {                          \
        if (why) {                \
            *why = (msg);         \
        }                         \
        return PROTO_IMAGE_INVALID; \
    } while (0)

static void copy_text(char *dst, size_t cap, const uint8_t *v, uint16_t n)
{
    if (n > cap - 1) {
        n = (uint16_t)(cap - 1);
    }
    memcpy(dst, v, n);
    dst[n] = '\0';
}

static bool get_text(const uint8_t *body, size_t len, uint8_t tag, char *dst, size_t cap)
{
    const uint8_t *v;
    uint16_t n;
    if (!proto_find(body, len, tag, &v, &n)) {
        dst[0] = '\0';
        return false;
    }
    copy_text(dst, cap, v, n);
    return true;
}

/* "MAJOR.MINOR.PATCH" of decimal digits only, to major << 16 | minor << 8 | patch. */
static bool semver_packed(const uint8_t *v, uint16_t n, uint32_t *out)
{
    uint32_t part[3] = {0, 0, 0};
    unsigned k = 0, digits = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (v[i] >= '0' && v[i] <= '9') {
            part[k] = part[k] * 10u + (uint32_t)(v[i] - '0');
            if (++digits > 3 || part[k] > 255u) {
                return false;
            }
        } else if (v[i] == '.' && digits && k < 2) {
            k++;
            digits = 0;
        } else {
            return false;
        }
    }
    if (k != 2 || !digits) {
        return false;
    }
    *out = part[0] << 16 | part[1] << 8 | part[2];
    return true;
}

static uint32_t image_bytes(uint8_t format, uint32_t w, uint32_t h)
{
    switch (format) {
    case PROTO_ASSET_FORMAT_MONO1: return (w + 7u) / 8u * h;
    case PROTO_ASSET_FORMAT_RGB565: return 2u * w * h;
    case PROTO_ASSET_FORMAT_RGB565A8: return 3u * w * h;
    default: return 0;
    }
}

static proto_status_t parse_image(cf_face_t *f, const uint8_t *b, size_t n, const uint8_t *data, size_t data_len,
                                  size_t *pos, const cf_limits_t *lim, const char **why)
{
    cf_image_t *im = &f->image[f->n_images];
    uint8_t set = 0, slot = 0;
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_IMAGE_FORMAT, &im->format);
    (void)proto_get_u16(b, n, PROTO_TAG_FACE_IMAGE_WIDTH, &im->width);
    (void)proto_get_u16(b, n, PROTO_TAG_FACE_IMAGE_HEIGHT, &im->height);
    const bool has_set = proto_get_u8(b, n, PROTO_TAG_FACE_IMAGE_SET, &set);
    const bool has_slot = proto_get_u8(b, n, PROTO_TAG_FACE_IMAGE_SLOT, &slot);
    if (im->format >= 32 || !(lim->formats & (1u << im->format)) || image_bytes(im->format, 1, 1) == 0) {
        FAIL("image format");
    }
    if (has_set != has_slot) {
        FAIL("image set and slot go together");
    }
    im->len = image_bytes(im->format, im->width, im->height);
    if (im->len > data_len - *pos) {
        FAIL("data area length");
    }
    im->data = data + *pos;
    *pos += im->len;
    if (has_set) {
        im->set = set;
        im->slot = slot;
        if (set < 1 || set > CF_MAX_SETS || slot >= CF_SLOTS || f->set_image[set][slot]) {
            FAIL("digit set slot");
        }
        f->set_image[set][slot] = (uint8_t)(f->n_images + 1u);
    }
    f->n_images++;
    return PROTO_OK;
}

static proto_status_t parse_option(cf_face_t *f, const uint8_t *b, size_t n, const char **why)
{
    cf_option_t *o = &f->option[f->n_options];
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_OPTION_ID, &o->id);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_OPTION_TYPE, &o->type);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_OPTION_DEFAULT, &o->def);
    (void)get_text(b, n, PROTO_TAG_FACE_OPTION_LABEL, o->label[0], sizeof(o->label[0]));
    (void)get_text(b, n, PROTO_TAG_FACE_OPTION_LABEL_VI, o->label[1], sizeof(o->label[1]));
    for (unsigned i = 0; i < f->n_options; i++) {
        if (f->option[i].id == o->id) {
            FAIL("option id repeated");
        }
    }
    size_t pos = 0;
    const uint8_t *v;
    uint16_t vl;
    while (proto_find_next(b, n, PROTO_TAG_FACE_OPTION_CHOICE, &pos, &v, &vl)) {
        if (o->n_choices == CF_MAX_CHOICES) {
            FAIL("more than 8 choices");
        }
        cf_choice_t *c = &o->choice[o->n_choices];
        const bool has_colour = proto_get_u16(v, vl, PROTO_TAG_FACE_CHOICE_COLOUR, &c->colour);
        (void)get_text(v, vl, PROTO_TAG_FACE_CHOICE_LABEL, c->label[0], sizeof(c->label[0]));
        (void)get_text(v, vl, PROTO_TAG_FACE_CHOICE_LABEL_VI, c->label[1], sizeof(c->label[1]));
        if (o->type == PROTO_FACE_OPTION_TYPE_COLOUR && !has_colour) {
            FAIL("colour choice without a colour");
        }
        o->n_choices++;
    }
    if (o->type == PROTO_FACE_OPTION_TYPE_TOGGLE) {
        if (o->n_choices || o->def > 1) {
            FAIL("toggle");
        }
    } else if (o->type == PROTO_FACE_OPTION_TYPE_CHOICE || o->type == PROTO_FACE_OPTION_TYPE_COLOUR) {
        if (o->n_choices < 1 || o->def >= o->n_choices) {
            FAIL("choices");
        }
    }
    f->n_options++;
    return PROTO_OK;
}

static proto_status_t parse_element(cf_face_t *f, const uint8_t *b, size_t n, const char **why)
{
    cf_element_t *e = &f->element[f->n_elements];
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_KIND, &e->kind);
    (void)proto_get_i16(b, n, PROTO_TAG_FACE_ELEMENT_X, &e->x);
    (void)proto_get_i16(b, n, PROTO_TAG_FACE_ELEMENT_Y, &e->y);
    (void)proto_get_u16(b, n, PROTO_TAG_FACE_ELEMENT_W, &e->w);
    (void)proto_get_u16(b, n, PROTO_TAG_FACE_ELEMENT_H, &e->h);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_ALIGN, &e->align);
    (void)proto_get_u16(b, n, PROTO_TAG_FACE_ELEMENT_COLOUR, &e->colour);
    e->has_colour_option = proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_COLOUR_OPTION, &e->colour_option);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_SOURCE, &e->source);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_FORMAT, &e->format);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_IMAGE, &e->image);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_SET, &e->set);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_FONT, &e->font);
    (void)proto_get_i16(b, n, PROTO_TAG_FACE_ELEMENT_SPACING, &e->spacing);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_ICON, &e->icon);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_THICKNESS, &e->thickness);
    (void)proto_get_u8(b, n, PROTO_TAG_FACE_ELEMENT_RADIUS, &e->radius);
    (void)proto_get_bool(b, n, PROTO_TAG_FACE_ELEMENT_FILL, &e->fill);
    (void)proto_get_u16(b, n, PROTO_TAG_FACE_ELEMENT_ARC_START, &e->arc_start);
    (void)proto_get_u16(b, n, PROTO_TAG_FACE_ELEMENT_ARC_END, &e->arc_end);
    (void)get_text(b, n, PROTO_TAG_FACE_ELEMENT_LITERAL, e->literal, sizeof(e->literal));
    size_t pos = 0;
    const uint8_t *v;
    uint16_t vl;
    while (proto_find_next(b, n, PROTO_TAG_FACE_ELEMENT_VISIBLE_IF, &pos, &v, &vl)) {
        if (e->n_conds == CF_MAX_CONDS) {
            FAIL("more than 2 conditions");
        }
        cf_cond_t *c = &e->cond[e->n_conds++];
        (void)proto_get_u8(v, vl, PROTO_TAG_FACE_COND_KIND, &c->kind);
        (void)proto_get_u8(v, vl, PROTO_TAG_FACE_COND_OPTION, &c->option);
        (void)proto_get_u8(v, vl, PROTO_TAG_FACE_COND_VALUE, &c->value);
        (void)proto_get_u8(v, vl, PROTO_TAG_FACE_COND_SOURCE, &c->source);
        (void)proto_get_bool(v, vl, PROTO_TAG_FACE_COND_NEGATE, &c->negate);
    }
    f->n_elements++;
    return PROTO_OK;
}

/* The height of the digits of set `s` when it holds all ten and they are equally tall, else 0. */
static uint16_t digit_height(const cf_face_t *f, uint8_t s)
{
    if (s < 1 || s > CF_MAX_SETS) {
        return 0;
    }
    uint16_t h = 0;
    for (unsigned d = 0; d < 10; d++) {
        const uint8_t idx = f->set_image[s][d];
        if (!idx || (h && f->image[idx - 1].height != h)) {
            return 0;
        }
        h = f->image[idx - 1].height;
    }
    return h;
}

static bool in_screen(const cf_face_t *f, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return x >= 0 && y >= 0 && x + w <= f->width && y + h <= f->height;
}

static proto_status_t check_elements(cf_face_t *f, const cf_limits_t *lim, const char **why)
{
    for (unsigned s = 1; s <= CF_MAX_SETS; s++) {
        f->set_height[s] = digit_height(f, (uint8_t)s);
    }
    for (unsigned i = 0; i < f->n_elements; i++) {
        const cf_element_t *e = &f->element[i];
        const int32_t x = e->x, y = e->y, w = e->w, h = e->h;
        switch (e->kind) {
        case PROTO_FACE_ELEMENT_RECT:
            if (w < 1 || h < 1 || !in_screen(f, x, y, w, h)) FAIL("rect");
            break;
        case PROTO_FACE_ELEMENT_LINE:
            if ((w == 0 && h == 0) || !in_screen(f, x, y, w, h)) FAIL("line");
            break;
        case PROTO_FACE_ELEMENT_ARC:
            if (!lim->arcs || w < 8 || w != h || e->source == 0 || !in_screen(f, x, y, w, h)) FAIL("arc");
            break;
        case PROTO_FACE_ELEMENT_IMAGE:
            if (e->image >= f->n_images ||
                !in_screen(f, x, y, f->image[e->image].width, f->image[e->image].height)) {
                FAIL("image element");
            }
            break;
        case PROTO_FACE_ELEMENT_NUMBER:
            if (e->set < 1 || e->set > CF_MAX_SETS || !f->set_height[e->set] || e->source == 0 ||
                !in_screen(f, x, y, w, f->set_height[e->set])) {
                FAIL("number");
            }
            break;
        case PROTO_FACE_ELEMENT_TEXT:
            if (e->font == 0 || w < 1 || h < 1 || (e->source == 0 && !e->literal[0]) || !in_screen(f, x, y, w, h)) {
                FAIL("text");
            }
            break;
        case PROTO_FACE_ELEMENT_ICON:
            if (e->icon == 0 || w < 1 || h < 1 || !in_screen(f, x, y, w, h)) FAIL("icon");
            break;
        default: break; /* unknown kinds are skipped */
        }
    }
    return PROTO_OK;
}

proto_status_t cf_parse(const uint8_t *file, size_t len, const cf_limits_t *lim, const char *expected_id,
                        uint32_t contract, cf_face_t *out, const char **why)
{
    if (why) {
        *why = "";
    }
    if (len > lim->max_file) {
        if (why) {
            *why = "file above max_size";
        }
        return PROTO_TOO_LARGE;
    }
    if (len < CF_ASSET_HEADER_LEN + 4u) FAIL("short file");
    if (memcmp(file, "ESPA", 4) != 0 || file[4] != 1 || file[5] != PROTO_ASSET_TYPE_CLOCK_FACE ||
        file[6] != PROTO_ASSET_FORMAT_CLOCK_FACE_V1 || file[7] != 0) {
        FAIL("bad container header");
    }
    if (proto_rd32(file + 12) != len - CF_ASSET_HEADER_LEN) FAIL("payload_len");
    if (proto_rd16(file + 8) != lim->width || proto_rd16(file + 10) != lim->height) FAIL("screen size");
    const uint32_t tlv_len = proto_rd32(file + CF_ASSET_HEADER_LEN);
    if (tlv_len > len - 20u) FAIL("tlv_len past the end");
    const size_t tlv_end = 20u + tlv_len, data_start = tlv_end + (4u - tlv_end % 4u) % 4u;
    if (data_start > len) FAIL("padding");
    for (size_t i = tlv_end; i < data_start; i++) {
        if (file[i]) FAIL("padding");
    }
    const uint8_t *tlv = file + 20;
    if (proto_validate_body(&proto_struct_face_root, tlv, tlv_len) != PROTO_OK) FAIL("tlv");

    memset(out, 0, sizeof(*out));
    out->width = lim->width;
    out->height = lim->height;
    const uint8_t *data = file + data_start;
    const size_t data_len = len - data_start;
    size_t data_pos = 0;
    bool have_meta = false;
    proto_tlv_reader_t r;
    proto_tlv_t t;
    proto_status_t err, st;
    proto_tlv_reader_init(&r, tlv, tlv_len);
    while (proto_tlv_next(&r, &t, &err)) {
        switch (t.tag) {
        case PROTO_TAG_FACE_ROOT_META: {
            have_meta = true;
            (void)get_text(t.val, t.len, PROTO_TAG_FACE_META_ID, out->id, sizeof(out->id));
            (void)get_text(t.val, t.len, PROTO_TAG_FACE_META_NAME, out->name[0], sizeof(out->name[0]));
            (void)get_text(t.val, t.len, PROTO_TAG_FACE_META_NAME_VI, out->name[1], sizeof(out->name[1]));
            (void)get_text(t.val, t.len, PROTO_TAG_FACE_META_AUTHOR, out->author, sizeof(out->author));
            (void)get_text(t.val, t.len, PROTO_TAG_FACE_META_VERSION, out->version, sizeof(out->version));
            (void)proto_get_u8(t.val, t.len, PROTO_TAG_FACE_META_UPDATE_ANCHOR, &out->update_anchor);
            const uint8_t *mc;
            uint16_t mcl;
            uint32_t need;
            if (proto_find(t.val, t.len, PROTO_TAG_FACE_META_MIN_CONTRACT, &mc, &mcl) &&
                (!semver_packed(mc, mcl, &need) || need > contract)) {
                FAIL("min_contract");
            }
            break;
        }
        case PROTO_TAG_FACE_ROOT_IMAGE:
            if (out->n_images == CF_MAX_IMAGES) FAIL("too many images");
            if ((st = parse_image(out, t.val, t.len, data, data_len, &data_pos, lim, why)) != PROTO_OK) return st;
            break;
        case PROTO_TAG_FACE_ROOT_OPTION:
            if (out->n_options == CF_MAX_OPTIONS) FAIL("too many options");
            if ((st = parse_option(out, t.val, t.len, why)) != PROTO_OK) return st;
            break;
        case PROTO_TAG_FACE_ROOT_ELEMENT:
            if (out->n_elements == CF_MAX_ELEMENTS) FAIL("too many elements");
            if ((st = parse_element(out, t.val, t.len, why)) != PROTO_OK) return st;
            break;
        default: break;
        }
    }
    if (err != PROTO_OK || !have_meta) FAIL("tlv");
    if (data_pos != data_len) FAIL("data area length");
    if (expected_id && strcmp(out->id, expected_id) != 0) FAIL("meta.id differs from the asset id");
    return check_elements(out, lim, why);
}

void cf_peek_names(const uint8_t *head, size_t len, char name[2][CF_NAME_MAX + 1])
{
    name[0][0] = name[1][0] = '\0';
    if (len < CF_ASSET_HEADER_LEN + 4u) {
        return;
    }
    const uint32_t tlv_len = proto_rd32(head + CF_ASSET_HEADER_LEN);
    const size_t avail = len - (CF_ASSET_HEADER_LEN + 4u);
    proto_tlv_reader_t r;
    proto_tlv_t t;
    proto_status_t err;
    proto_tlv_reader_init(&r, head + CF_ASSET_HEADER_LEN + 4u, tlv_len < avail ? tlv_len : avail);
    while (proto_tlv_next(&r, &t, &err)) {
        if (t.tag == PROTO_TAG_FACE_ROOT_META) {
            (void)get_text(t.val, t.len, PROTO_TAG_FACE_META_NAME, name[0], CF_NAME_MAX + 1);
            (void)get_text(t.val, t.len, PROTO_TAG_FACE_META_NAME_VI, name[1], CF_NAME_MAX + 1);
            return;
        }
    }
}

bool cf_draws_source(const cf_face_t *f, uint8_t source)
{
    for (unsigned i = 0; i < f->n_elements; i++) {
        const cf_element_t *e = &f->element[i];
        if (e->source == source) {
            return true;
        }
        for (unsigned c = 0; c < e->n_conds; c++) {
            if (e->cond[c].source == source) {
                return true;
            }
        }
    }
    return false;
}
