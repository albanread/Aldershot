/* httpd.c PORT ROOT LOG -- a small HTTP server for NetSurf's fetch checks in
 * the box (ports/netsurf/boxcheck.py).  ROSGD's own; MIT licence.
 *
 * A Linux program, run by *RunBox: it listens on 127.0.0.1:PORT, puts
 * itself in the background -- *RunBox returns and the next command runs --
 * and serves the files under ROOT, a connection a process, each response
 * with Connection: close.  Each request's line, its Cookie, User-Agent,
 * Referer and Content-Type headers, and a POST's body (CR and LF as \r and
 * \n) go to LOG.  Some paths are its own:
 *
 *   /redirect    302 to /page2.html
 *   /setcookie   a page that sets the cookie rosgd=biscuit
 *   /cookie      a page showing the Cookie header it was sent
 *   /agent       a page showing the User-Agent
 *   /form        (POST) a page showing the body
 *   /big         3 MB of page, nearly all of it a comment, then a paragraph
 *   /missing     404
 *
 * It ends after 10 minutes with no connection.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUEST_MAX (1 << 20)
#define BIG         (3 << 20)

static int log_fd = -1;

static void log_line(const char *what, const char *text, size_t n)
{
    char line[1024];
    int len = snprintf(line, sizeof line, "%s %.*s\n", what, (int)(n < 900 ? n : 900), text);
    if (log_fd >= 0 && len > 0)
        write(log_fd, line, (size_t)len);
}

static void send_all(int c, const void *p, size_t n)
{
    const char *b = p;
    while (n) {
        ssize_t w = write(c, b, n);
        if (w <= 0)
            return;
        b += w, n -= (size_t)w;
    }
}

static void respond(int c, const char *status, const char *type, const char *extra, const void *body, size_t n)
{
    char head[1024];
    int len = snprintf(head, sizeof head,
                       "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n",
                       status, type, n, extra ? extra : "");
    send_all(c, head, (size_t)len);
    send_all(c, body, n);
}

static void page(int c, const char *title, const char *text, const char *extra)
{
    char body[8192];
    int n = snprintf(body, sizeof body, "<html><head><title>%s</title></head><body><p>%s</p></body></html>",
                     title, text);
    respond(c, "200 OK", "text/html", extra, body, (size_t)n);
}

/* A request header's value, "" if it has none */
static void header(const char *req, const char *name, char *out, size_t max)
{
    size_t nl = strlen(name);
    out[0] = 0;
    for (const char *l = strstr(req, "\r\n"); l && l[2] != '\r'; l = strstr(l + 2, "\r\n"))
        if (!strncasecmp(l + 2, name, nl) && l[2 + nl] == ':') {
            const char *v = l + 3 + nl;
            v += strspn(v, " \t");
            size_t n = strcspn(v, "\r\n");
            snprintf(out, max, "%.*s", (int)(n < max - 1 ? n : max - 1), v);
            return;
        }
}

static const char *type_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    if (!strcmp(dot, ".html"))
        return "text/html";
    if (!strcmp(dot, ".css"))
        return "text/css";
    if (!strcmp(dot, ".png"))
        return "image/png";
    if (!strcmp(dot, ".js"))
        return "application/javascript";
    return "text/plain";
}

static void serve(int c, const char *root)
{
    static char req[REQUEST_MAX + 1];
    size_t n = 0;
    char *end = NULL;
    size_t want = 0;
    while (n < REQUEST_MAX) {
        ssize_t r = read(c, req + n, REQUEST_MAX - n);
        if (r <= 0)
            break;
        n += (size_t)r;
        req[n] = 0;
        if (!end && (end = strstr(req, "\r\n\r\n"))) {
            char cl[32];
            header(req, "Content-Length", cl, sizeof cl);
            want = (size_t)(end + 4 - req) + (size_t)strtoul(cl, NULL, 10);
        }
        if (end && n >= want)
            break;
    }
    if (!end)
        return;
    char method[16] = "", path[1024] = "";
    sscanf(req, "%15s %1023s", method, path);
    log_line("REQ", req, strcspn(req, "\r\n"));
    static const char *const logged[] = { "Cookie", "User-Agent", "Referer", "Content-Type" };
    char value[2048];
    for (size_t i = 0; i < sizeof logged / sizeof logged[0]; i++) {
        header(req, logged[i], value, sizeof value);
        if (value[0]) {
            char what[32];
            snprintf(what, sizeof what, "HDR %s:", logged[i]);
            log_line(what, value, strlen(value));
        }
    }
    if (!strcmp(method, "POST")) {
        char body[900];
        size_t o = 0;
        for (const char *b = end + 4; *b && o + 3 < sizeof body; b++)
            if (*b == '\r' || *b == '\n')
                body[o++] = '\\', body[o++] = *b == '\r' ? 'r' : 'n';
            else
                body[o++] = *b;
        log_line("BODY", body, o);
    }
    char text[4096];
    if (!strcmp(path, "/redirect")) {
        respond(c, "302 Found", "text/html", "Location: /page2.html\r\n", "moved", 5);
    } else if (!strcmp(path, "/setcookie")) {
        page(c, "Cookie set", "a cookie was set", "Set-Cookie: rosgd=biscuit; Path=/\r\n");
    } else if (!strcmp(path, "/cookie")) {
        header(req, "Cookie", value, sizeof value);
        snprintf(text, sizeof text, "cookie header [%s]", value);
        page(c, "Cookie", text, NULL);
    } else if (!strcmp(path, "/agent")) {
        header(req, "User-Agent", value, sizeof value);
        snprintf(text, sizeof text, "agent [%s]", value);
        page(c, "Agent", text, NULL);
    } else if (!strcmp(path, "/form") && !strcmp(method, "POST")) {
        snprintf(text, sizeof text, "posted [%s]", end + 4);
        page(c, "Posted", text, NULL);
    } else if (!strcmp(path, "/big")) {
        static const char head[] = "<html><head><title>Big</title></head><body><!-- ";
        static const char tail[] = " --><p>big page end</p></body></html>";
        size_t len = sizeof head - 1 + BIG + sizeof tail - 1;
        char *b = malloc(len);
        if (!b)
            return;
        memcpy(b, head, sizeof head - 1);
        memset(b + sizeof head - 1, 'x', BIG);
        memcpy(b + sizeof head - 1 + BIG, tail, sizeof tail - 1);
        respond(c, "200 OK", "text/html", NULL, b, len);
        free(b);
    } else {
        char file[2048];
        struct stat st;
        snprintf(file, sizeof file, "%s%s", root, path);
        int fd = strstr(path, "..") ? -1 : open(file, O_RDONLY);
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
            respond(c, "404 Not Found", "text/html", NULL, "<html><body><p>not here</p></body></html>", 41);
            if (fd >= 0)
                close(fd);
            return;
        }
        char *b = malloc((size_t)st.st_size + 1);
        size_t got = 0;
        while (b && got < (size_t)st.st_size) {
            ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        close(fd);
        if (b)
            respond(c, "200 OK", type_of(path), NULL, b, got);
        free(b);
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "httpd PORT ROOT LOG\n");
        return 2;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0), on = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 64)) {
        fprintf(stderr, "httpd: %s\n", strerror(errno));
        return 1;
    }
    log_fd = open(argv[3], O_WRONLY | O_CREAT | O_APPEND, 0644);
    printf("httpd: listening on 127.0.0.1:%s\n", argv[1]);
    fflush(stdout);
    /* Listening already, so a connection waits for the child: the parent
     * returns to *RunBox, and the child leaves the terminal behind.  The
     * parent leads the terminal's session, so its exit hangs the terminal
     * up: the child ignores that, and the parent waits until the child has
     * a session of its own. */
    signal(SIGHUP, SIG_IGN);
    int ready[2];
    if (pipe(ready))
        return 1;
    pid_t pid = fork();
    if (pid < 0)
        return 1;
    if (pid > 0) {
        char c;
        close(ready[1]);
        read(ready[0], &c, 1);
        return 0;
    }
    close(ready[0]);
    setsid();
    write(ready[1], "", 1);
    close(ready[1]);
    signal(SIGCHLD, SIG_IGN);               /* no zombies */
    signal(SIGPIPE, SIG_IGN);
    for (int fd = 0; fd < 256; fd++)
        if (fd != s && fd != log_fd)
            close(fd);
    for (;;) {
        struct pollfd p = { .fd = s, .events = POLLIN };
        if (poll(&p, 1, 600 * 1000) <= 0)
            return 0;
        int c = accept(s, NULL, NULL);
        if (c < 0)
            continue;
        if (fork() == 0) {
            close(s);
            serve(c, argv[2]);
            shutdown(c, SHUT_WR);
            close(c);
            _exit(0);
        }
        close(c);
    }
}
