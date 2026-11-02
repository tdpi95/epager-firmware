/* Host test helper: a scratch directory and a capacity function for blob_fs. Header only. */
#pragma once

/* The including file defines _DEFAULT_SOURCE before its first include (mkdtemp). */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "epager/blob_fs.h"

typedef struct {
    char dir[BLOB_FS_DIR_MAX + 1];
    uint32_t total; /* free = total - the sizes of the regular files, like blob_mem */
} tmp_fs_t;

static inline int tmp_fs_make(tmp_fs_t *t, uint32_t total)
{
    memset(t, 0, sizeof(*t));
    snprintf(t->dir, sizeof(t->dir), "/tmp/epager_blob_XXXXXX");
    t->total = total;
    return mkdtemp(t->dir) != NULL ? 0 : -1;
}

static inline blob_status_t tmp_fs_usage(void *ctx, uint32_t *total, uint32_t *free_bytes)
{
    const tmp_fs_t *t = ctx;
    uint64_t used = 0;
    DIR *d = opendir(t->dir);
    if (d == NULL) {
        return BLOB_ERR;
    }
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char p[512];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", t->dir, e->d_name);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            used += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    *total = t->total;
    *free_bytes = used > t->total ? 0 : t->total - (uint32_t)used;
    return BLOB_OK;
}

/* Remove the directory with its files and one level of subdirectories. */
static inline void tmp_fs_remove_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d != NULL) {
        const struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char p[512];
            struct stat st;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
                tmp_fs_remove_dir(p);
            } else {
                unlink(p);
            }
        }
        closedir(d);
    }
    rmdir(dir);
}
