/* copyto SRC DST -- a file copied, in the box's own namespaces; for
 * putting a freshly built presenter into the root's /tmp (its tmpfs is
 * /wpe/tmp to the box) without rebuilding a stick.  Static, for *RunBox. */
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int in = open(argv[1], O_RDONLY);
    if (in < 0) return 1;
    int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) return 1;
    char b[65536];
    ssize_t n, w, off;
    while ((n = read(in, b, sizeof b)) > 0) {
        off = 0;
        while (off < n && (w = write(out, b + off, n - off)) > 0) off += w;
        if (off < n) return 1;
    }
    return n < 0 ? 1 : 0;
}
