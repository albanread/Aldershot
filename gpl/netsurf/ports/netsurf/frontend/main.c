/* main.c -- NetSurf's native RISC OS front end: the task (design 22,
 * section 11).  ROSGD's own; MIT licence.
 *
 *   NetSurf [-log file] [-snap file] [-quit] [-V logfile] [--option=value...] [url]
 *
 * A Wimp task: NetSurf's core and libraries are a POSIX application's,
 * over the Unix bridge; this is its Wimp half, making SWIs through the
 * gate (<swis.h>).  An icon on the icon bar, and a browser window on the
 * URL given, else the home page.  Wimp_PollIdle wakes the task when
 * NetSurf's scheduler has something due -- its fetches' polls among them;
 * http: and https: are RISC OS's own URL_Fetcher and AcornHTTP
 * (fetch_url.c), which go on while the task is paged out -- and, while the
 * pointer is over a page, every few centiseconds to follow it.
 *
 * Resources (Messages, the style sheets, the welcome page) are in
 * <NetSurf$Dir>.Resources, or the directory $NETSURFRES names.  Downloads
 * go to $NetSurf$Downloads, else HostFS:$.Downloads (download.c).
 *
 * For the checks: -log writes what the pages did to a file (titles, URLs,
 * loads, the scripts' console); -snap saves the screen (*ScreenSave) when
 * the first page has loaded, and -quit then ends the task.
 */
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <kernel.h>
#include <swis.h>
#include <nsutils/time.h>

#include "utils/errors.h"
#include "utils/file.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "netsurf/browser.h"
#include "netsurf/fetch.h"
#include "netsurf/misc.h"
#include "netsurf/netsurf.h"

#include "rosgd/rosgd.h"
#include "rosgd/fetch_url.h"

#define TASK 0x4B534154u

static FILE *log_file;
static const char *snap_file;
static bool snap_quit, snapped, quitting;
static char res[PATH_MAX];              /* the resources, a Unix path ending in / */

void ro_log(const char *fmt, ...)
{
    if (!log_file)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);
}

void ro_quit(void)
{
    quitting = true;
}

/* The task stays on the icon bar, unless a check is to end */
void ro_windows_gone(void)
{
    if (snap_quit)
        ro_quit();
}

/* ---- the scheduler ---------------------------------------------------------------- */

struct job {
    uint64_t when;                      /* monotonic milliseconds */
    void (*cb)(void *);
    void *p;
    struct job *next;
};
static struct job *jobs;                /* soonest first */

static nserror ro_schedule(int t, void (*cb)(void *), void *p)
{
    for (struct job **j = &jobs; *j;)
        if ((*j)->cb == cb && (*j)->p == p) {
            struct job *gone = *j;
            *j = gone->next;
            free(gone);
        } else {
            j = &(*j)->next;
        }
    if (t < 0)
        return NSERROR_OK;
    struct job *n = malloc(sizeof *n);
    if (!n)
        return NSERROR_NOMEM;
    uint64_t now = 0;
    nsu_getmonotonic_ms(&now);
    n->when = now + (uint64_t)t, n->cb = cb, n->p = p;
    struct job **j = &jobs;
    while (*j && (*j)->when <= n->when)
        j = &(*j)->next;
    n->next = *j;
    *j = n;
    return NSERROR_OK;
}

/* What is due, run; the milliseconds to the next, or -1 if none */
static int schedule_run(void)
{
    for (;;) {
        uint64_t now = 0;
        nsu_getmonotonic_ms(&now);
        struct job *j = jobs;
        if (!j)
            return -1;
        if (j->when > now)
            return (int)(j->when - now);
        jobs = j->next;
        void (*cb)(void *) = j->cb;
        void *p = j->p;
        free(j);
        cb(p);
    }
}

/* ---- the checks' snapshot ------------------------------------------------------------ */

static void snapshot(void *p)
{
    (void)p;
    char command[PATH_MAX + 16];
    snprintf(command, sizeof command, "ScreenSave %s", snap_file);
    _kernel_oscli(command);
    ro_log("snap %s", snap_file);
    if (snap_quit)
        ro_quit();
}

void ro_loaded(struct gui_window *gw)
{
    (void)gw;
    ro_log("loaded");
    if (snap_file && !snapped) {
        snapped = true;
        ro_schedule(1000, snapshot, NULL);      /* the redraws done first */
    }
}

/* ---- NetSurf's misc and fetch tables ---------------------------------------------------- */

static struct gui_misc_table misc_table = { .schedule = ro_schedule, .quit = ro_quit };

/* A MIME type from a file's extension, or its RISC OS type (",xxx") */
static const char *ro_filetype(const char *path)
{
    static const char *const map[][2] = {
        { "html", "text/html" }, { "htm", "text/html" }, { "faf", "text/html" },
        { "css", "text/css" }, { "f79", "text/css" },
        { "png", "image/png" }, { "b60", "image/png" },
        { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "c85", "image/jpeg" },
        { "gif", "image/gif" }, { "695", "image/gif" },
        { "bmp", "image/bmp" }, { "69c", "image/bmp" },
        { "ico", "image/x-icon" }, { "132", "image/x-icon" },
        { "svg", "image/svg+xml" }, { "js", "application/javascript" },
        { "txt", "text/plain" }, { "fff", "text/plain" },
        { "zip", "application/zip" }, { "a91", "application/zip" },
        { "pdf", "application/pdf" }, { "adf", "application/pdf" },
        { "gz", "application/gzip" }, { "tar", "application/x-tar" },
    };
    const char *leaf = strrchr(path, '/'), *dot;
    leaf = leaf ? leaf + 1 : path;
    dot = strrchr(leaf, ',');
    if (!dot)
        dot = strrchr(leaf, '.');
    if (dot)
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcasecmp(dot + 1, map[i][0]))
                return map[i][1];
    return "text/plain";
}

static struct nsurl *ro_resource_url(const char *path)
{
    char file[PATH_MAX];
    nsurl *url = NULL;
    if (!strcmp(path, "favicon.ico"))
        path = "favicon.png";
    snprintf(file, sizeof file, "%s%s", res, path);
    if (access(file, R_OK) || netsurf_path_to_nsurl(file, &url) != NSERROR_OK)
        return NULL;
    return url;
}

static struct gui_fetch_table fetch_table = { .filetype = ro_filetype, .get_resource_url = ro_resource_url };

/* A RISC OS directory as a Unix path: the task's current directory moved
 * there for a moment (the bridge takes RISC OS names) */
static bool unix_dir(const char *riscos, char *out, size_t max)
{
    char here[PATH_MAX];
    if (!getcwd(here, sizeof here))
        return false;
    bool ok = !chdir(riscos) && getcwd(out, max);
    if (!ok)
        ro_log("%s: %s", riscos, strerror(errno));
    return !chdir(here) && ok;
}

/* A RISC OS file's name as a file: URL, for NetSurf's POSIX side, which
 * reads host files: its directory through the bridge (unix_dir), its leaf
 * the host's name -- RISC OS's '/' a '.', "index/html" being index.html --
 * or the leaf with ",xxx" where HostFS keeps the type in the name */
static bool riscos_file_url(const char *name, char *out, size_t max)
{
    char canon[PATH_MAX], dir[PATH_MAX], leaf[256], path[PATH_MAX];
    if (_swix(OS_FSControl, _INR(0, 5), 37, name, canon, 0, 0, (int)sizeof canon))
        return false;
    char *dot = strrchr(canon, '.');
    if (!dot)
        return false;
    *dot = 0;
    if (!unix_dir(canon, dir, sizeof dir))
        return false;
    size_t n = 0;
    for (const char *l = dot + 1; *l && n < sizeof leaf - 5; l++)
        leaf[n++] = *l == '/' ? '.' : *l;
    leaf[n] = 0;
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    int type = 0;
    if (stat(path, &st) && !_swix(OS_File, _INR(0, 1) | _OUT(6), 23, name, &type))
        snprintf(path, sizeof path, "%s/%s,%03x", dir, leaf, type & 0xFFF);
    size_t o = (size_t)snprintf(out, max, "file://");
    for (const unsigned char *c = (const unsigned char *)path; *c && o + 4 < max; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
            strchr("-._~/,", *c))
            out[o++] = (char)*c;
        else
            o += (size_t)snprintf(out + o, max - o, "%%%02X", *c);
    }
    out[o] = 0;
    return o + 4 < max;
}

/* $NetSurf$Downloads, else HostFS:$.Downloads, made if it is not there */
bool ro_downloads_dir(char *out, size_t max)
{
    const char *d = getenv("NetSurf$Downloads");
    if (d && *d)
        return unix_dir(d, out, max);
    if (!unix_dir("HostFS:$", out, max))
        return false;
    size_t n = strlen(out);
    if (n + 11 > max)
        return false;
    strcpy(out + n, n && out[n - 1] == '/' ? "Downloads" : "/Downloads");
    return !mkdir(out, 0755) || errno == EEXIST;
}

/* $NETSURFRES, else $NetSurf$Dir.Resources as a Unix path: the task's
 * current directory moved there for a moment (the bridge takes RISC OS
 * names) */
static bool find_resources(void)
{
    const char *e = getenv("NETSURFRES");
    if (e && *e) {
        snprintf(res, sizeof res, "%s%s", e, e[strlen(e) - 1] == '/' ? "" : "/");
        return true;
    }
    const char *dir = getenv("NetSurf$Dir");
    char there[PATH_MAX];
    if (!dir)
        return false;
    snprintf(there, sizeof there, "%s.Resources", dir);
    if (!unix_dir(there, res, sizeof res - 1))
        return false;
    strcat(res, "/");
    return true;
}

/* ---- the task ------------------------------------------------------------------------ */

static void die(const char *why)
{
    _kernel_oserror e = { 1, "" };
    snprintf(e.errmess, sizeof e.errmess, "NetSurf: %s", why);
    ro_log("die %s", why);
    if (snap_quit)                              /* a check: nobody to click OK */
        exit(EXIT_FAILURE);
    _swix(Wimp_ReportError, _INR(0, 2), &e, 1, "NetSurf");
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    const char *start = NULL;
    /* our options first; NetSurf's own (-V, --name=value) go on to it */
    int n = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-log") && i + 1 < argc)
            log_file = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "-snap") && i + 1 < argc)
            snap_file = argv[++i];
        else if (!strcmp(argv[i], "-quit"))
            snap_quit = true;
        else
            argv[n++] = argv[i];
    }
    argc = n;
    argv[argc] = NULL;

    /* NetSurf's log is stderr, which is the screen: nowhere, unless -V */
    if (!(argc > 2 && !strcmp(argv[1], "-V")))
        freopen("/dev/null", "w", stderr);

    struct netsurf_table table = {
        .misc = &misc_table,
        .window = ro_window_table,
        .fetch = &fetch_table,
        .download = ro_download_table,
        .bitmap = ro_bitmap_table,
        .layout = ro_layout_table,
    };
    if (netsurf_register(&table) != NSERROR_OK)
        die("its tables were refused");
    nslog_init(NULL, &argc, argv);
    if (nsoption_init(NULL, &nsoptions, &nsoptions_default) != NSERROR_OK)
        die("options failed to initialise");
    nsoption_set_bool(enable_javascript, true);
    nsoption_commandline(&argc, argv, nsoptions);
    static char start_url[PATH_MAX * 3];
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-')
            start = argv[i];
    /* a file run (an HTML file double-clicked, Alias$@RunType_FAF): its
     * RISC OS name, not a URL */
    if (start && !strstr(start, "://") && start[0] != '/' &&
        riscos_file_url(start, start_url, sizeof start_url))
        start = start_url;

    int task = 0;
    /* the messages wanted, besides Quit: R3 = 0 is none of them (Wimp02's
     * markinitialised; -1 would be all), and with 0 Message_ModeChange never
     * came -- after a mode change the page kept the old mode's scale and
     * colours, blank or wrongly drawn as it scrolled (#87) */
    static const uint32_t wanted[] = { 5, 0x400C1, 0 };   /* DataOpen, ModeChange */
    if (_swix(Wimp_Initialise, _INR(0, 3) | _OUT(1), 380, TASK, "NetSurf", wanted, &task))
        return EXIT_FAILURE;
    ro_mode_changed();
    browser_set_dpi(180 >> ro_eigx);
    ro_log("mode eig %d,%d", ro_eigx, ro_eigy);

    if (!find_resources())
        die("no Resources directory (<NetSurf$Dir>.Resources, or $NETSURFRES)");
    ro_log("resources %s", res);
    ro_window_init(res);
    char messages[PATH_MAX];
    snprintf(messages, sizeof messages, "%sMessages", res);
    messages_add_from_file(messages);
    if (netsurf_init(NULL) != NSERROR_OK)
        die("NetSurf failed to initialise");
    if (fetch_url_register() != NSERROR_OK)
        die("the URL_Fetcher fetcher failed to register");
    if (ro_window_new(start) != NSERROR_OK)
        die("no window");
    ro_log("started");

    static uint32_t block[64];
    while (!quitting) {
        int next = schedule_run(), event = 0;
        if (quitting)
            break;
        if (ro_window_tracking() && (next < 0 || next > 40))
            next = 40;                                  /* the pointer followed */
        /* no caret gained or lost */
        uint32_t mask = 1 << 11 | 1 << 12;
        if (next < 0) {
            _swix(Wimp_Poll, _INR(0, 1) | _OUT(0), mask | 1, block, &event);
        } else {
            unsigned now = 0;
            _swix(OS_ReadMonotonicTime, _OUT(0), &now);
            _swix(Wimp_PollIdle, _INR(0, 2) | _OUT(0), mask, block, now + (unsigned)(next + 9) / 10, &event);
        }
        switch (event) {
        case 0: ro_window_track(); break;
        case 1: ro_window_redraw(block); break;
        case 2: ro_window_open(block); break;
        case 3: ro_window_close(block); break;
        case 6: ro_window_click(block); break;
        case 4: ro_window_leave(block); break;
        case 5: ro_window_enter(block); break;
        case 8: ro_window_key(block); break;
        case 9: ro_window_menu(block); break;
        case 17:
        case 18:
            if (block[4] == 0)                          /* Message_Quit */
                quitting = true;
            else if (block[4] == 5 && block[10] == 0xFAF) {     /* Message_DataOpen, HTML */
                /* a double-click on an HTML file while NetSurf runs: claimed
                 * (Message_DataLoadAck), so the Filer does not start another,
                 * and the file opened in a new window */
                char url[PATH_MAX * 3];
                if (riscos_file_url((const char *)&block[11], url, sizeof url)) {
                    block[3] = block[2];
                    block[4] = 4;
                    _swix(Wimp_SendMessage, _INR(0, 2), 17, block, block[1]);
                    ro_window_new(url);
                }
            } else if (block[4] == 0x400C1) {           /* Message_ModeChange */
                ro_mode_changed();
                browser_set_dpi(180 >> ro_eigx);
                ro_log("mode eig %d,%d", ro_eigx, ro_eigy);
                ro_window_mode_changed();
            }
            break;
        default:
            break;
        }
    }

    ro_log("quit");
    netsurf_exit();
    _swix(Wimp_CloseDown, _INR(0, 1), task, TASK);
    if (log_file)
        fclose(log_file);
    return 0;
}
