/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* resolver.c -- the Resolver, rewritten for ROSGD as a native module.
 *
 * RISC OS 5's Resolver (0.76) is a DNS client of its own. It sends queries
 * over the Internet module's UDP sockets, from name servers in
 * Inet$Resolvers, and keeps a cache. RISC OS ships it as a binary with no
 * source, so its behaviour was read from that binary's code.
 * Under ROSGD the host already has a resolver: getaddrinfo(), configured
 * by the kernel's DHCP through /etc/resolv.conf. So this module is the
 * interface over it: the four SWIs of chunk &46000, and RISC OS's hostent.
 *
 * What clients depend on, this keeps:
 *
 *   - A hostent in the RMA, which the cache owns and clients read directly:
 *     name, aliases, address type (2), address length (4), address list.
 *     IPv4 only, as the original is.
 *   - GetHostByName blocks. GetHost with a name in R0 does not. It answers
 *     36, EINPROGRESS, until the lookup has ended, and clients poll. With
 *     R0 = 0 it is a blocking lookup of the address R1 points to.
 *   - A name with a character outside letters, digits, '-', '.' and '_' is a
 *     bad parameter, &818040, before any lookup.
 *   - With no name server configured, a name that is neither a dotted
 *     address nor in the hosts file gives "No DNS service configured",
 *     &818042, at once. It does not start a query that waits to time out.
 *
 * A non-blocking lookup runs on a thread of its own. The thread calls only
 * getaddrinfo(), and leaves its answer in memory of its own. The next
 * GetHost, on the runtime's thread, turns that into the RMA hostent.
 * Nothing of the runtime is touched from the thread.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "resolver.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"

#define ERROR_BASE     0x818040u
#define ERR_BAD_PARAMS 0
#define ERR_NO_DNS     2
#define ERR_NO_MEMORY  6

#define EINPROGRESS_RO 36u      /* RISC OS's errno, as the status */
#define STATUS_NOT_FOUND 1u     /* h_errno's, as the failure status */
#define STATUS_TRY_AGAIN 2u

#define MAX_ADDRS 16

enum { PENDING, FOUND, FAILED };

/* A lookup's answer, as the worker thread leaves it: host memory. */
struct answer {
    _Atomic int state;
    uint32_t status;
    unsigned n;
    uint8_t addr[MAX_ADDRS][4];
    char canon[256];
};

/* A cache entry, in the RMA. */
struct entry {
    uint32_t next;              /* arena address of the next entry, or 0 */
    uint32_t name;              /* arena address of the name, lower case */
    uint32_t state;
    uint32_t status;
    uint32_t hostent;           /* arena address, when FOUND */
    uint64_t answer;            /* host pointer to a struct answer, PENDING */
};

struct workspace {
    uint32_t cache;             /* the first entry, or 0 */
};

struct ros_module resolver_module;

static struct workspace *ws(void)
{
    return ros_ptr(ros_ld32(resolver_module.private_word));
}

static os_error *error(unsigned n, const char *text)
{
    return ros_error(ERROR_BASE + n, "%s", text);
}

/* The original's test: letters, digits, '-', '.' and '_' only. */
static int valid_name(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '-' || *s == '.' || *s == '_'))
            return 0;
    return 1;
}

/* ---- what needs no name server ------------------------------------------- */

/* Whether /etc/resolv.conf names a name server: the kernel's DHCP writes one
 * there when the box has a network. */
static int have_dns(void)
{
    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[256];
    int ns = 0;
    while (f && !ns && fgets(line, sizeof line, f))
        ns = strncmp(line, "nameserver", 10) == 0;
    if (f)
        fclose(f);
    return ns;
}

/* A dotted address, or a name in /etc/hosts: 1 with *a filled, else 0. */
static int local_lookup(const char *name, struct answer *a)
{
    struct in_addr in;
    a->n = 0;
    snprintf(a->canon, sizeof a->canon, "%s", name);
    if (inet_pton(AF_INET, name, &in) == 1) {
        memcpy(a->addr[a->n++], &in, 4);
    } else {
        FILE *f = fopen("/etc/hosts", "r");
        char line[512];
        while (f && a->n < MAX_ADDRS && fgets(line, sizeof line, f)) {
            char *hash = strchr(line, '#'), *save = NULL;
            if (hash)
                *hash = 0;
            char *ip = strtok_r(line, " \t\r\n", &save), *w;
            if (!ip || inet_pton(AF_INET, ip, &in) != 1)
                continue;
            int first = 1;
            char *canon = NULL;
            while ((w = strtok_r(NULL, " \t\r\n", &save))) {
                if (first)
                    canon = w, first = 0;
                if (strcasecmp(w, name) == 0) {
                    memcpy(a->addr[a->n++], &in, 4);
                    snprintf(a->canon, sizeof a->canon, "%s", canon);
                    break;
                }
            }
        }
        if (f)
            fclose(f);
    }
    a->status = a->n ? 0 : STATUS_NOT_FOUND;
    atomic_store(&a->state, a->n ? FOUND : FAILED);
    return a->n != 0;
}

/* ---- the lookup, on any thread ------------------------------------------ */

static void lookup(const char *name, struct answer *a)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM,
                              .ai_flags = AI_CANONNAME };
    struct addrinfo *res = NULL;
    int r = getaddrinfo(name, NULL, &hints, &res);
    a->n = 0;
    snprintf(a->canon, sizeof a->canon, "%s", name);
    if (r == 0) {
        if (res && res->ai_canonname && *res->ai_canonname)
            snprintf(a->canon, sizeof a->canon, "%s", res->ai_canonname);
        for (struct addrinfo *p = res; p && a->n < MAX_ADDRS; p = p->ai_next) {
            const uint8_t *ip = (const uint8_t *)&((struct sockaddr_in *)p->ai_addr)->sin_addr;
            int dup = 0;
            for (unsigned k = 0; k < a->n; k++)
                dup |= memcmp(a->addr[k], ip, 4) == 0;
            if (!dup)
                memcpy(a->addr[a->n++], ip, 4);
        }
        freeaddrinfo(res);
    }
    a->status = a->n ? 0 : r == EAI_AGAIN ? STATUS_TRY_AGAIN : STATUS_NOT_FOUND;
    atomic_store(&a->state, a->n ? FOUND : FAILED);
}

struct job {
    struct answer *a;
    char name[256];
};

static void *worker(void *arg)
{
    struct job *job = arg;
    ros_thread_signal_stack();
    lookup(job->name, job->a);
    free(job);
    return NULL;
}

/* A lookup of name into a, on a thread of its own; with no thread, now.
 * 0, or -1 out of memory. */
static int start_lookup(const char *name, struct answer *a)
{
    struct job *job = malloc(sizeof *job);
    if (!job)
        return -1;
    atomic_init(&a->state, PENDING);
    job->a = a;
    snprintf(job->name, sizeof job->name, "%s", name);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, worker, job) != 0)
        worker(job);
    pthread_attr_destroy(&attr);
    return 0;
}

/* A reverse lookup, getnameinfo on a thread of its own, the caller asleep
 * (ros_sleep_fd) until it has ended: a task window's desktop goes on */
struct reverse {
    struct sockaddr_in sin;
    char name[256];
    int r;
    atomic_int done;
};

static void *reverse_worker(void *arg)
{
    struct reverse *rv = arg;
    ros_thread_signal_stack();
    rv->r = getnameinfo((struct sockaddr *)&rv->sin, sizeof rv->sin, rv->name, sizeof rv->name, NULL, 0,
                        NI_NAMEREQD);
    atomic_store(&rv->done, 1);
    return NULL;
}

static int reverse_lookup(struct reverse *rv)
{
    atomic_init(&rv->done, 0);
    pthread_t t;
    if (pthread_create(&t, NULL, reverse_worker, rv) != 0) {
        ROS_BLOCKING(reverse_worker(rv));
        return rv->r;
    }
    while (!atomic_load(&rv->done)) {
        os_error *slept;
        ros_sleep_fd(-1, 0, 10, &slept);        /* an Escape waits for the lookup's end */
    }
    pthread_join(t, NULL);
    return rv->r;
}

/* ---- hostents and the cache (the runtime's thread) ---------------------- */

/* RISC OS's hostent, and everything it points to, in one RMA block. */
static uint32_t hostent(const struct answer *a)
{
    uint32_t namelen = (uint32_t)strlen(a->canon) + 1;
    uint32_t size = 20 + 4 + 4 * (a->n + 1) + 4 * a->n + ((namelen + 3) & ~3u);
    uint8_t *p = ros_rma_alloc(size);
    if (!p)
        return 0;
    uint32_t h = ros_addr(p), aliases = h + 20, list = aliases + 4;
    uint32_t addrs = list + 4 * (a->n + 1), name = addrs + 4 * a->n;
    ros_st32(h, name);
    ros_st32(h + 4, aliases);
    ros_st32(h + 8, 2);                         /* AF_INET */
    ros_st32(h + 12, 4);
    ros_st32(h + 16, list);
    ros_st32(aliases, 0);
    for (uint32_t i = 0; i < a->n; i++) {
        ros_st32(list + 4 * i, addrs + 4 * i);
        memcpy(ros_ptr(addrs + 4 * i), a->addr[i], 4);
    }
    ros_st32(list + 4 * a->n, 0);
    memcpy(ros_ptr(name), a->canon, namelen);
    return h;
}

static struct entry *find(const char *name)
{
    for (uint32_t e = ws()->cache; e; e = ((struct entry *)ros_ptr(e))->next) {
        struct entry *en = ros_ptr(e);
        if (strcasecmp(ros_ptr(en->name), name) == 0)
            return en;
    }
    return NULL;
}

static struct entry *add(const char *name)
{
    struct entry *en = ros_rma_alloc(sizeof *en);
    char *copy = ros_rma_alloc((uint32_t)strlen(name) + 1);
    if (!en || !copy) {
        if (en)
            ros_rma_free(en);
        if (copy)
            ros_rma_free(copy);
        return NULL;
    }
    for (size_t i = 0; i <= strlen(name); i++)
        copy[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    memset(en, 0, sizeof *en);
    en->name = ros_addr(copy);
    en->state = PENDING;
    en->next = ws()->cache;
    ws()->cache = ros_addr(en);
    return en;
}

/* Remove the entry just added, at the head of the cache: nothing was looked up. */
static void forget(struct entry *en)
{
    ws()->cache = en->next;
    ros_rma_free(ros_ptr(en->name));
    ros_rma_free(en);
}

/* A pending entry whose answer has come: FOUND with a hostent, or FAILED. */
static void settle(struct entry *en)
{
    struct answer *a = (struct answer *)(uintptr_t)en->answer;
    if (en->state != PENDING || !a || atomic_load(&a->state) == PENDING)
        return;
    en->status = a->status;
    en->state = atomic_load(&a->state);
    if (en->state == FOUND && !(en->hostent = hostent(a))) {
        en->state = FAILED;
        en->status = STATUS_TRY_AGAIN;
    }
    free(a);
    en->answer = 0;
}

static void reply(const struct entry *en, uint32_t *status, uint32_t *h)
{
    *status = en->state == FOUND ? 0 : en->state == PENDING ? EINPROGRESS_RO : en->status;
    *h = en->state == FOUND ? en->hostent : 0;
}

static void flush(void)
{
    uint32_t keep = 0;
    for (uint32_t e = ws()->cache, next; e; e = next) {
        struct entry *en = ros_ptr(e);
        next = en->next;
        if (en->state == PENDING) {             /* its thread still writes the answer */
            en->next = keep;
            keep = e;
            continue;
        }
        if (en->hostent)
            ros_rma_free(ros_ptr(en->hostent));
        ros_rma_free(ros_ptr(en->name));
        ros_rma_free(en);
    }
    ws()->cache = keep;
}

/* ---- the SWIs ------------------------------------------------------------ */

os_error *xresolver_get_host_by_name(const char *host_name, uint32_t *status, uint32_t *h)
{
    if (!host_name || !valid_name(host_name))
        return error(ERR_BAD_PARAMS, "Bad parameters to Resolver SWI");
    struct entry *en = find(host_name);
    if (!en) {
        if (!(en = add(host_name)))
            return error(ERR_NO_MEMORY, "No free memory for Resolver");
        struct answer *a = calloc(1, sizeof *a);
        if (!a) {
            forget(en);                 /* else it stays PENDING with no answer, and waits for ever */
            return error(ERR_NO_MEMORY, "No free memory for Resolver");
        }
        if (!local_lookup(host_name, a)) {
            if (!have_dns()) {
                free(a);
                forget(en);
                return error(ERR_NO_DNS, "No DNS service configured");
            }
            if (start_lookup(host_name, a)) {       /* on a thread, waited for below */
                free(a);
                forget(en);
                return error(ERR_NO_MEMORY, "No free memory for Resolver");
            }
        }
        en->answer = (uintptr_t)a;
    }
    /* The lookup, this one's or one that GetHost started, is waited for
     * asleep, as the original sleeps on UpCall 6. A task window's desktop
     * goes on (runtime/sleep.c). Escape there ends the wait and not the
     * lookup. */
    while (en->state == PENDING) {
        settle(en);
        if (en->state == PENDING) {
            os_error *slept;
            if (ros_sleep_fd(-1, 0, 10, &slept) < 0)
                return slept;
        }
    }
    reply(en, status, h);
    return NULL;
}

os_error *xresolver_get_host(uint32_t host_name, uint32_t address, uint32_t *status,
                             uint32_t *h)
{
    char name[256];
    if (!host_name) {
        /* By address: its in-addr.arpa name, looked up blocking. */
        if (!address)
            return error(ERR_BAD_PARAMS, "Bad parameters to Resolver SWI");
        struct reverse rv = { .sin = { .sin_family = AF_INET } };
#ifdef __APPLE__
        rv.sin.sin_len = sizeof rv.sin;
#endif
        memcpy(&rv.sin.sin_addr, ros_ptr(address), 4);
        struct answer a = { 0 };
        int r = reverse_lookup(&rv);
        snprintf(name, sizeof name, "%s", rv.name);
        if (r != 0) {
            *status = STATUS_NOT_FOUND;
            *h = 0;
            return NULL;
        }
        snprintf(a.canon, sizeof a.canon, "%s", name);
        memcpy(a.addr[0], ros_ptr(address), 4);
        a.n = 1;
        struct entry *en = find(name);
        if (!en || en->state != FOUND) {
            if (!en && !(en = add(name)))
                return error(ERR_NO_MEMORY, "No free memory for Resolver");
            if (!(en->hostent = hostent(&a)))
                return error(ERR_NO_MEMORY, "No free memory for Resolver");
            en->state = FOUND;
            en->status = 0;
        }
        reply(en, status, h);
        return NULL;
    }

    snprintf(name, sizeof name, "%s", (const char *)ros_ptr(host_name));
    if (!valid_name(name))
        return error(ERR_BAD_PARAMS, "Bad parameters to Resolver SWI");
    struct entry *en = find(name);
    if (en) {
        settle(en);
        reply(en, status, h);
        return NULL;
    }
    if (!(en = add(name)))
        return error(ERR_NO_MEMORY, "No free memory for Resolver");
    struct answer *a = calloc(1, sizeof *a);
    if (a && local_lookup(name, a)) {
        en->answer = (uintptr_t)a;
        settle(en);
        reply(en, status, h);
        return NULL;
    }
    if (a && !have_dns()) {
        free(a);
        forget(en);
        return error(ERR_NO_DNS, "No DNS service configured");
    }
    if (!a || start_lookup(name, a)) {
        free(a);
        forget(en);                     /* else it stays PENDING with no answer, and waits for ever */
        return error(ERR_NO_MEMORY, "No free memory for Resolver");
    }
    en->answer = (uintptr_t)a;
    reply(en, status, h);
    return NULL;
}

static os_error *reconfigure(void);

os_error *xresolver_cache_control(uint32_t reason)
{
    /* 0-2 flush (the original's masks select which entries; all go here);
     * 3 re-reads the configuration, as *ResolverConfig does;
     * 4-9 set cache policy, which the host's resolver owns. */
    if (reason <= 2)
        flush();
    else if (reason == 3)
        return reconfigure();
    return NULL;
}

/* ---- *ResolverConfig ------------------------------------------------------- */

/* The original reads its configuration from the Inet$ variables: name
 * servers from Inet$Resolvers, the domains from Inet$LocalDomain and
 * Inet$SearchDomains, and timing from Inet$ResolverDelay and
 * Inet$ResolverRetries. !Internet's Startup runs *ResolverConfig after
 * setting them. Here the resolver is the host's, so the variables become
 * the host's configuration. In the box, where ROSGD is /init and the
 * system is its own, /etc/resolv.conf is written from them (and linked
 * back to the kernel's DHCP answer, /proc/net/pnp, when Inet$Resolvers is
 * empty). Hosted, the host's resolver is the host's business and is left
 * alone. Either way the cache is flushed, as the original's is, so that
 * names are looked up afresh. */

static void read_var(const char *name, char *out, size_t max)
{
    size_t nl = strlen(name) + 1;
    char *b = ros_rma_alloc(nl + max);
    out[0] = 0;
    if (!b)
        return;
    memcpy(b, name, nl);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(b), s.r[1] = ros_addr(b + nl), s.r[2] = (uint32_t)(max - 1), s.r[3] = 0, s.r[4] = 3;
    ros_swi(&s, ROS_X_BIT | 0x23);                  /* OS_ReadVarVal, expanded */
    if (!s.v) {
        size_t n = s.r[2] < max - 1 ? s.r[2] : max - 1;
        memcpy(out, b + nl, n);
        out[n] = 0;
    }
    ros_rma_free(b);
}

#define RESOLV_CONF "/etc/resolv.conf"

static os_error *write_resolv_conf(void)
{
    char servers[256], local[256], search[256], delay[16], retries[16];
    read_var("Inet$Resolvers", servers, sizeof servers);
    read_var("Inet$LocalDomain", local, sizeof local);
    read_var("Inet$SearchDomains", search, sizeof search);
    read_var("Inet$ResolverDelay", delay, sizeof delay);
    read_var("Inet$ResolverRetries", retries, sizeof retries);

    char text[1024];
    size_t n = 0;
    for (char *save, *ns = strtok_r(servers, " ,", &save); ns; ns = strtok_r(NULL, " ,", &save)) {
        struct in_addr a;
        if (inet_pton(AF_INET, ns, &a) != 1)
            return ros_error(ERROR_BASE + ERR_BAD_PARAMS, "Inet$Resolvers: %s is not a name server's address", ns);
        n += (size_t)snprintf(text + n, sizeof text - n, "nameserver %s\n", ns);
    }
    if (!n) {                                       /* back to the kernel's DHCP answer */
        unlink(RESOLV_CONF);
        if (access("/proc/net/pnp", R_OK) == 0)
            symlink("/proc/net/pnp", RESOLV_CONF);
        return NULL;
    }
    if (local[0])
        n += (size_t)snprintf(text + n, sizeof text - n, "domain %s\n", local);
    if (search[0] && n < sizeof text)
        n += (size_t)snprintf(text + n, sizeof text - n, "search %s\n", search);
    long d = strtol(delay, NULL, 10), r = strtol(retries, NULL, 10);
    if (d > 0 && n < sizeof text)
        n += (size_t)snprintf(text + n, sizeof text - n, "options timeout:%ld\n", d);
    if (r > 0 && n < sizeof text)
        n += (size_t)snprintf(text + n, sizeof text - n, "options attempts:%ld\n", r);
    if (n >= sizeof text)
        return error(ERR_BAD_PARAMS, "The Inet$ resolver variables are too long");

    FILE *f = fopen(RESOLV_CONF ".new", "w");
    int bad = !f;
    if (f) {
        bad = fwrite(text, 1, n, f) != n;
        bad |= fclose(f) == EOF;
    }
    if (bad || rename(RESOLV_CONF ".new", RESOLV_CONF) != 0)
        return ros_error(ERROR_BASE + ERR_BAD_PARAMS, "Cannot write %s: %s", RESOLV_CONF, strerror(errno));
    return NULL;
}

static os_error *reconfigure(void)
{
    for (uint32_t e = ws()->cache; e; e = ((struct entry *)ros_ptr(e))->next)
        if (((struct entry *)ros_ptr(e))->state == PENDING)
            return error(ERR_BAD_PARAMS, "Resolver is busy");
    os_error *err = getpid() == 1 ? write_resolv_conf() : NULL;
    flush();
    return err;
}

static os_error *cmd_resolverconfig(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    return reconfigure();
}

static const struct ros_command commands[] = {
    { "ResolverConfig", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *ResolverConfig",
      "*ResolverConfig reads the resolver configuration from the Inet$ variables.", cmd_resolverconfig },
    { 0 },
};

/* ---- the module ---------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    ros_st32(m->private_word, ros_addr(w));
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    for (uint32_t e = ws()->cache; e; e = ((struct entry *)ros_ptr(e))->next)
        if (((struct entry *)ros_ptr(e))->state == PENDING)
            return error(ERR_BAD_PARAMS, "Resolver is busy");
    flush();
    ros_rma_free(ws());
    ros_st32(m->private_word, 0);
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ROS_ERR_NO_SUCH_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module resolver_module = {
    .title = "Resolver",
    .help = "Resolver\t0.76 (24 Sep 2026) ROSGD native, over the host's resolver",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .commands = commands,
    .swi_chunk = 0x46000,
    .swi_thunks = ros_swi_thunks_Resolver,
    .swi_names = ros_swi_names_Resolver,
    .swi_prefix = "Resolver",
};

__attribute__((constructor)) static void count(void)
{
    resolver_module.swi_count = ros_swi_count_Resolver;
}
