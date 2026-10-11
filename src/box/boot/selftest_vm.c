/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_vm.c checks that the memory calls behave as they do on Linux,
 * since the box's own code and its programs rely on that behaviour. The
 * test runs wherever the box runs: on Linux, and on the HAL, which has its
 * own vm.c.
 *
 * Setting pages to PROT_NONE with mprotect must keep their memory. When the
 * caller sets them back to read and write, the pages must still hold what
 * they held before, and mincore must report them resident throughout. (In
 * #200 the HAL freed the pages, and a fresh zeroed page came back in their
 * place.) */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <ifaddrs.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#endif

#include "selftest.h"
#include "rosgd/platform.h"
#ifdef __linux__
#include "../tests/hal/p9stress.h"
#endif

#define check ros_check

/* ---- system calls that the HAL answers itself ------------------------------
 * These check faults in the HAL's own answers (hal/syscall.c, thread.c,
 * process.c, unix.c, socket.c, proc.c). Each is a rule that Linux also
 * keeps, so each passes there too. */

#ifdef __linux__
/* Waits up to ms for a child. Returns its status, or -1 if it did not end
 * (it is then killed). */
static int wait_child(pid_t pid, int ms)
{
    int st = 0;
    for (int t = 0; t <= ms; t += 10) {
        if (waitpid(pid, &st, WNOHANG) == pid)
            return st;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return -1;
}
#endif

static void halsys_pipe(void)
{
    int p[2];
    char c = 'x';
    if (pipe(p)) {
        check(0, "pipe read and write with a bad buffer give EFAULT, not 0 (end of file)", "pipe failed");
        return;
    }
    write(p[1], &c, 1);
    errno = 0;
    long r = read(p[0], (void *)8, 1);
    int e1 = errno;
    char back = 0;
    long again = read(p[0], &back, 1);          /* the byte is still there */
    errno = 0;
    long w = write(p[1], (void *)8, 4);
    int e2 = errno;
    close(p[0]), close(p[1]);
    check(r == -1 && e1 == EFAULT && again == 1 && back == 'x' && w == -1 && e2 == EFAULT,
          "pipe read and write with a bad buffer give EFAULT, not 0 (end of file)",
          "read %ld errno %d, then %ld; write %ld errno %d", r, e1, again, w, e2);
}

#ifdef __linux__
static void halsys_counts(void)
{
    char buf[16];
    struct iovec v = { buf, sizeof buf };
    struct pollfd pf = { 0, 0, 0 };
    struct timespec zero = { 0, 0 };
    int dn = open("/dev/null", O_WRONLY);
    int dz = open("/dev/zero", O_RDONLY);
    errno = 0;
    long w = syscall(SYS_writev, dn, &v, 1L << 20);
    int ew = errno;
    errno = 0;
    long rd = syscall(SYS_readv, dz, &v, -1L);
    int er = errno;
    errno = 0;
    long pl = syscall(SYS_ppoll, &pf, 1UL << 40, &zero, 0, 8);
    int ep = errno;
    close(dn), close(dz);
    /* Linux refuses a ppoll count only above RLIMIT_NOFILE, so the ppoll
     * part is a rule of the HAL's own. */
    struct ros_hal_stats hs;
    int on_hal = ros_hal_stats(&hs) == 0;
    check(w == -1 && ew == EINVAL && rd == -1 && er == EINVAL && (!on_hal || (pl == -1 && ep == EINVAL)),
          "writev and readv with a garbage count, and ppoll with a garbage nfds, give EINVAL at once",
          "writev %ld/%d, readv %ld/%d, ppoll %ld/%d", w, ew, rd, er, pl, ep);
}

static volatile int sig_seen;
static void on_signal(int s) { (void)s; sig_seen = 1; }

static pthread_t sig_target;
static int sig_number;
static void *sig_sender(void *arg)
{
    (void)arg;
    usleep(50000);
    pthread_kill(sig_target, sig_number);
    return 0;
}

static int ppoll_errno;

/* ppoll for 200 ms while a thread signals this one after 50 ms.  Returns how
 * many milliseconds it waited, and the call's result in *res. */
static long ppoll_signalled(int sig, const sigset_t *mask, long *res)
{
    struct timespec t0, t1, ts = { 0, 200000000 };
    pthread_t th;
    sig_target = pthread_self(), sig_number = sig;
    pthread_create(&th, 0, sig_sender, 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    *res = syscall(SYS_ppoll, 0, 0, &ts, mask, 8);
    ppoll_errno = errno;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_join(th, 0);
    return (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
}

static void halsys_signals(void)
{
    struct sigaction sa = { .sa_handler = on_signal };
    struct sigaction old1, old2;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, &old1);
    sigset_t full, none;
    sigfillset(&full);
    sigemptyset(&none);
    long r1, r2, r3;
    long ms1 = ppoll_signalled(SIGUSR1, &full, &r1);        /* masked for the wait: no early end */
    usleep(20000);                                          /* let the handler run now the mask is back */
    long ms2 = ppoll_signalled(SIGUSR1, &none, &r2);        /* not masked: it ends the wait */
    int ppoll_errno_2 = ppoll_errno;
    usleep(20000);
    struct sigaction ign = { .sa_handler = SIG_IGN };
    sigemptyset(&ign.sa_mask);
    sigaction(SIGUSR2, &ign, &old2);
    long ms3 = ppoll_signalled(SIGUSR2, &none, &r3);        /* ignored: no early end */
    sigaction(SIGUSR1, &old1, 0);
    sigaction(SIGUSR2, &old2, 0);
    check(r1 == 0 && ms1 >= 150, "a signal masked by ppoll's sigmask does not end the wait",
          "result %ld after %ld ms", r1, ms1);
    check(r2 == -1 && ppoll_errno_2 == EINTR && ms2 < 150, "a handled, unmasked signal still ends ppoll with EINTR",
          "result %ld after %ld ms", r2, ms2);
    check(r3 == 0 && ms3 >= 150, "an ignored signal does not end the wait", "result %ld after %ld ms", r3, ms3);
}

static pid_t kill_tid;
static void *sleeper(void *arg)
{
    (void)arg;
    kill_tid = (pid_t)syscall(SYS_gettid);
    for (;;)
        usleep(10000);
    return 0;
}

static void halsys_tgkill(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t th;
        pthread_create(&th, 0, sleeper, 0);
        while (!kill_tid)
            usleep(1000);
        syscall(SYS_tgkill, getpid(), kill_tid, SIGKILL);
        sleep(2);                               /* not reached: the process is killed */
        _exit(0);
    }
    int st = pid > 0 ? wait_child(pid, 5000) : -1;
    check(pid > 0 && st != -1 && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
          "tgkill of SIGKILL at another thread kills the process", "status %d", st);
}

static void *spinner(void *arg)
{
    (void)arg;
    for (;;)
        usleep(1000);
    return 0;
}

static void halsys_exec(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t th;
        pthread_create(&th, 0, spinner, 0);
        usleep(20000);
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 1), dup2(null, 2);
        execl("/usr/bin/ssh-keygen", "ssh-keygen", "-Z", (char *)0);
        _exit(99);
    }
    int st = pid > 0 ? wait_child(pid, 5000) : -1;
    check(pid > 0 && st != -1 && WIFEXITED(st) && WEXITSTATUS(st) != 99,
          "execve from a process with two threads ends the other thread: the new program runs and exits",
          "status %d", st);
}

static int fx_a, fx_b, fx_result;
static void *fx_waiter(void *arg)
{
    (void)arg;
    struct timespec ts = { 1, 500000000 };
    fx_result = (int)syscall(SYS_futex, &fx_a, 0 | 128, 0, &ts);     /* FUTEX_WAIT_PRIVATE */
    return 0;
}

static void halsys_requeue(void)
{
    int up[2], down[2];
    if (pipe(up) || pipe(down)) {
        check(0, "FUTEX_CMP_REQUEUE does not move a waiter of another process", "pipe failed");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t th;
        char c = 'r';
        pthread_create(&th, 0, fx_waiter, 0);
        usleep(100000);
        write(up[1], &c, 1);
        read(down[0], &c, 1);
        syscall(SYS_futex, &fx_a, 1 | 128, 1);                  /* FUTEX_WAKE_PRIVATE */
        pthread_join(th, 0);
        _exit(fx_result == 0 ? 0 : 2);
    }
    char c;
    long rq = -2;
    if (pid > 0 && read(up[0], &c, 1) == 1) {
        /* FUTEX_CMP_REQUEUE_PRIVATE: wake none, move one waiter at this address to fx_b */
        rq = syscall(SYS_futex, &fx_a, 4 | 128, 0, (void *)1L, &fx_b, 0);
        write(down[1], &c, 1);
    }
    int st = pid > 0 ? wait_child(pid, 5000) : -1;
    close(up[0]), close(up[1]), close(down[0]), close(down[1]);
    check(rq == 0 && st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "FUTEX_CMP_REQUEUE does not move a waiter of another process (it can still be woken at its own address)",
          "requeue %ld, status %d", rq, st);
}

static long mem_free_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long kb = -1;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "MemFree: %ld kB", &kb) == 1)
            break;
    if (f)
        fclose(f);
    return kb;
}

static void halsys_netlink(void)
{
    /* The leak was the HAL's. On Linux MemFree also moves with the slab and
     * the page cache, so a threshold that suits the HAL's allocator does not
     * suit the kernel. */
    struct ros_hal_stats hs;
    if (ros_hal_stats(&hs) != 0)
        return;
    long before = mem_free_kb();
    int done = 0;
    for (int i = 0; i < 300; i++) {
        struct ifaddrs *ifa;
        if (getifaddrs(&ifa))
            break;
        freeifaddrs(ifa);
        done++;
    }
    long used = before - mem_free_kb();
    /* A leaked 4 KB page for each of 300 calls would be 1,200 KB. */
    check(before > 0 && done == 300 && used < 600, "getifaddrs 300 times does not leak the netlink reply pages",
          "%d calls, %ld KB used", done, used);
}

static void halsys_unix_zero(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        int sv[2];
        static char block[4096];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
            _exit(3);
        while (send(sv[0], block, sizeof block, MSG_DONTWAIT) > 0)
            ;
        _exit(send(sv[0], block, 0, 0) == 0 ? 0 : 2);   /* blocking, ring full, nothing to send */
    }
    int st = pid > 0 ? wait_child(pid, 3000) : -1;
    check(pid > 0 && st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "a zero-length send on a full AF_UNIX stream returns 0 and does not block", "status %d", st);
}

static void halsys_procnet(void)
{
    char a[8] = "", b[8] = "";
    int fa = open("/proc/net/dev", O_RDONLY), fb = open("/proc/net/route", O_RDONLY);
    long ra = fa >= 0 ? read(fa, a, 6) : -1;    /* read after both are open */
    long rb = fb >= 0 ? read(fb, b, 5) : -1;
    if (fa >= 0)
        close(fa);
    if (fb >= 0)
        close(fb);
    check(ra == 6 && rb == 5 && !memcmp(a, "Inter-", 6) && !memcmp(b, "Iface", 5),
          "two /proc/net files open at once each give their own text", "dev \"%.6s\", route \"%.5s\"", a, b);
}

static void halsys_reboot(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        long r = setuid(1000);
        errno = 0;
        /* LINUX_REBOOT_CMD_CAD_ON: harmless on Linux, which still wants root for it */
        r = r ? 5 : syscall(SYS_reboot, 0xfee1dead, 0x28121969, 0x89abcdef, 0);
        _exit(r == -1 && errno == EPERM ? 0 : 2);
    }
    int st = pid > 0 ? wait_child(pid, 3000) : -1;
    check(pid > 0 && st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "reboot by a user who is not root gives EPERM",
          "status %d", st);

    /* #228: as root, a wrong magic number and a command that does not exist
     * are refused, and nothing happens -- the HAL used to power off */
    errno = 0;
    long m = syscall(SYS_reboot, 0xdeadbeef, 0x28121969, 0x01234567, 0);
    int em = errno;
    errno = 0;
    long c = syscall(SYS_reboot, 0xfee1dead, 0x28121969, 0x13572468, 0);
    int ec = errno;
    check(m == -1 && em == EINVAL && c == -1 && ec == EINVAL,
          "reboot: a wrong magic number and an unknown command give EINVAL, and the box carries on",
          "%ld/%d %ld/%d", m, em, c, ec);
}

/* ---- the share's caches and buffers (hal/ninep.c) --------------------------
 * The HAL keeps what it learns of the share for a second, and holds small
 * writes and reads ahead.  These checks are made through plain system calls
 * on /host, so they pass on Linux too.  The box's own changes must show at
 * once; a change made on the Mac may take a second, and a hook in the HAL
 * (call &7204) stands in for the Mac by stopping the box's own changes from
 * dropping what the HAL keeps. */

#define HAL_SYS_SHARE 0x7204

static char shd[200];                       /* the test's directory on the share */

static void sh_path(char *out, const char *leaf)
{
    snprintf(out, 300, "%s/%s", shd, leaf);
}

static long sh_size(const char *leaf)
{
    struct stat st;
    char p[300];
    sh_path(p, leaf);
    return stat(p, &st) ? -1 : (long)st.st_size;
}

static int sh_listed(const char *leaf)
{
    DIR *d = opendir(shd);
    int found = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL)
        found |= !strcmp(e->d_name, leaf);
    if (d)
        closedir(d);
    return found;
}

static int sh_put(const char *leaf, const void *data, size_t n)
{
    char p[300];
    sh_path(p, leaf);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t w = write(fd, data, n);
    close(fd);
    return w == (ssize_t)n ? 0 : -1;
}

static long sh_get(const char *leaf, void *buf, size_t max)
{
    char p[300];
    sh_path(p, leaf);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = read(fd, buf, max);
    close(fd);
    return n;
}

static uint64_t sh_rng = 88172645463325252ULL;
static uint32_t sh_rand(void)
{
    sh_rng ^= sh_rng << 13, sh_rng ^= sh_rng >> 7, sh_rng ^= sh_rng << 17;
    return (uint32_t)(sh_rng >> 16);
}

static uint8_t sh_pat(uint64_t i)
{
    return (uint8_t)((i * 0x9E3779B97F4A7C15ULL) >> 56) ^ (uint8_t)(i >> 12);
}

/* Each kind of change the box makes shows at once in the next lookup,
 * listing, stat and read, through any descriptor. */
static void share_own_changes(void)
{
    char p[300], q[300], buf[512];
    struct stat st;
    sh_path(p, "a");
    int miss = stat(p, &st);                        /* a miss, kept */
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(miss == -1 && fd >= 0 && stat(p, &st) == 0 && sh_listed("a"),
          "share: a file made is found at once by stat and by a listing, after a miss that was kept",
          "miss %d fd %d", miss, fd);

    /* a write is seen by stat, by another handle's fstat and read, at once */
    write(fd, "0123456789", 10);
    int fd2 = open(p, O_RDONLY);
    struct stat s2;
    int ok = stat(p, &st) == 0 && st.st_size == 10 && fstat(fd2, &s2) == 0 && s2.st_size == 10;
    long n = read(fd2, buf, sizeof buf);
    check(ok && n == 10 && !memcmp(buf, "0123456789", 10),
          "share: ten bytes written are seen at once by stat, by another handle's fstat and by its read",
          "stat %ld, fstat %ld, read %ld", (long)st.st_size, (long)s2.st_size, n);

    /* more written through a third handle, at an offset; the reader's chunk must not hide it */
    int fd3 = open(p, O_WRONLY);
    pwrite(fd3, "abcdefghij", 10, 10);
    n = pread(fd2, buf, sizeof buf, 0);
    check(n == 20 && !memcmp(buf, "0123456789abcdefghij", 20) && fstat(fd, &st) == 0 && st.st_size == 20,
          "share: a write through a third handle is read at once through the second, and fstat sees 20 bytes",
          "read %ld, size %ld", n, (long)st.st_size);
    pwrite(fd3, "XY", 2, 5);                        /* overwritten inside what is held */
    n = pread(fd2, buf, 20, 0);
    check(n == 20 && !memcmp(buf, "01234XY789abcdefghij", 20), "share: an overwrite inside held data is read at once",
          "read %ld \"%.20s\"", n, buf);

    /* truncate by handle: longer, shorter */
    int t1 = ftruncate(fd, 100);
    n = pread(fd2, buf, sizeof buf, 0);
    int t1ok = n == 100 && buf[50] == 0 && !memcmp(buf, "01234XY789", 10) && stat(p, &st) == 0 && st.st_size == 100;
    int t2 = ftruncate(fd, 5);
    n = pread(fd2, buf, sizeof buf, 0);
    check(t1 == 0 && t1ok && t2 == 0 && n == 5 && stat(p, &st) == 0 && st.st_size == 5,
          "share: a truncate to 100 and then to 5 bytes is seen at once by stat and by a read through another handle",
          "reads %ld, size %ld", n, (long)st.st_size);

    /* appended through O_APPEND by two handles: no write is lost or reordered */
    close(fd), close(fd2), close(fd3);
    sh_put("ap", "", 0);
    sh_path(p, "ap");
    int a1 = open(p, O_WRONLY | O_APPEND), a2 = open(p, O_WRONLY | O_APPEND);
    for (int i = 0; i < 10; i++)
        write(i & 1 ? a2 : a1, i & 1 ? "BB" : "A", i & 1 ? 2 : 1);
    close(a1), close(a2);
    n = sh_get("ap", buf, sizeof buf);
    check(n == 15 && !memcmp(buf, "ABBABBABBABBABB", 15), "share: O_APPEND writes through two handles all arrive, in order",
          "%ld bytes \"%.15s\"", n, buf);

    /* rename */
    sh_path(p, "a");
    sh_path(q, "b");
    int r_miss = stat(q, &st);
    int r = rename(p, q);
    check(r_miss == -1 && r == 0 && stat(p, &st) == -1 && errno == ENOENT && stat(q, &st) == 0 && sh_listed("b") &&
              !sh_listed("a"),
          "share: a rename shows at once: the new name is found (after a miss), the old is gone, the listing is right",
          "rename %d", r);

    /* unlink */
    int u = unlink(q);
    check(u == 0 && stat(q, &st) == -1 && errno == ENOENT && !sh_listed("b"),
          "share: a file deleted is gone at once from stat and from a listing", "unlink %d", u);

    /* data held for a file, and then the file is opened with O_TRUNC, renamed, linked, or deleted */
    sh_path(p, "t");
    int ta = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(ta, "0123456789", 10);                    /* held */
    int tb = open(p, O_RDWR | O_TRUNC);
    struct stat tst;
    int trunc_ok = fstat(tb, &tst) == 0 && tst.st_size == 0;
    pwrite(ta, "ab", 2, 0);
    n = pread(tb, buf, sizeof buf, 0);
    check(ta >= 0 && tb >= 0 && trunc_ok && n == 2 && !memcmp(buf, "ab", 2),
          "share: held data is sent before another handle truncates the file, and what is written after is read at once",
          "truncated size ok %d, read %ld", trunc_ok, n);
    close(tb);
    lseek(ta, 2, SEEK_SET);
    write(ta, "cdefg", 5);                          /* held again, at position 2 */
    sh_path(q, "t2");
    int rn = rename(p, q);
    close(ta);
    n = sh_get("t2", buf, sizeof buf);
    check(rn == 0 && n == 7 && !memcmp(buf, "abcdefg", 7),
          "share: data held for a file that is renamed is all in the file under its new name", "rename %d, %ld bytes", rn, n);
    int tc = open(q, O_WRONLY);
    write(tc, "XYZ", 3);                            /* held */
    char q3[300];
    sh_path(q3, "t3");
    int tl = link(q, q3);
    long lsz = sh_size("t3");                       /* through the other name: the size includes what is held */
    int ul = unlink(q);
    int cl = close(tc);
    n = sh_get("t3", buf, sizeof buf);
    check(tl == 0 && lsz == 7 && ul == 0 && cl == 0 && n == 7 && !memcmp(buf, "XYZdefg", 7),
          "share: data held for a file is seen through another name of it, and a close after the first name is deleted works",
          "link %d, size %ld, unlink %d, close %d, %ld bytes", tl, lsz, ul, cl, n);
    unlink(q3);
    sh_put("sl_t", "12345", 5);
    sh_path(p, "sl_t");
    sh_path(q, "sl");
    symlink("sl_t", q);
    int sa = open(q, O_WRONLY);                     /* through the link */
    pwrite(sa, "ABCDEFGH", 8, 0);                   /* held */
    long tsz = sh_size("sl_t");
    close(sa);
    check(tsz == 8, "share: data written through a link is counted in the target's size at once", "size %ld", tsz);
    unlink(q);
    unlink(p);

    /* mode and times */
    sh_put("m", "x", 1);
    sh_path(p, "m");
    stat(p, &st);
    chmod(p, 0640);
    int c1 = stat(p, &st) == 0 && (st.st_mode & 0777) == 0640;
    struct timespec ts[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
    utimensat(AT_FDCWD, p, ts, 0);
    int c2 = stat(p, &st) == 0 && st.st_mtime == 1000000000;
    check(c1 && c2, "share: a chmod and a utimensat show at once in stat", "mode %o, mtime %ld", st.st_mode & 0777,
          (long)st.st_mtime);

    /* directories */
    sh_path(p, "d");
    int d_miss = stat(p, &st);
    int mk = mkdir(p, 0755);
    int d_ok = stat(p, &st) == 0 && S_ISDIR(st.st_mode) && sh_listed("d");
    sh_path(q, "d/in");
    int in_ok = sh_put("d/in", "z", 1) == 0 && sh_size("d/in") == 1;
    unlink(q);
    int rm = rmdir(p);
    check(d_miss == -1 && mk == 0 && d_ok && in_ok && rm == 0 && stat(p, &st) == -1 && !sh_listed("d"),
          "share: mkdir, a file in the new directory, and rmdir show at once", "mkdir %d rmdir %d", mk, rm);
    sh_path(p, "e/deep");
    int deep_miss = stat(p, &st);                   /* kept: neither e nor e/deep is there */
    sh_path(q, "e");
    mkdir(q, 0755);
    sh_put("e/deep", "q", 1);
    check(deep_miss == -1 && stat(p, &st) == 0, "share: a name below a directory that was missing is found once both are made",
          "stat of e/deep after: %d", errno);
    unlink(p);
    rmdir(q);

    /* symlinks: a link's target that is not there, then made; then deleted */
    sh_path(p, "lk");
    sh_path(q, "tg");
    int sl = symlink("tg", p);
    char tbuf[64] = { 0 };
    ssize_t rl = readlink(p, tbuf, sizeof tbuf - 1);
    int dangling = stat(p, &st) == -1 && lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
    sh_put("tg", "target", 6);
    int through = stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 6;
    unlink(q);
    int gone = stat(p, &st) == -1 && lstat(p, &st) == 0;
    check(sl == 0 && rl == 2 && !strcmp(tbuf, "tg") && dangling && through && gone,
          "share: a link to a missing target, then to a file made, then to one deleted, is seen at once each time",
          "symlink %d readlink %zd dangling %d through %d gone %d", sl, rl, dangling, through, gone);
    unlink(p);

    /* a hard link: both names show the count at once */
    sh_put("h1", "hh", 2);
    sh_path(p, "h1");
    sh_path(q, "h2");
    stat(p, &st);
    int hl = link(p, q);
    struct stat s1b, s2b;
    int both = stat(p, &s1b) == 0 && stat(q, &s2b) == 0 && s1b.st_nlink == 2 && s2b.st_nlink == 2 &&
               s1b.st_ino == s2b.st_ino;
    unlink(q);
    check(hl == 0 && both && stat(p, &st) == 0 && st.st_nlink == 1,
          "share: link shows a count of 2 at once on both names, and unlink takes it back to 1", "link %d nlink %ld", hl,
          (long)st.st_nlink);
    unlink(p);

    /* an extended attribute (HostFS keeps RISC OS attributes in one) */
    sh_put("x", "x", 1);
    sh_path(p, "x");
    char xv[8];
    ssize_t g0 = getxattr(p, "user.riscos.attr", xv, sizeof xv);
    int g0e = errno;
    char one = 0x13, two = 0x33;
    int sx1 = setxattr(p, "user.riscos.attr", &one, 1, 0);
    ssize_t g1 = getxattr(p, "user.riscos.attr", xv, sizeof xv);
    int v1 = g1 == 1 && xv[0] == 0x13;
    int sx2 = setxattr(p, "user.riscos.attr", &two, 1, 0);
    ssize_t g2 = getxattr(p, "user.riscos.attr", xv, sizeof xv);
    check(g0 == -1 && g0e == ENODATA && sx1 == 0 && v1 && sx2 == 0 && g2 == 1 && xv[0] == 0x33,
          "share: an extended attribute set shows at once, after a miss that was kept, and again when it is changed",
          "get %zd/%d set %d get %zd set %d get %zd", g0, g0e, sx1, g1, sx2, g2);
    unlink(p);
}

/* What is written is sent to the Mac in time, and is not lost when the
 * descriptor is closed, synced or the process ends. */
static void share_write_behind(int on_hal)
{
    char p[300], buf[64];
    sh_path(p, "wb");
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, "held", 4);
    long held = on_hal ? syscall(HAL_SYS_SHARE, 3) : 4;
    check(fd >= 0 && (held >= 4 || !on_hal), "share: four bytes written are held by the HAL", "held %ld", held);
    if (on_hal) {
        usleep(1300000);                            /* a second and a bit: the HAL sends what it holds */
        long after = syscall(HAL_SYS_SHARE, 3);
        check(after == 0, "share: written data is sent within about a second without a close or a sync",
              "%ld bytes still held after 1.3 s", after);
    }
    write(fd, "more", 4);
    close(fd);
    long n = sh_get("wb", buf, sizeof buf);
    check(n == 8 && !memcmp(buf, "heldmore", 8), "share: data written in two goes, with the file closed, is all there",
          "%ld bytes", n);

    /* fsync sends it: nothing is held afterwards */
    fd = open(p, O_WRONLY | O_TRUNC);
    write(fd, "sync", 4);
    int fs = fsync(fd);
    long left = on_hal ? syscall(HAL_SYS_SHARE, 3) : 0;
    check(fs == 0 && left == 0, "share: fsync sends what is held", "fsync %d, %ld held", fs, left);
    close(fd);

    /* a child writes through an inherited descriptor and ends without closing it */
    fd = open(p, O_WRONLY | O_TRUNC);
    write(fd, "par", 3);
    pid_t pid = fork();
    if (pid == 0) {
        write(fd, "ent", 3);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    write(fd, "!", 1);
    close(fd);
    n = sh_get("wb", buf, sizeof buf);
    check(pid > 0 && n == 7 && !memcmp(buf, "parent!", 7),
          "share: a child's write through an inherited descriptor, and the parent's before and after, are in order",
          "%ld bytes \"%.7s\"", n, buf);

    /* a child ends without closing: what it wrote is on the Mac */
    pid = fork();
    if (pid == 0) {
        int cfd = open(p, O_WRONLY | O_TRUNC);
        write(cfd, "child", 5);
        _exit(0);
    }
    waitpid(pid, &st, 0);
    long after_child = on_hal ? syscall(HAL_SYS_SHARE, 3) : 0;
    n = sh_get("wb", buf, sizeof buf);
    check(pid > 0 && n == 5 && !memcmp(buf, "child", 5) && after_child == 0,
          "share: a process that ends without closing its file has its data written", "%ld bytes, %ld held", n, after_child);
    unlink(p);
}

/* A great deal of data, in awkward pieces, comes back whole. */
static void share_integrity(void)
{
    char p[300];
    sh_path(p, "big");
    static const unsigned piece[] = { 1, 7, 1, 7, 4096, 7, 1, 65535, 4096, 65537, 7 };
    const uint64_t total = 50ULL << 20;
    uint8_t *buf = malloc(70000);
    if (!buf) {
        check(0, "share: 50 MB written in odd pieces comes back whole", "no memory");
        return;
    }
    int rd = open(p, O_RDONLY | O_CREAT, 0644);     /* the reader is open from the start */
    int wr = open(p, O_WRONLY | O_TRUNC);
    uint64_t at = 0;
    unsigned k = 0, writes = 0;
    int werr = 0;
    while (at < total) {
        unsigned n = piece[k++ % (sizeof piece / sizeof piece[0])];
        if (at + n > total)
            n = (unsigned)(total - at);
        for (unsigned i = 0; i < n; i++)
            buf[i] = sh_pat(at + i);
        if (write(wr, buf, n) != (ssize_t)n) {
            werr = 1;
            break;
        }
        at += n, writes++;
        if (!(writes % 997)) {                      /* now and then the reader looks: it must see all so far */
            uint8_t probe[16];
            ssize_t g = pread(rd, probe, sizeof probe, (off_t)(at > 16 ? at - 16 : 0));
            unsigned want = at > 16 ? 16 : (unsigned)at;
            for (unsigned i = 0; i < want && g == (ssize_t)want; i++)
                if (probe[i] != sh_pat(at - want + i))
                    g = -2;
            if (g != (ssize_t)want) {
                werr = 2;
                break;
            }
        }
    }
    struct stat st;
    int sz = fstat(rd, &st) == 0 && (uint64_t)st.st_size == at;
    close(wr);
    /* read it all back through the reader, in pieces of other sizes */
    static const unsigned rpiece[] = { 4096, 1, 65536, 7, 100000, 65535, 3 };
    uint8_t *rb = malloc(100000);
    uint64_t pos = 0;
    unsigned bad = 0;
    k = 0;
    while (rb && pos < at && !bad) {
        unsigned n = rpiece[k++ % (sizeof rpiece / sizeof rpiece[0])];
        ssize_t g = read(rd, rb, n);
        if (g <= 0) {
            bad = 1;
            break;
        }
        for (ssize_t i = 0; i < g; i++)
            if (rb[i] != sh_pat(pos + (uint64_t)i)) {
                bad = 2;
                break;
            }
        pos += (uint64_t)g;
    }
    close(rd);
    /* and once more, from a new handle, in one large read at the end */
    int again = open(p, O_RDONLY);
    int tail = again >= 0 && pread(again, rb, 100, (off_t)(at - 100)) == 100;
    for (unsigned i = 0; tail && i < 100; i++)
        tail = rb[i] == sh_pat(at - 100 + i);
    if (again >= 0)
        close(again);
    check(!werr && sz && !bad && pos == at && tail,
          "share: 50 MB written in pieces of 1, 7, 4096, 65535 and 65537 bytes comes back whole through a second handle",
          "write error %d, size ok %d, read fault %u at %llu of %llu, tail %d", werr, sz, bad, (unsigned long long)pos,
          (unsigned long long)at, tail);
    unlink(p);
    free(buf);
    free(rb);
}

/* Reads and writes at random places of one file through two handles agree
 * with a model in memory. */
static void share_model(void)
{
    char p[300];
    sh_path(p, "model");
    enum { MAXLEN = 1 << 20 };
    uint8_t *model = calloc(MAXLEN + 70000, 1), *tmp = malloc(70000), *got = malloc(70000);
    if (!model || !tmp || !got) {
        check(0, "share: random reads and writes through two handles agree with a model", "no memory");
        return;
    }
    int f1 = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644), f2 = open(p, O_RDWR);
    long size = 0;
    unsigned bad = 0, ops = 30000, i;
    for (i = 0; i < ops && !bad; i++) {
        int fd = sh_rand() & 1 ? f1 : f2;
        unsigned op = sh_rand() % 100;
        uint32_t len = sh_rand() % 4 ? sh_rand() % 300 + 1 : sh_rand() % 70000 + 1;
        uint32_t off = sh_rand() % 3 ? (uint32_t)sh_rand() % (MAXLEN / 4) : (uint32_t)sh_rand() % MAXLEN;
        if (op < 45) {                              /* pwrite */
            for (uint32_t k = 0; k < len; k++)
                tmp[k] = (uint8_t)sh_rand();
            if (pwrite(fd, tmp, len, off) != (ssize_t)len)
                bad = 1;
            if ((long)(off + len) > size)
                size = off + len;
            memcpy(model + off, tmp, len);
        } else if (op < 55) {                       /* lseek and write (the file position) */
            for (uint32_t k = 0; k < len; k++)
                tmp[k] = (uint8_t)sh_rand();
            lseek(fd, off, SEEK_SET);
            if (write(fd, tmp, len) != (ssize_t)len)
                bad = 2;
            if ((long)(off + len) > size)
                size = off + len;
            memcpy(model + off, tmp, len);
        } else if (op < 65) {                       /* sequential reads from the position */
            lseek(fd, off, SEEK_SET);
            long want = off >= (uint32_t)size ? 0 : (long)(size - off < len ? size - off : len);
            long g = read(fd, got, len);
            if (g != want || (g > 0 && memcmp(got, model + off, (size_t)g)))
                bad = 3;
        } else if (op < 90) {                       /* pread */
            long want = off >= (uint32_t)size ? 0 : (long)(size - off < len ? size - off : len);
            long g = pread(fd, got, len, off);
            if (g != want || (g > 0 && memcmp(got, model + off, (size_t)g)))
                bad = 4;
        } else if (op < 95) {                       /* fstat through either handle */
            struct stat st;
            if (fstat(fd, &st) || st.st_size != size)
                bad = 5;
        } else {                                    /* truncate, up or down */
            uint32_t ns = sh_rand() % (MAXLEN / 2);
            if (ftruncate(fd, ns))
                bad = 6;
            if ((long)ns > size)
                memset(model + size, 0, ns - (uint32_t)size);
            else
                memset(model + ns, 0, (size_t)(size - ns));
            size = ns;
        }
    }
    struct stat st;
    int sz = stat(p, &st) == 0 && st.st_size == size;
    close(f1), close(f2);
    /* the whole file, from a new handle, against the model */
    int f3 = open(p, O_RDONLY);
    uint8_t *all = malloc((size_t)size + 1);
    long g = f3 >= 0 && all ? pread(f3, all, (size_t)size, 0) : -1;
    int same = g == size && !memcmp(all, model, (size_t)size);
    if (f3 >= 0)
        close(f3);
    check(!bad && sz && same, "share: 30000 random reads, writes and truncates through two handles agree with a model",
          "fault %u at operation %u, size ok %d, whole file same %d (%ld of %ld bytes)", bad, i, sz, same, g, size);
    unlink(p);
    free(model), free(tmp), free(got), free(all);
}

/* A change made behind the box's back shows within about two seconds.  The
 * HAL's hook keeps the box's own changes from dropping what it knows, which
 * is how a change on the Mac looks to it. */
static void share_behind(void)
{
    char p[300], buf[300];
    struct stat st;
    sh_put("o1", "old data of the file", 20);
    sh_put("o3", "to be deleted", 13);
    sh_put("o4", "to be renamed", 13);
    sh_put("o6", "modes", 5);
    char p1[300], p3[300], p4[300], p5[300], p6[300], p2[300];
    sh_path(p1, "o1"), sh_path(p2, "o2"), sh_path(p3, "o3"), sh_path(p4, "o4"), sh_path(p5, "o5"), sh_path(p6, "o6");
    chmod(p6, 0644);
    int rd = open(p1, O_RDONLY);
    long n0 = pread(rd, buf, 20, 0);                /* a chunk is kept */
    int primed = n0 == 20 && stat(p1, &st) == 0 && st.st_size == 20 && stat(p2, &st) == -1 && stat(p3, &st) == 0 &&
                 stat(p4, &st) == 0 && stat(p5, &st) == -1 && stat(p6, &st) == 0;
    long hook = syscall(HAL_SYS_SHARE, 1);
    /* the changes, as if the Mac made them */
    sh_put("o2", "new file", 8);
    int w = open(p1, O_WRONLY);
    pwrite(w, "NEW", 3, 0);
    ftruncate(w, 30);
    close(w);
    unlink(p3);
    rename(p4, p5);
    chmod(p6, 0600);
    syscall(HAL_SYS_SHARE, 0);                      /* the box's own changes drop what is kept again */
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int seen[6] = { 0 };
    double at[6] = { 0 };
    for (;;) {
        char b2[64];
        int g1 = stat(p1, &st) == 0 && st.st_size == 30;
        long r1 = pread(rd, b2, 20, 0);
        int g1d = r1 == 20 && !memcmp(b2, "NEW data of the file", 20) ? 1 : 0;
        int g2 = stat(p2, &st) == 0 && st.st_size == 8;
        int g3 = stat(p3, &st) == -1 && errno == ENOENT;
        int g5 = stat(p5, &st) == 0 && stat(p4, &st) == -1;
        int g6 = stat(p6, &st) == 0 && (st.st_mode & 0777) == 0600;
        int now[6] = { g1, g1d, g2, g3, g5, g6 };
        clock_gettime(CLOCK_MONOTONIC, &t);
        double el = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
        int all = 1;
        for (int i = 0; i < 6; i++) {
            if (now[i] && !seen[i])
                seen[i] = 1, at[i] = el;
            all &= seen[i];
        }
        if (all || el > 2.5)
            break;
        usleep(50000);
    }
    check(hook >= 0 && primed && seen[0] && seen[1] && seen[2] && seen[3] && seen[4] && seen[5],
          "share: changes made behind the box's back (a new file, a write and a longer size, a delete, a rename, a chmod, "
          "and the data read through a handle that holds a chunk) are all seen within 2.5 seconds",
          "hook %ld primed %d seen: size %d data %d new %d delete %d rename %d mode %d; at %.2f %.2f %.2f %.2f %.2f %.2f s",
          hook, primed, seen[0], seen[1], seen[2], seen[3], seen[4], seen[5], at[0], at[1], at[2], at[3], at[4], at[5]);
    close(rd);
    unlink(p1), unlink(p2), unlink(p5), unlink(p6);
}

/* The share under kills, closes, exec and several cores at once, and the HAL's
 * wait for a reply (hal/ninep.c: wfi, the share's interrupt, the timeout).
 * The cases are in tests/hal/p9stress.h; tests/hal/run-soak.py runs them
 * for much longer. */
#define STRESS_ROUNDS 12

static void share_stress(int on_hal)
{
    static struct p9s s;
    memset(&s, 0, sizeof s);
    if (p9s_setup(&s, shd)) {
        check(0, "share stress: the files for the stress cases", "setup failed (errno %d)", errno);
        return;
    }
    static const char *const how[] = {
        "a thread killed with tgkill while its siblings are in share calls",
        "exit_group while the other threads are in share calls",
        "a process killed by another while its threads are in share calls",
        "exit_group from a thread while the others are in share calls",
    };
    for (int h = 0; h < 4; h++) {
        int bad = p9s_kill(&s, STRESS_ROUNDS, h, 3);
        check(!bad, how[h], "%d of %d rounds went wrong: %s", bad, STRESS_ROUNDS, s.why);
        s.why[0] = 0;
    }
    int bad = p9s_exec(&s, STRESS_ROUNDS, "/usr/bin/ssh-keygen", 3);
    check(!bad, "execve while the process's threads are in share calls", "%d of %d rounds went wrong: %s", bad,
          STRESS_ROUNDS, s.why);
    s.why[0] = 0;
    bad = p9s_close_read(&s, STRESS_ROUNDS * 20, 4);
    check(!bad, "a descriptor closed and reopened while reads on it are in the share", "%s", s.why);
    s.why[0] = 0;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    bad = p9s_parallel(&s, 100, cpus < 4 ? 4 : (int)cpus);
    check(!bad, "several threads writing, reading and listing on the share at once: every byte is its own", "%s",
          s.why);
    p9s_cleanup(&s);

    if (!on_hal)
        return;
    /* The wait for a reply is by interrupt: one left asserted is taken
     * and acknowledged as an exception (ninep_irq), and nothing is wedged */
    struct statfs sf;
    long before = syscall(HAL_SYS_SHARE, 5);
    syscall(HAL_SYS_SHARE, 4);                          /* the next clunk's interrupt stays on */
    int r = statfs(shd, &sf);
    long after = before;
    for (int i = 0; i < 200 && after == before; i++) {
        usleep(10000);
        after = syscall(HAL_SYS_SHARE, 5);
    }
    check(r == 0 && after > before, "the share's interrupt, left asserted, is taken and acknowledged",
          "statfs %d, interrupts %ld -> %ld", r, before, after);
    r = statfs(shd, &sf);
    check(r == 0, "the share works after a stray interrupt", "statfs %d", r);
    /* A reply that never comes: -EIO after the limit (1 s for the hook, 30 s otherwise),
     * measured by the timer, and the share is not marked dead */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    syscall(HAL_SYS_SHARE, 6);
    r = statfs(shd, &sf);
    int e = errno;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check(r == -1 && e == EIO && ms >= 900 && ms < 3000, "a reply that never comes ends in EIO after the limit",
          "statfs %d errno %d after %ld ms", r, e, ms);
    r = statfs(shd, &sf);
    check(r == 0, "the share still works after a timeout of the hook", "statfs %d", r);
    long w = syscall(HAL_SYS_SHARE, 7);
    check((w >> 40) > 0, "the HAL slept (wfi) for share replies", "slept %ld, spun %ld, ticks %ld", w >> 40,
          (w >> 20) & 0xFFFFF, w & 0xFFFFF);
}

static void share_checks(void)
{
    struct stat st;
    if (stat("/host", &st) || !S_ISDIR(st.st_mode))
        return;                                     /* no share */
    int on_hal = syscall(HAL_SYS_SHARE, 3) >= 0;
    snprintf(shd, sizeof shd, "/host/rosgd-share-XXXXXX");
    if (!mkdtemp(shd)) {
        check(0, "share: a directory for the share's tests", "mkdtemp failed");
        return;
    }
    share_own_changes();
    share_write_behind(on_hal);
    share_integrity();
    share_model();
    if (on_hal)
        share_behind();
    share_stress(on_hal);
    rmdir(shd);
}
#endif

static void ros_selftest_halsys(void)
{
    halsys_pipe();
#ifdef __linux__
    halsys_counts();
    halsys_signals();
    halsys_tgkill();
    halsys_exec();
    halsys_requeue();
    halsys_netlink();
    halsys_unix_zero();
    halsys_procnet();
    halsys_reboot();
    share_checks();
#endif
}

void ros_selftest_vm(void)
{
    ros_selftest_halsys();
    const size_t page = (size_t)sysconf(_SC_PAGESIZE), n = 4;   /* 16 KB pages when hosted on a Mac */
    uint8_t *m = mmap(NULL, n * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        check(0, "mprotect PROT_NONE keeps the memory", "mmap failed");
        return;
    }
    for (size_t i = 0; i < n * page; i++)
        m[i] = (uint8_t)(i * 7 + 3);
    int none = mprotect(m + page, 2 * page, PROT_NONE);
    unsigned char vec[4] = { 0 };
    int mc = mincore(m, n * page, vec);
    int back = mprotect(m + page, 2 * page, PROT_READ | PROT_WRITE);
    size_t bad = 0;
    for (size_t i = 0; i < n * page; i++)
        bad += m[i] != (uint8_t)(i * 7 + 3);
    /* Also check a page that goes to PROT_NONE and back a second time, with a write in between. */
    m[page] = 0xA5;
    int again = mprotect(m + page, page, PROT_NONE) || mprotect(m + page, page, PROT_READ);
    int kept = m[page] == 0xA5;
    munmap(m, n * page);
    check(!none && !back && !again && !bad && kept && !mc && (vec[1] & 1) && (vec[2] & 1),
          "mprotect PROT_NONE keeps the memory: back to read and write, the pages are as they were; "
          "mincore has them resident (#200)",
          "mprotect %d %d %d, %zu bytes changed, kept %d, mincore %d %d%d%d%d", none, back, again, bad, kept,
          mc, vec[0] & 1, vec[1] & 1, vec[2] & 1, vec[3] & 1);
}
