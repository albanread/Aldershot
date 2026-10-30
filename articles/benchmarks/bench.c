/* bench.c -- the language comparison in C (long long throughout: the Intel box's long is 32 bits): fib(30), a sieve of the primes
 * below 2,000,000, and 200,000 string keys in a hash table (open
 * addressing, doubled when half full).  clock() counts centiseconds in the
 * box, so each test is repeated until half a second has gone, best of 3. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* RISC OS's C library has no strdup */
static char *dupstr(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }
#include <time.h>

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static long long sieve(void)
{
    int n = 2000000;
    char *s = calloc(n + 1, 1);
    long long c = 0;
    for (int i = 2; i <= n; i++)
        if (!s[i]) {
            c++;
            for (long long j = (long long)i * i; j <= n; j += i) s[j] = 1;
        }
    free(s);
    return c;
}

typedef struct { char *k; long long v; } slot;

static unsigned hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static slot *find(slot *t, unsigned cap, const char *k)
{
    unsigned i = hash(k) & (cap - 1);
    while (t[i].k && strcmp(t[i].k, k)) i = (i + 1) & (cap - 1);
    return &t[i];
}

static long long dict(void)
{
    unsigned cap = 8, used = 0;
    slot *t = calloc(cap, sizeof *t);
    char key[16];
    long long s = 0;
    for (int i = 1; i <= 200000; i++) {
        sprintf(key, "k%d", i);
        slot *e = find(t, cap, key);
        if (!e->k) {
            e->k = dupstr(key);
            if (++used * 2 > cap) {
                unsigned nc = cap * 2;
                slot *nt = calloc(nc, sizeof *nt);
                for (unsigned j = 0; j < cap; j++)
                    if (t[j].k) *find(nt, nc, t[j].k) = t[j];
                free(t);
                t = nt, cap = nc;
                e = find(t, cap, key);
            }
        }
        e->v = (long long)i * 2;
    }
    for (int i = 1; i <= 200000; i++) {
        sprintf(key, "k%d", i);
        s += find(t, cap, key)->v;
    }
    for (unsigned j = 0; j < cap; j++) free(t[j].k);
    free(t);
    return s;
}

static long long work(int w) { return w == 0 ? fib(30) : w == 1 ? sieve() : dict(); }

int main(void)
{
    static const char *names[] = { "fib", "sieve", "dict" };
    for (int w = 0; w < 3; w++) {
        double best = 1e30;
        long long r = 0;
        for (int k = 0; k < 3; k++) {
            clock_t t = clock(), e;
            int runs = 0;
            do { r = work(w); runs++; e = clock() - t; } while (e < CLOCKS_PER_SEC / 2);
            double ms = (double)e * 1000.0 / CLOCKS_PER_SEC / runs;
            if (ms < best) best = ms;
        }
        printf("c %s %.1f %lld\n", names[w], best, r);
    }
    return 0;
}
