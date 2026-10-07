/* download.c -- NetSurf's native RISC OS front end: downloads (design 22,
 * section 11).  ROSGD's own; MIT licence.
 *
 * A download goes straight into the Downloads directory -- $NetSurf$Downloads
 * if it is set, else Downloads on the HostFS disc (HostFS:$.Downloads, made
 * if need be) -- with no save box to drag.  The file keeps the name the
 * server or the URL gives it, made unique with -1, -2, ... when it would
 * replace one there.  The window's status line says how it went.
 *
 * Downloads start with Adjust over a link (window.c), or when NetSurf fetches
 * something it cannot show.
 */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/errors.h"
#include "netsurf/download.h"
#include "desktop/download.h"

#include "rosgd/rosgd.h"

struct gui_download_window {
    FILE *f;
    struct gui_window *g;
    char path[PATH_MAX];
    const char *leaf;               /* in path */
    unsigned long long got;
};

/* The name for the file: the given one, less anything a path or RISC OS
 * would take for something else */
static void leaf_name(const char *given, char *out, size_t max)
{
    size_t n = 0;
    for (const char *p = given && *given ? given : "download"; *p && n + 1 < max; p++)
        out[n++] = *p == '/' || *p == '\\' || *p == ':' || (unsigned char)*p < ' ' || *p == 127 ? '_' : *p;
    out[n] = 0;
    if (!n || !strcmp(out, ".") || !strcmp(out, ".."))
        snprintf(out, max, "download");
}

static struct gui_download_window *d_create(struct download_context *ctx, struct gui_window *parent)
{
    char dir[PATH_MAX], leaf[256];
    if (!ro_downloads_dir(dir, sizeof dir)) {
        ro_window_note(parent, "No Downloads directory");
        return NULL;
    }
    struct gui_download_window *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    leaf_name(download_context_get_filename(ctx), leaf, sizeof leaf);
    /* name.ext, name-1.ext, name-2.ext ... */
    const char *dot = strrchr(leaf, '.');
    int stem = dot && dot != leaf ? (int)(dot - leaf) : (int)strlen(leaf);
    for (int i = 0; i < 1000; i++) {
        if (i)
            snprintf(d->path, sizeof d->path, "%s/%.*s-%d%s", dir, stem, leaf, i, leaf + stem);
        else
            snprintf(d->path, sizeof d->path, "%s/%s", dir, leaf);
        struct stat st;
        if (stat(d->path, &st) && errno == ENOENT)
            break;
    }
    d->f = fopen(d->path, "wb");
    if (!d->f) {
        ro_window_note(parent, "The download could not be saved");
        free(d);
        return NULL;
    }
    d->g = parent;
    d->leaf = strrchr(d->path, '/') + 1;
    char note[300];
    snprintf(note, sizeof note, "Downloading %s", d->leaf);
    ro_window_note(parent, note);
    ro_log("download %s", d->path);
    return d;
}

static nserror d_data(struct gui_download_window *d, const char *data, unsigned int size)
{
    if (fwrite(data, 1, size, d->f) != size)
        return NSERROR_SAVE_FAILED;
    d->got += size;
    return NSERROR_OK;
}

static void d_error(struct gui_download_window *d, const char *why)
{
    char note[400];
    fclose(d->f);
    remove(d->path);
    snprintf(note, sizeof note, "Download of %s failed: %s", d->leaf, why);
    ro_window_note(d->g, note);
    ro_log("download failed %s: %s", d->path, why);
    free(d);
}

static void d_done(struct gui_download_window *d)
{
    char note[400];
    bool ok = fclose(d->f) == 0;
    snprintf(note, sizeof note, ok ? "Saved %s in Downloads (%llu bytes)" : "Download of %s failed", d->leaf,
             d->got);
    ro_window_note(d->g, note);
    ro_log("downloaded %s %llu", d->path, d->got);
    free(d);
}

static struct gui_download_table download_table = {
    .create = d_create, .data = d_data, .error = d_error, .done = d_done,
};
struct gui_download_table *ro_download_table = &download_table;
