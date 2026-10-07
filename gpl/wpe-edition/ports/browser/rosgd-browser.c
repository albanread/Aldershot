/* rosgd-browser.c -- the WPE edition's browser engine: one WebKit view in a
 * Wayland window, steered from RISC OS (RISCOSGrandDesign design 23, W4).
 *
 * WPE WebKit has no browser of its own to speak of: MiniBrowser shows a
 * page and nothing else.  This is the engine of !Browser.  It opens one
 * web view, on WPE's own Wayland platform, in a Wayland window, which
 * ROSGD's compositor makes a Wimp window (WaylandWindows).  The RISC OS
 * side -- the toolbar, the address field -- is WaylandWindows' browser part
 * (rosgd/modules/waylandwin/browser.c), which steers this over a socket:
 *
 *     /run/rosgd-browser/<pid>.sock      (in the WPE root; /wpe/run/... to
 *                                         RISC OS, whose /init sees it so)
 *
 * The compositor tells WaylandWindows each window's client's process id,
 * and a socket under that pid says the window is a browser's.
 *
 * Lines from RISC OS:  go URL, back, forward, reload, stop, scrollto Y
 * Lines to RISC OS:    url URL, title TEXT, nav CANBACK CANFORWARD,
 *                      load started|finished, progress PERCENT,
 *                      download started|done|failed NAME,
 *                      scroll Y HEIGHT VIEWHEIGHT
 *
 * The page's own scroll bars are hidden (a user style sheet): the window's
 * scroll bar is RISC OS's, the Wimp's, which WaylandWindows keeps in step
 * with the page -- the page says where it is scrolled and how tall it is
 * (a user script, a message handler), and a drag of the Wimp's bar comes
 * back as scrollto.
 *
 * Downloads go to the share's Downloads directory, HostFS::Host.$.Downloads
 * (/host/Downloads here), made if need be; a name already there gets a
 * number.  Web content runs in WebKit's own sandbox (bubblewrap, in the
 * box kernel's namespaces: design 23 R10).
 *
 *     rosgd-browser [URL]       (no URL: the start page)
 */
#define _GNU_SOURCE                     /* accept4 */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wpe/webkit.h>

#define START_PAGE "https://www.riscosopen.org/"
#define SOCK_DIR   "/run/rosgd-browser"
#define DOWNLOADS  "/host/Downloads"

static WebKitWebView *view;
static GMainLoop *loop;
static char sock_path[100];             /* a socket address's room: 108 */
static int listen_fd = -1, client_fd = -1;
static guint client_src;
static char in[8192];
static size_t inlen;
/* the page's last scroll report: said again to a RISC OS that connects
 * after it (the page loads before the window is made, so its first
 * reports go to nobody) */
static char last_scroll[64];

/* ---- to RISC OS ----------------------------------------------------------- */

static void say(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void say(const char *fmt, ...)
{
    if (client_fd < 0)
        return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof line - 2)
        n = (int)sizeof line - 2;
    for (int i = 0; i < n; i++)              /* one line: no newlines inside */
        if (line[i] == '\n' || line[i] == '\r')
            line[i] = ' ';
    line[n++] = '\n';
    if (send(client_fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 && errno != EAGAIN) {
        g_source_remove(client_src);
        close(client_fd);
        client_fd = -1;
    }
}

static void say_state(void)
{
    const char *uri = webkit_web_view_get_uri(view);
    const char *title = webkit_web_view_get_title(view);
    say("url %s", uri ? uri : "");
    say("title %s", title && *title ? title : (uri ? uri : "Browser"));
    say("nav %d %d", webkit_web_view_can_go_back(view), webkit_web_view_can_go_forward(view));
    if (last_scroll[0])
        say("scroll %s", last_scroll);
}

/* ---- the view's news --------------------------------------------------------- */

static void title_changed(GObject *o, GParamSpec *p, gpointer d)
{
    (void)o, (void)p, (void)d;
    const char *title = webkit_web_view_get_title(view);
    const char *uri = webkit_web_view_get_uri(view);
    const char *t = title && *title ? title : (uri ? uri : "Browser");
    WPEToplevel *top = wpe_view_get_toplevel(webkit_web_view_get_wpe_view(view));
    if (top)
        wpe_toplevel_set_title(top, t);
    say("title %s", t);
}

static void uri_changed(GObject *o, GParamSpec *p, gpointer d)
{
    (void)o, (void)p, (void)d;
    const char *uri = webkit_web_view_get_uri(view);
    say("url %s", uri ? uri : "");
    say("nav %d %d", webkit_web_view_can_go_back(view), webkit_web_view_can_go_forward(view));
}

static void progress_changed(GObject *o, GParamSpec *p, gpointer d)
{
    (void)o, (void)p, (void)d;
    say("progress %d", (int)(webkit_web_view_get_estimated_load_progress(view) * 100 + 0.5));
}

static void load_changed(WebKitWebView *v, WebKitLoadEvent e, gpointer d)
{
    (void)v, (void)d;
    if (e == WEBKIT_LOAD_STARTED)
        say("load started");
    else if (e == WEBKIT_LOAD_FINISHED) {
        say("load finished");
        say_state();
    }
}

/* ---- the scroll bar: RISC OS's, in step with the page ------------------------------- */

static const char hide_bars[] =
    "::-webkit-scrollbar{display:none!important;width:0!important;height:0!important}"
    "html{scrollbar-width:none!important}";

/* where the top frame is scrolled, its height and the view's, once a frame
 * when any of them may have changed */
static const char report_scroll[] =
    "(function(){var t=0;"
    "function r(){t=0;var d=document.documentElement,b=document.body;"
    "var h=Math.max(d?d.scrollHeight:0,b?b.scrollHeight:0,innerHeight);"
    "window.webkit.messageHandlers.rosgd.postMessage(Math.round(scrollY)+' '+h+' '+innerHeight);}"
    "function q(){if(!t)t=requestAnimationFrame(r);}"
    "addEventListener('scroll',q,{passive:true});addEventListener('resize',q);"
    "addEventListener('load',q);addEventListener('DOMContentLoaded',function(){"
    "try{new ResizeObserver(q).observe(document.documentElement);}catch(e){}q();});q();})();";

static void scroll_message(WebKitUserContentManager *m, JSCValue *v, gpointer d)
{
    (void)m, (void)d;
    char *t = jsc_value_to_string(v);
    if (t) {
        snprintf(last_scroll, sizeof last_scroll, "%s", t);
        say("scroll %s", t);
    }
    g_free(t);
}

static void scroll_setup(void)
{
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(view);
    WebKitUserStyleSheet *css = webkit_user_style_sheet_new(hide_bars, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                             WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
    webkit_user_content_manager_add_style_sheet(ucm, css);
    webkit_user_style_sheet_unref(css);
    g_signal_connect(ucm, "script-message-received::rosgd", G_CALLBACK(scroll_message), NULL);
    webkit_user_content_manager_register_script_message_handler(ucm, "rosgd", NULL);
    WebKitUserScript *js = webkit_user_script_new(report_scroll, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                  WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, js);
    webkit_user_script_unref(js);
}

/* ---- downloads ------------------------------------------------------------------ */

static gboolean decide_destination(WebKitDownload *dl, const char *suggested, gpointer d)
{
    (void)d;
    mkdir(DOWNLOADS, 0755);
    char *base = g_path_get_basename(suggested && *suggested ? suggested : "download");
    for (char *c = base; *c; c++)               /* nothing that leaves the directory */
        if (*c == '/')
            *c = '_';
    char *path = g_build_filename(DOWNLOADS, base, NULL);
    for (int n = 1; g_file_test(path, G_FILE_TEST_EXISTS) && n < 1000; n++) {
        g_free(path);
        char *dot = strrchr(base, '.');
        char *name = dot && dot != base ? g_strdup_printf("%.*s-%d%s", (int)(dot - base), base, n, dot)
                                        : g_strdup_printf("%s-%d", base, n);
        path = g_build_filename(DOWNLOADS, name, NULL);
        g_free(name);
    }
    webkit_download_set_destination(dl, path);
    char *shown = g_path_get_basename(path);
    say("download started %s", shown);
    g_object_set_data_full(G_OBJECT(dl), "rosgd-name", shown, g_free);
    g_free(path);
    g_free(base);
    return TRUE;
}

static void download_finished(WebKitDownload *dl, gpointer d)
{
    (void)d;
    if (g_object_get_data(G_OBJECT(dl), "rosgd-failed"))
        return;
    const char *name = g_object_get_data(G_OBJECT(dl), "rosgd-name");
    say("download done %s", name ? name : "?");
}

static void download_failed(WebKitDownload *dl, GError *err, gpointer d)
{
    (void)d;
    g_object_set_data(G_OBJECT(dl), "rosgd-failed", GINT_TO_POINTER(1));
    const char *name = g_object_get_data(G_OBJECT(dl), "rosgd-name");
    say("download failed %s: %s", name ? name : "?", err ? err->message : "");
}

static void download_started(WebKitNetworkSession *s, WebKitDownload *dl, gpointer d)
{
    (void)s, (void)d;
    g_signal_connect(dl, "decide-destination", G_CALLBACK(decide_destination), NULL);
    g_signal_connect(dl, "finished", G_CALLBACK(download_finished), NULL);
    g_signal_connect(dl, "failed", G_CALLBACK(download_failed), NULL);
}

/* a response the view cannot show (a zip, a disc image) is downloaded */
static gboolean decide_policy(WebKitWebView *v, WebKitPolicyDecision *dec, WebKitPolicyDecisionType type,
                              gpointer d)
{
    (void)v, (void)d;
    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
        return FALSE;
    WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(dec);
    if (!webkit_response_policy_decision_is_mime_type_supported(r)) {
        webkit_policy_decision_download(dec);
        return TRUE;
    }
    return FALSE;
}

/* ---- from RISC OS ------------------------------------------------------------------ */

/* what is typed in the address field: a URL, a host name, or words to
 * search for */
static char *as_uri(const char *t)
{
    while (*t == ' ')
        t++;
    if (strstr(t, "://") || !strncmp(t, "about:", 6) || !strncmp(t, "file:", 5))
        return g_strdup(t);
    if (!strchr(t, ' ') && strchr(t, '.'))
        return g_strdup_printf("https://%s", t);
    char *q = g_uri_escape_string(t, NULL, FALSE);
    char *u = g_strdup_printf("https://duckduckgo.com/html/?q=%s", q);
    g_free(q);
    return u;
}

static void command(char *l)
{
    if (!strncmp(l, "go ", 3)) {
        char *u = as_uri(l + 3);
        webkit_web_view_load_uri(view, u);
        g_free(u);
    } else if (!strcmp(l, "back")) {
        webkit_web_view_go_back(view);
    } else if (!strcmp(l, "forward")) {
        webkit_web_view_go_forward(view);
    } else if (!strcmp(l, "reload")) {
        webkit_web_view_reload(view);
    } else if (!strcmp(l, "stop")) {
        webkit_web_view_stop_loading(view);
    } else if (!strcmp(l, "state")) {
        say_state();
    } else if (!strncmp(l, "scrollto ", 9)) {
        char js[64];
        snprintf(js, sizeof js, "window.scrollTo(window.scrollX,%d)", atoi(l + 9));
        webkit_web_view_evaluate_javascript(view, js, -1, NULL, NULL, NULL, NULL, NULL);
    }
}

static gboolean client_readable(gint fd, GIOCondition c, gpointer d)
{
    (void)c, (void)d;
    ssize_t n = recv(fd, in + inlen, sizeof in - 1 - inlen, MSG_DONTWAIT);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            close(fd);
            client_fd = -1;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }
    inlen += (size_t)n;
    char *start = in, *nl;
    while ((nl = memchr(start, '\n', inlen - (size_t)(start - in))) != NULL) {
        *nl = 0;
        command(start);
        start = nl + 1;
    }
    inlen -= (size_t)(start - in);
    memmove(in, start, inlen);
    if (inlen == sizeof in - 1)
        inlen = 0;
    return G_SOURCE_CONTINUE;
}

static gboolean accept_client(gint fd, GIOCondition c, gpointer d)
{
    (void)c, (void)d;
    int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0)
        return G_SOURCE_CONTINUE;
    if (client_fd >= 0) {                       /* the newest RISC OS wins */
        g_source_remove(client_src);
        close(client_fd);
    }
    client_fd = cfd;
    inlen = 0;
    client_src = g_unix_fd_add(cfd, G_IO_IN | G_IO_HUP, client_readable, NULL);
    say_state();
    return G_SOURCE_CONTINUE;
}

static int listen_control(void)
{
    mkdir(SOCK_DIR, 0755);
    snprintf(sock_path, sizeof sock_path, SOCK_DIR "/%d.sock", (int)getpid());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path);
    unlink(sock_path);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 2) != 0) {
        fprintf(stderr, "rosgd-browser: %s: %s\n", sock_path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    listen_fd = fd;
    g_unix_fd_add(fd, G_IO_IN, accept_client, NULL);
    return 0;
}

/* ---- the window ------------------------------------------------------------------------ */

static void closed(WebKitWebView *v, gpointer d)
{
    (void)v, (void)d;
    g_main_loop_quit(loop);
}

static gboolean quit_on_signal(gpointer d)
{
    (void)d;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    /* the control socket is made before the window maps, so WaylandWindows
     * finds it when the compositor reports the window */
    if (listen_control() != 0)
        return 1;

    WebKitNetworkSession *session = webkit_network_session_get_default();
    g_signal_connect(session, "download-started", G_CALLBACK(download_started), NULL);
    WebKitSettings *settings = webkit_settings_new_with_settings(
        "enable-developer-extras", FALSE, "enable-smooth-scrolling", TRUE, NULL);
    view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session,
                                        "settings", settings, NULL));
    g_object_unref(settings);
    g_signal_connect(view, "notify::title", G_CALLBACK(title_changed), NULL);
    g_signal_connect(view, "notify::uri", G_CALLBACK(uri_changed), NULL);
    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(progress_changed), NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(load_changed), NULL);
    g_signal_connect(view, "decide-policy", G_CALLBACK(decide_policy), NULL);
    g_signal_connect(view, "close", G_CALLBACK(closed), NULL);
    scroll_setup();

    WPEView *wv = webkit_web_view_get_wpe_view(view);
    WPEToplevel *top = wv ? wpe_view_get_toplevel(wv) : NULL;
    if (top)
        wpe_toplevel_set_title(top, "Browser");

    char *u = as_uri(argc > 1 ? argv[1] : START_PAGE);
    webkit_web_view_load_uri(view, u);
    g_free(u);

    loop = g_main_loop_new(NULL, FALSE);
    /* the window gone (its close icon): the program goes too */
    if (wv)
        g_signal_connect_swapped(wv, "closed", G_CALLBACK(g_main_loop_quit), loop);
    g_unix_signal_add(SIGTERM, quit_on_signal, NULL);
    g_main_loop_run(loop);

    unlink(sock_path);
    g_object_unref(view);
    return 0;
}
