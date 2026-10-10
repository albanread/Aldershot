/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* dest.c -- where a print job goes, and the commands that say so.
 *
 * There is one destination at a time. It is a file, or a printer on the
 * network. A file is the one `*PrintTo file` named, or a dated name in
 * `Printer$Out` (`HostFS:$.PrintOut` unless it is set), made if it is not
 * there. A job opened as `printer:Name` takes that name instead of the
 * date.
 *
 * The file is written where HostFS keeps it, so its RISC OS type is its
 * host name's suffix. A job that turns out to be a PDF, which is what the
 * printer driver writes, is typed &ADF, and a double-click opens it in
 * !PDF. Anything else is left as data.
 *
 * A printer is chosen by `*PrintTo`, and the job is sent to it over IPP
 * (ipp.c). It is the same two calls as a file.
 *
 * The choice is kept. `*PrintTo` writes it to
 * `<Choices$Write>.Printers.PrintTo` (`HostFS:$.Choices` when there is no
 * Choices$Write). The file holds "printer <uri>" or "file <name>", and no
 * file means the dated default. The first job, `*Printers` or `*PrintInfo`
 * after a restart reads the choice back, asking the printer again. A
 * printer that does not answer then fails the job with its own error. The
 * job does not go quietly to a file. The printer is asked again next time.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "fileswitch.h"
#include "ipp.h"
#include "pdriver.h"

#define DEST_ERR 0xC01EEu

/* The destination.  An empty file and no printer means the dated default. */
static char dest_file[256];
static char dest_printer[256];              /* an ipp:// or ipps:// address */
static struct ipp_printer printer;          /* what it said when it was chosen */
static uint32_t printer_format;             /* IPP_FMT_: what it will be sent */
static char last_job[256];
static uint32_t last_bytes;
static uint32_t last_id;

/* A system variable's value, or NULL */
static const char *var(const char *name, char *out, size_t max)
{
    char *b = ros_rma_alloc(1100);
    if (!b)
        return NULL;
    size_t n = strlen(name) + 1;
    if (n > 500) {
        ros_rma_free(b);
        return NULL;
    }
    memcpy(b + 512, name, n);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(b + 512), s.r[1] = ros_addr(b), s.r[2] = 500, s.r[3] = 0, s.r[4] = 3;
    ros_swi(&s, XOS_ReadVarVal);
    const char *r = NULL;
    if (!s.v && (int32_t)s.r[2] > 0 && (size_t)s.r[2] < max) {
        memcpy(out, b, s.r[2]);
        out[s.r[2]] = 0;
        r = out;
    }
    ros_rma_free(b);
    return r;
}

/* Where jobs are kept: Printer$Out, else HostFS:$.PrintOut */
static const char *out_dir(char *out, size_t max)
{
    if (var("Printer$Out", out, max))
        return out;
    snprintf(out, max, "HostFS:$.PrintOut");
    return out;
}

/* A RISC OS name as Linux spells it */
static os_error *host_of(const char *riscos, char *out, size_t max)
{
    if (riscos[0] == '/') {                     /* a Linux path, as typed */
        snprintf(out, max, "%s", riscos);
        return NULL;
    }
    return ros_hostfs_linux_path(riscos, out, max);
}

/* mkdir -p, for the output directory */
static void make_dirs(const char *path)
{
    char t[1024];
    snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(t, 0777);
            *p = '/';
        }
    mkdir(t, 0777);
}

/* The kept choice ----------------------------------------------------- */

static int choice_loaded;                   /* read back since the start */
static os_error *choose(const char *rest, int quiet);

/* <Choices$Write>.Printers.PrintTo, as Linux spells it.  A box started
 * with no !Boot has no Choices$Write: then the disc's own Choices, which is
 * where !Boot would have put it. */
static int choice_file(char *host, size_t max, int make)
{
    char w[256], dir[1024];
    if (!var("Choices$Write", w, sizeof w))
        snprintf(w, sizeof w, "HostFS:$.Choices");
    char riscos[300];
    snprintf(riscos, sizeof riscos, "%s.Printers", w);
    if (host_of(riscos, dir, sizeof dir))
        return 0;
    if (make)
        make_dirs(dir);
    snprintf(host, max, "%s/PrintTo", dir);
    return 1;
}

/* Keep the destination as it now is */
static void save_choice(void)
{
    char f[1100];
    if (!choice_file(f, sizeof f, dest_printer[0] || dest_file[0]))
        return;
    if (!dest_printer[0] && !dest_file[0]) {
        unlink(f);
        return;
    }
    FILE *o = fopen(f, "w");
    if (!o)
        return;
    if (dest_printer[0])
        fprintf(o, "printer %s\n", dest_printer);
    else
        fprintf(o, "file %s\n", dest_file);
    fclose(o);
}

/* The kept destination, once after a restart; an error if its printer
 * does not answer, and then it is tried again next time */
static os_error *load_choice(void)
{
    if (choice_loaded)
        return NULL;
    char f[1100], line[300];
    if (!choice_file(f, sizeof f, 0)) {
        choice_loaded = 1;
        return NULL;
    }
    FILE *i = fopen(f, "r");
    if (!i) {
        choice_loaded = 1;
        return NULL;
    }
    line[0] = 0;
    if (!fgets(line, sizeof line, i))
        line[0] = 0;
    fclose(i);
    line[strcspn(line, "\r\n")] = 0;
    const char *what = !strncmp(line, "printer ", 8) ? line + 8
                     : !strncmp(line, "file ", 5)    ? line + 5 : "";
    if (!*what) {
        choice_loaded = 1;
        return NULL;
    }
    os_error *e = choose(what, 1);
    if (!e)
        choice_loaded = 1;
    return e;
}

/* The job's name: what `*PrintTo file` said, else the leaf the job was
 * opened with (printer:Report), else the date. */
static void job_name(const char *path, char *leaf, size_t max)
{
    if (path && *path) {
        const char *last = strrchr(path, '.');
        snprintf(leaf, max, "%s", last ? last + 1 : path);
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(leaf, max, "Print-%04d-%02d-%02d-%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* What the writer should make: PDF for a file, and for a printer whatever
 * it takes.  IPP Everywhere requires PWG Raster of every printer and only
 * recommends PDF, so a printer that takes no PDF is sent raster at its own
 * resolution. */
int pdriver_dest_format(uint32_t *dpi, int *gray)
{
    load_choice();
    if (dest_printer[0] && printer_format == IPP_FMT_PWG) {
        *dpi = printer.dpi ? printer.dpi : 300;
        *gray = !(printer.colour & IPP_COLOUR_SRGB) && (printer.colour & IPP_COLOUR_SGRAY);
        return 1;
    }
    return 0;
}

/* The chosen printer's name, for PDriver_Info: what applications show in
 * their print dialogues */
const char *pdriver_dest_name(void)
{
    load_choice();
    if (!dest_printer[0])
        return NULL;
    return printer.model[0] ? printer.model : "Printer";
}

/* A job is starting: its host file, and the name to show for it. */
os_error *pdriver_dest_open(const char *path, char *host, size_t hostmax,
                            char *shown, size_t shownmax)
{
    os_error *le = load_choice();
    if (le)
        return le;
    if (dest_printer[0]) {                  /* a printer: spool it, then send */
        /* A name nobody can guess, made here with O_EXCL: a fixed name in
         * /tmp could be a link made beforehand, and two jobs at once would
         * share it. */
        if (hostmax < sizeof "/tmp/rosgd-print-XXXXXX")
            return ros_error(DEST_ERR, "The print job could not be started");
        snprintf(host, hostmax, "/tmp/rosgd-print-XXXXXX");
        int spool = mkstemp(host);
        if (spool < 0)
            return ros_error(DEST_ERR, "The print job could not be started: %s", strerror(errno));
        close(spool);
        char leaf[128];
        job_name(path, leaf, sizeof leaf);  /* what the printer will call it */
        snprintf(shown, shownmax, "%s", leaf);
        return NULL;
    }
    char riscos[512];
    if (dest_file[0]) {
        snprintf(riscos, sizeof riscos, "%s", dest_file);
    } else {
        char dir[256], leaf[128];
        out_dir(dir, sizeof dir);
        job_name(path, leaf, sizeof leaf);
        snprintf(riscos, sizeof riscos, "%s.%s", dir, leaf);
        char dirhost[1024];
        if (!host_of(dir, dirhost, sizeof dirhost))
            make_dirs(dirhost);
    }
    snprintf(shown, shownmax, "%s", riscos);
    os_error *e = host_of(riscos, host, hostmax);
    if (e)
        return e;
    /* the type comes with the name: a PDF unless the job says otherwise */
    size_t n = strlen(host);
    if (n + 5 < hostmax)
        snprintf(host + n, hostmax - n, ",adf");
    return NULL;
}

/* A job is finished: it is a PDF if it says so, and data if it does not. */
os_error *pdriver_dest_done(const char *host, const char *shown, uint32_t bytes)
{
    if (dest_printer[0]) {
        FILE *f = fopen(host, "rb");
        if (!f)
            return ros_error(DEST_ERR, "The print job could not be read back");
        void *doc = malloc(bytes ? bytes : 1);
        size_t got = doc ? fread(doc, 1, bytes, f) : 0;
        fclose(f);
        remove(host);
        if (!doc || got != bytes) {
            free(doc);
            return ros_error(DEST_ERR, "The print job could not be read back");
        }
        uint32_t id = 0;
        os_error *e = ipp_print(dest_printer, shown, ipp_format_name(printer_format),
                                doc, got, 1, &id);
        free(doc);
        if (e)
            return e;
        snprintf(last_job, sizeof last_job, "%s on %s", shown, dest_printer);
        last_bytes = bytes, last_id = id;
        return NULL;
    }
    char head[8] = { 0 };
    FILE *f = fopen(host, "rb");
    if (f) {
        if (fread(head, 1, 5, f) != 5)
            head[0] = 0;
        fclose(f);
    }
    if (memcmp(head, "%PDF-", 5) != 0) {
        char plain[1024];
        snprintf(plain, sizeof plain, "%s", host);
        char *comma = strrchr(plain, ',');
        if (comma) {
            snprintf(comma, sizeof plain - (size_t)(comma - plain), ",ffd");
            rename(host, plain);
        }
    }
    snprintf(last_job, sizeof last_job, "%s", shown);
    last_bytes = bytes;
    return NULL;
}

/* ---- the commands ------------------------------------------------------------ */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    void *p = ros_rma_alloc((uint32_t)n + 2);
    if (!p)
        return;
    memcpy(p, line, (size_t)n);
    ((char *)p)[n] = '\n';
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(p), s.r[1] = (uint32_t)n + 1;
    ros_swi(&s, XOS_WriteN);
    ros_rma_free(p);
}

static os_error *cmd_printers(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    char dir[256];
    load_choice();
    say("Destination  Where a job goes");
    say("%s file       %s", dest_printer[0] ? " " : "*",
        dest_file[0] ? dest_file : out_dir(dir, sizeof dir));
    if (dest_printer[0])
        say("* printer    %s, %s, as %s", dest_printer,
            printer.model[0] ? printer.model : "a printer",
            ipp_format_name(printer_format));
    say("The driver is PDF %u.%02u, %u dpi.", PDRIVER_VERSION / 100, PDRIVER_VERSION % 100,
        PDRIVER_DPI);
    return NULL;
}

static os_error *cmd_printto(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    /* the tail as far as its first control character: a command line ends
     * in a CR, and "file" followed by it is still the keyword */
    char line[300];
    const char *raw = tail ? ros_ptr(tail) : "";
    size_t k = 0;
    while (raw[k] >= 32 && k < sizeof line - 1)
        line[k] = raw[k], k++;
    line[k] = 0;
    const char *t = line;
    while (*t == ' ')
        t++;
    choice_loaded = 1;                      /* what is said now wins */
    if (argc == 0 || !*t) {
        dest_file[0] = 0;
        save_choice();
        return NULL;
    }
    const char *rest = t;
    if (!strncasecmp(t, "file", 4) && (t[4] == ' ' || t[4] == 0)) {
        rest = t + 4;
        while (*rest == ' ')
            rest++;
    }
    if (!*rest) {
        dest_file[0] = dest_printer[0] = 0;
        save_choice();
        return NULL;
    }
    os_error *e = choose(rest, 0);
    if (!e)
        save_choice();
    return e;
}

/* Make `rest` the destination: an ipp:// printer, asked now, or a file.
 * Quiet when it is the kept choice being read back. */
static os_error *choose(const char *rest, int quiet)
{
    /* a printer: ask it what it is and what it takes, now, so that a job
     * does not find out at the end that it cannot be printed */
    if (!strncasecmp(rest, "ipp://", 6) || !strncasecmp(rest, "ipps://", 7)) {
        char uri[256];
        size_t k = 0;
        while (rest[k] && rest[k] > 32 && k < sizeof uri - 1)
            k++;
        memcpy(uri, rest, k);
        uri[k] = 0;
        struct ipp_printer p;
        os_error *e = ipp_ask(uri, &p);
        if (e)
            return e;
        uint32_t fmt = (p.formats & IPP_FMT_PDF)   ? IPP_FMT_PDF
                     : (p.formats & IPP_FMT_PWG)   ? IPP_FMT_PWG
                                                   : 0;
        if (!fmt)
            return ros_error(DEST_ERR, "%s takes neither PDF nor PWG Raster",
                             p.model[0] ? p.model : uri);
        printer = p, printer_format = fmt;
        snprintf(dest_printer, sizeof dest_printer, "%s", uri);
        dest_file[0] = 0;
        if (!quiet)
            say("%s, %s, printed as %s%s.", p.model[0] ? p.model : uri,
            p.state == 3 ? "idle" : p.state == 4 ? "busy" : p.state == 5 ? "stopped" : "there",
            ipp_format_name(fmt),
            fmt == IPP_FMT_PWG ? " at its own resolution" : "");
        return NULL;
    }
    dest_printer[0] = 0;
    size_t n = 0;
    while (rest[n] && rest[n] >= 32 && n < sizeof dest_file - 1)
        n++;
    memcpy(dest_file, rest, n);
    dest_file[n] = 0;
    return NULL;
}

static os_error *cmd_printinfo(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    char dir[256];
    os_error *le = load_choice();
    if (le)
        say("The kept printer did not answer: %s", le->errmess);
    if (dest_printer[0]) {
        say("The next job goes to %s.", dest_printer);
        say("It is %s, %s, and takes%s%s%s%s.",
            printer.model[0] ? printer.model : "a printer",
            printer.state == 3 ? "idle" : printer.state == 4 ? "busy"
                : printer.state == 5 ? "stopped" : "there",
            printer.formats & IPP_FMT_PDF ? " PDF" : "",
            printer.formats & IPP_FMT_PWG ? " PWG Raster" : "",
            printer.formats & IPP_FMT_URF ? " Apple Raster" : "",
            printer.formats & IPP_FMT_JPEG ? " JPEG" : "");
        say("It is sent %s%s.", ipp_format_name(printer_format),
            printer_format == IPP_FMT_PWG && printer.dpi ? ", at its own resolution" : "");
    } else if (dest_file[0])
        say("The next job goes to %s.", dest_file);
    else
        say("The next job goes to %s, under a dated name.", out_dir(dir, sizeof dir));
    say("The page is %u by %u millipoints, printed at %u dpi.",
        pdriver_paper_x(), pdriver_paper_y(), PDRIVER_DPI);
    if (last_job[0] && last_id)
        say("The last job was %s, %u bytes, printer job %u.", last_job, last_bytes, last_id);
    else if (last_job[0])
        say("The last job was %s, %u bytes.", last_job, last_bytes);
    else
        say("Nothing has been printed yet.");
    return NULL;
}

const struct ros_command pdriver_commands[] = {
    { "Printers", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *Printers",
      "*Printers lists the places a print job can go, with a star against the one in\r"
      "use. *PrintTo chooses; *PrintInfo says where the next job will go.\r",
      cmd_printers },
    { "PrintTo", ROS_CMD_INFO(0, 2, 0, 0), "Syntax: *PrintTo [file] [<file name>]",
      "*PrintTo says where the next print job goes: a file name, or a printer named\r"
      "by its address (*PrintTo ipp://printer.local/ipp/print), which is asked there\r"
      "and then what it is and what it takes. With nothing, jobs go back to a dated\r"
      "name in Printer$Out, which is HostFS:$.PrintOut unless it is set.\r",
      cmd_printto },
    { "PrintInfo", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *PrintInfo",
      "*PrintInfo says where the next print job will go, how big the page is, and\r"
      "what the last job was.\r",
      cmd_printinfo },
    { NULL }
};
