#include <string.h>

#include "epager/clock_face.h"

uint8_t cf_option_range(const cf_option_t *o)
{
    switch (o->type) {
    case PROTO_FACE_OPTION_TYPE_TOGGLE: return 2;
    case PROTO_FACE_OPTION_TYPE_CHOICE:
    case PROTO_FACE_OPTION_TYPE_COLOUR: return o->n_choices;
    default: return 0;
    }
}

void cf_opts_resolve(const cf_face_t *f, const uint8_t *pairs, size_t n, uint8_t values[CF_MAX_OPTIONS])
{
    for (unsigned i = 0; i < f->n_options; i++) {
        const cf_option_t *o = &f->option[i];
        uint8_t v = o->def;
        for (size_t k = 0; k + 1 < n; k += 2) {
            if (pairs[k] == o->id) {
                v = pairs[k + 1] < cf_option_range(o) ? pairs[k + 1] : o->def;
            }
        }
        values[i] = v;
    }
    for (unsigned i = f->n_options; i < CF_MAX_OPTIONS; i++) {
        values[i] = 0;
    }
}

size_t cf_opts_pack(const cf_face_t *f, const uint8_t values[CF_MAX_OPTIONS], uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (unsigned i = 0; i < f->n_options && n + 2 <= cap; i++) {
        if (values[i] != f->option[i].def) {
            out[n++] = f->option[i].id;
            out[n++] = values[i];
        }
    }
    return n;
}
