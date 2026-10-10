/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation in C of RISC OS Open's kernel source
 * (Sources/Kernel: s.ModHand, s.UnSqueeze).
 */

/* module.c: the module chain, and OS_Module.
 *
 * A module is initialised as RISC OS initialises one. A private word is
 * made for it, then its initialise entry runs. The entry is either native
 * or compiled code entered through the dispatcher with R10 -> the command
 * tail, R11 = the number of incarnations it already has, and R12 -> the
 * private word. A compiled entry returns as the kernel expects: V clear, or
 * V set and R0 -> an error.
 *
 * OS_Module is the kernel's (Kernel/s/ModHand), reimplemented:
 *
 *   - Incarnations: a module may run more than once, as "Title%Postfix".
 *     Each incarnation is a node in the RMA in the kernel's layout. The
 *     node holds the link, the private word and the postfix. So OS_Module's
 *     R5 points at a postfix and R12 at a private word as ever. The first
 *     is "Base". The preferred one, first in the list, takes the module's
 *     SWIs. Every incarnation gets service calls.
 *   - Names match as the kernel's do: without case, and abbreviable with
 *     ".".
 *   - Killing a module (OS_Module 4, *RMKill) calls its finalise entry,
 *     frees the workspace its private word points at, as the kernel does,
 *     and takes it out of the chain. A ROM module can be started again
 *     (OS_Module 3, *RMReInit). Service_ModulePostInit and PostFinal are
 *     issued around each.
 *   - Every module in the image is a ROM module. A native one gets a module
 *     header built in the RMA, with its title, help, SWI chunk and names.
 *     So whatever reads a module through its header (OS_Module 12, 18, 19)
 *     reads a native one too.
 *   - The RMA is claimed from as the kernel claims: in blocks of 32n bytes.
 *
 *   - Entering a module as the application (2) runs its start entry,
 *     compiled or native, as the kernel's EnterIt does. It does not
 *     return.
 *
 *   - Loading a module from a file (OS_Module 1, *RMLoad; 0, *RMRun, which
 *     then enters it) is the kernel's Load_Module. The file must be of type
 *     Module. It is read into the RMA and a module of the same title is
 *     killed first. Then the new module is linked and initialised with the
 *     tail after the file's name. The file is either an x32 C module's
 *     image (A64X32 in the Apple Silicon box), which is the ELF32 that
 *     roscc link --module writes, placed as the ROM's are
 *     (ros_module_image_load), or a RISC OS module of ARM code. The ARM
 *     container runs the ARM code where it was loaded (place_module).
 *     OS_Module 10 and 11 place an image already in memory the same way.
 *     When a loaded module is killed, its image is freed. So *RMReInit
 *     cannot bring it back, as on RISC OS.
 *
 *   - ARM shadows (#147): an ARM module with the title of a native module
 *     does not replace it. If the title may be shadowed (policy_of: the
 *     ROM's column, ROSGD$Shadowable), the ARM module becomes the native
 *     module's shadow. It is out of the chain and hangs off its twin. ARM
 *     callers (switrace.h's caller kind) reach it for SWIs and OS_Module
 *     lookups, and native callers reach the twin. If the title may not be
 *     shadowed, the load is absorbed. It succeeds and nothing loads.
 *     A native module joining while an ARM module holds its title demotes
 *     the ARM one to its shadow, workspace and all.
 *     SharedCLibrary's shadow is built in. It is 5.30's, made at the first
 *     ARM reference (runtime/armrun/box.c).
 *     Shadows get service calls in a second pass, after every chain module,
 *     by each service's class (service_class below). A shadow's own
 *     PostInit and PostFinal go to shadows only. A shadow's vector claims,
 *     variables and ResourceFS files are scoped where they are made
 *     (vector.c, sysvars.c, resourcefs.c).
 *     *ARMPrefer (and rosgd.armprefer=) sends a title's native callers to
 *     its shadow too, for testing.
 *
 * Not here: podule modules (17), because there are no podules.
 */
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/armbox.h"
#include "rosgd/background.h"
#include "rosgd/capp.h"
#include "rosgd/heap.h"
#include "rosgd/platform.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/rom.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "rosgd/task.h"
#include "rosgd/environment.h"

#define SERVICE_MODULE_POST_INIT  0xDAu
#define SERVICE_MODULE_POST_FINAL 0xDBu

#define ERR_NOT_MOD           0x100u
#define ERR_MH_NO_ROOM        0x101u
#define ERR_RM_NOT_FOUND      0x102u
#define ERR_CANT_KILL         0x103u
#define ERR_BAD_MODULE_REASON 0x105u
#define ERR_NO_MORE_MODULES   0x108u
#define ERR_NO_MORE_INCS      0x109u
#define ERR_POSTFIX_NEEDED    0x10Au
#define ERR_INC_EXISTS        0x10Bu
#define ERR_INC_NOT_FOUND     0x10Cu
#define ERR_MODULE_POSTFIX    0x107u
#define ERR_RM_ALIGNMENT      0x117u
#define ERR_BAD_PARAMETERS    0x1EAu

static struct ros_module *chain;
static struct ros_module *rom[1024];   /* the chain's every module, as
                                        * enumerated by OS_Module 20 */
static unsigned rom_count;

struct ros_module *ros_module_first(void)
{
    return chain;
}

struct ros_module *ros_module_for_swi(uint32_t number)
{
    for (struct ros_module *m = chain; m; m = m->next)
        if (m->swi_chunk && number - m->swi_chunk < 64)
            return m;
    return NULL;
}

static uint32_t upper(uint32_t c)
{
    return c >= 'a' && c <= 'z' ? c - 0x20 : c;
}

/* ---- ARM shadows ------------------------------------------------------ */

static int same_title(const char *a, const char *b)
{
    for (; *a && upper((uint8_t)*a) == upper((uint8_t)*b); a++, b++)
        ;
    return !*a && !*b;
}

/* Whether ROSGD$Shadowable lists title. The variable is a comma-separated
 * list of titles, and spaces round each are allowed. */
static int variable_lists(const char *title)
{
    static const char name[] = "ROSGD$Shadowable";
    char *b = ros_rma_alloc(sizeof name + 256);
    if (!b)
        return 0;
    memcpy(b, name, sizeof name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b), c.r[1] = ros_addr(b + sizeof name), c.r[2] = 255, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, 0x20000u | 0x23u);              /* XOS_ReadVarVal, expanded */
    int found = 0;
    if (!c.v && c.r[2] < 256) {
        char *v = b + sizeof name;
        v[c.r[2]] = 0;
        char *save = NULL;
        for (char *t = strtok_r(v, ", ", &save); t && !found; t = strtok_r(NULL, ", ", &save))
            found = same_title(t, title);
    }
    ros_rma_free(b);
    return found;
}

/* A native module's shadow policy. The ROM's modules have theirs from the
 * column in boot/rom_contents.c, set as runtime/rom.c starts them. A policy
 * is one of three things. The module is shadowable (the libraries: MimeMap,
 * Squash, ZLib, CompressPNG, CompressJPEG, BASICVFP, BootCommands,
 * ColourPicker). Or it is shadowed by the built-in ARM module
 * (SharedCLibrary). Or it is never shadowed, and an ARM load of the title
 * is absorbed. A module with no policy of its own (one loaded from a file)
 * has the policy of a ROM module of the same title. Failing that, it has
 * ROSGD$Shadowable's: shadowable if the variable lists its title, else
 * never. */
static int policy_of(const struct ros_module *m)
{
    if (m->shadow_policy)
        return m->shadow_policy;
    for (unsigned i = 0; i < rom_count; i++)
        if (rom[i] != m && rom[i]->shadow_policy && rom[i]->title && same_title(rom[i]->title, m->title))
            return rom[i]->shadow_policy;
    return variable_lists(m->title) ? ROS_SHADOW_ALLOW : ROS_SHADOW_NEVER;
}

/* ---- the escape hatch: *ARMPrefer, rosgd.armprefer= ---------------------- */

/* The titles whose native callers are sent to the shadow. A chain module
 * of one of these titles has prefer_arm set as it joins. While it has a
 * shadow, native callers reach the shadow too. */
#define PREFER_MAX 16
static char preferred[PREFER_MAX][32];

static int is_preferred(const char *title)
{
    for (unsigned i = 0; i < PREFER_MAX; i++)
        if (preferred[i][0] && same_title(preferred[i], title))
            return 1;
    return 0;
}

os_error *ros_module_armprefer(const char *title, int on)
{
    size_t n = strlen(title);
    if (!n || n >= sizeof preferred[0])
        return ros_error(ERR_RM_NOT_FOUND, "Module '%s' not found", title);
    if (same_title(title, "SharedCLibrary"))
        return ros_error(ERR_BAD_PARAMETERS, "SharedCLibrary's ARM shadow serves ARM code only: "
                         "native programs cannot use it");
    for (struct ros_module *m = chain; m; m = m->next)
        if (!m->arm && same_title(m->title, title) && policy_of(m) == ROS_SHADOW_BUILTIN)
            return ros_error(ERR_BAD_PARAMETERS, "%s's ARM shadow serves ARM code only", m->title);
    int at = -1;
    for (unsigned i = 0; i < PREFER_MAX; i++)
        if (preferred[i][0] ? same_title(preferred[i], title) : at < 0)
            at = (int)i;
    if (on) {
        if (at < 0)
            return ros_error(ERR_BAD_PARAMETERS, "Too many titles preferred (%u)", PREFER_MAX);
        memcpy(preferred[at], title, n + 1);
    } else if (at >= 0 && preferred[at][0] && same_title(preferred[at], title)) {
        preferred[at][0] = 0;
    }
    for (struct ros_module *m = chain; m; m = m->next)
        if (!m->arm && same_title(m->title, title))
            m->prefer_arm = (uint8_t)(on != 0);
    return NULL;
}

const char *ros_module_armpreferred(unsigned i)
{
    for (unsigned k = 0; k < PREFER_MAX; k++)
        if (preferred[k][0] && !i--)
            return preferred[k];
    return NULL;
}

/* The module that a caller of a given kind reaches for chain module m.
 * If make is set, the built-in SharedCLibrary is made when an ARM caller
 * needs it. */
static struct ros_module *route(struct ros_module *m, int kind, int make)
{
    if (!m || m->twin)
        return m;
    if (kind == ROS_KIND_ARM && make && !m->shadow && !m->arm &&
        m->shadow_policy == ROS_SHADOW_BUILTIN)
        ros_armrun_builtin_clib(m);     /* sets m->shadow, or fails and the native one answers */
    if (m->shadow && (kind == ROS_KIND_ARM || m->prefer_arm))
        return m->shadow;
    return m;
}

struct ros_module *ros_module_route(struct ros_module *m, int kind)
{
    return route(m, kind, 1);
}

struct ros_module *ros_module_route_existing(struct ros_module *m, int kind)
{
    return route(m, kind, 0);
}

struct ros_module *ros_module_for_swi_kind(uint32_t number, int kind)
{
    for (struct ros_module *m = chain; m; m = m->next) {
        if (m->swi_chunk && number - m->swi_chunk < 64)
            return route(m, kind, 1);
        /* A shadow whose chunk is not its twin's. Only an ARM caller reaches it. */
        if (kind == ROS_KIND_ARM && m->shadow && m->shadow->swi_chunk &&
            number - m->shadow->swi_chunk < 64)
            return m->shadow;
    }
    return NULL;
}

/* A shadow initialising on this thread. Its vector claims are a shadow's
 * before it is linked to its twin. The ARM code running cannot change a
 * system variable that already exists. */
static _Thread_local struct ros_module *initialising;

struct ros_module *ros_module_shadow_initialising(void)
{
    return initialising;
}

static int holds(const struct ros_module *m, uint32_t addr)
{
    return m && m->arm && m->base && addr - m->base < m->image_end - m->base;
}

struct ros_module *ros_module_shadow_at(uint32_t addr)
{
    if (holds(initialising, addr))
        return initialising;
    for (struct ros_module *m = chain; m; m = m->next)
        if (m->shadow && holds(m->shadow, addr))
            return m->shadow;
    return NULL;
}

struct ros_module *ros_module_arm_at(uint32_t addr)
{
    if (holds(initialising, addr))
        return initialising;
    for (struct ros_module *m = chain; m; m = m->next) {
        if (holds(m, addr))
            return m;
        if (m->shadow && holds(m->shadow, addr))
            return m->shadow;
    }
    return NULL;
}

/* ---- incarnation nodes: [0] link, [4] private word, [8] postfix -------------------- */

static uint32_t inc_next(uint32_t node) { return ros_ld32(node); }
static uint32_t inc_word(uint32_t node) { return node + 4; }
static uint32_t inc_postfix(uint32_t node) { return node + 8; }

static uint32_t new_node(uint32_t postfix)
{
    uint32_t n = 0;
    while (ros_ld8(postfix + n) > ' ')
        n++;
    uint8_t *b = ros_rma_alloc(8 + n + 1);
    if (!b)
        return 0;
    uint32_t node = ros_addr(b);
    ros_st32(node, 0);
    ros_st32(node + 4, 0);
    memcpy(b + 8, ros_ptr(postfix), n);
    b[8 + n] = 0;
    return node;
}

/* The preferred incarnation's private word is the module's. */
static void prefer_first(struct ros_module *m)
{
    m->private_word = m->incarnations ? inc_word(m->incarnations) : 0;
}

/* ---- entering a module ---------------------------------------------------------------- */

/* Enter compiled module code from native code, catching anything raised
 * on the way. A module's own errors come back as V and R0. A fault in
 * compiled code, such as a call to an address nothing compiled, is raised.
 *
 * Module code runs inside the OS (OS_Module, the kernel starting up) and
 * never on the way out to the caller. So the SWIs it makes are not the
 * outermost. Background work and callbacks wait, as they wait in RISC OS
 * until the return to user mode. At depth 0 they would run on the top of
 * the SVC stack, over this code's own frame. The Wimp's initialisation
 * lost its return address that way. */
static os_error *enter(struct ros_module *m, uint32_t addr, uint32_t r10, uint32_t r11)
{
    struct ros_cpu s;
    struct ros_handler h;
    uint32_t depth = ros_call_depth;
    ros_cpu_enter(&s);
    s.r[10] = r10;
    s.r[11] = r11;
    s.r[12] = m->private_word;
    if (!ROS_TRY(&h)) {
        ros_call_depth = depth;
        return (os_error *)h.error;
    }
    ros_call_depth = depth + 1;
    ros_call(&s, addr);
    ros_call_depth = depth;
    if (s.r[15] != ROS_RETURN_TO_NATIVE)
        ros_bad_return(&s, ROS_RETURN_TO_NATIVE);
    ros_handler_pop(&h);
    return s.v ? (os_error *)ros_ptr(s.r[0]) : NULL;
}

static unsigned count_incarnations(const struct ros_module *m)
{
    unsigned n = 0;
    for (uint32_t i = m->incarnations; i; i = inc_next(i))
        n++;
    return n;
}

/* Initialise one incarnation, env the tail (arena). */
static os_error *call_init(struct ros_module *m, uint32_t node, uint32_t env)
{
    /* An x32 module starting afresh, as its only incarnation, gets its
     * statics as they were placed. So *RMReInit, or a start after *RMKill,
     * begins where RISC OS's C module would. It does not begin where the
     * last run left the statics. A second incarnation shares the first's
     * statics, because x32 statics sit beside the code. */
    uint32_t others = m->incarnations;
    if (others == node)
        others = inc_next(node);
    if (m->pristine && !others)
        memcpy(ros_ptr(m->base), m->pristine, m->pristine_size);
    uint32_t keep = m->private_word;
    m->private_word = inc_word(node);
    struct ros_module *outer = initialising;
    if (m->twin)
        initialising = m;               /* a shadow: the scoping rules apply while it starts */
    os_error *e = NULL;
    if (m->init)
        e = m->init(m, ros_ptr(env));
    else if (m->init_addr)
        e = enter(m, m->init_addr, env, count_incarnations(m));
    initialising = outer;
    m->private_word = keep;
    return e;
}

/* Finalise one incarnation, fatally or not. A fatal death frees the
 * workspace the private word points at, if it is an RMA block. */
static os_error *call_final(struct ros_module *m, uint32_t node, int fatal)
{
    uint32_t keep = m->private_word;
    m->private_word = inc_word(node);
    os_error *e = NULL;
    if (m->final)
        e = m->final(m, fatal);
    else if (m->final_addr)
        e = enter(m, m->final_addr, (uint32_t)fatal, 0);
    m->private_word = keep;
    if (!e && fatal) {
        uint32_t ws = ros_ld32(inc_word(node));
        if (ws)
            ros_heap_free(ros_rma_heap(), ws);      /* if it is not a block, no matter */
        ros_st32(inc_word(node), 0);
    }
    return e;
}

/* The version in a help string, BCD with the point between the halves:
 * "Title<tab>1.23 (date)" is &00012300 (the kernel's GetVerNoFromHelpString). */
static uint32_t help_version(const char *help)
{
    unsigned col = 0;
    const char *p = help;
    for (; *p && col < 16; p++) {
        col++;
        if (*p == 9)
            col = (col + 7) & ~7u;
    }
    if (col < 16)
        return 0;
    for (; *p; p++) {
        if ((uint8_t)*p < 31 && *p != 9)
            return 0;
        if (*p >= '0' && *p <= '9')
            break;
    }
    uint32_t whole = 0, frac = 0;
    for (; *p >= '0' && *p <= '9'; p++)
        whole = whole << 4 | (uint32_t)(*p - '0');
    if (*p == '.')
        for (int shift = 12; shift >= 0 && p[1] >= '0' && p[1] <= '9'; shift -= 4, p++)
            frac |= (uint32_t)(p[1] - '0') << shift;
    return whole << 16 | (frac & 0xFFFF);
}

static void service_call(struct ros_cpu *s, int shadows_only);

static void post_service(struct ros_module *m, uint32_t node, uint32_t service)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = m->base;
    s.r[1] = service;
    s.r[2] = m->base ? m->base + ros_ld32(m->base + 0x10) : 0;
    s.r[3] = node == m->base_incarnation ? 0 : inc_postfix(node);
    s.r[4] = help_version(m->help);
    /* A shadow's own goes to shadows only. For native callers nothing
     * changes, and a native module watching for its dependency's restart
     * must not see a shadow's. */
    if (m->twin)
        service_call(&s, 1);
    else
        ros_service_call(&s);
}

/* ---- the chain --------------------------------------------------------------------------- */

/* A native module's header, in the RMA. It holds the title, help, SWI chunk
 * and names, so whatever reads a module by its header reads a native one
 * too. */
static uint32_t native_header_size(const struct ros_module *m)
{
    size_t title = strlen(m->title) + 1, help = strlen(m->help) + 1, names = 0;
    if (m->swi_prefix) {
        names = strlen(m->swi_prefix) + 2;
        for (uint32_t i = 0; i < m->swi_count; i++)
            names += strlen(m->swi_names[i]) + 1;
    }
    return (uint32_t)(56 + title + help + names);
}

static void native_header_in(struct ros_module *m, uint8_t *h)
{
    size_t title = strlen(m->title) + 1, help = strlen(m->help) + 1;
    memset(h, 0, 56);
    uint32_t off = 56;
    ros_st32(ros_addr(h) + 0x10, off);
    memcpy(h + off, m->title, title);
    off += (uint32_t)title;
    ros_st32(ros_addr(h) + 0x14, off);
    memcpy(h + off, m->help, help);
    off += (uint32_t)help;
    ros_st32(ros_addr(h) + 0x1C, m->swi_chunk);
    if (m->swi_prefix) {
        ros_st32(ros_addr(h) + 0x24, off);
        size_t n = strlen(m->swi_prefix) + 1;
        memcpy(h + off, m->swi_prefix, n);
        off += (uint32_t)n;
        for (uint32_t i = 0; i < m->swi_count; i++) {
            n = strlen(m->swi_names[i]) + 1;
            memcpy(h + off, m->swi_names[i], n);
            off += (uint32_t)n;
        }
        h[off] = 0;
    }
    ros_st32(ros_addr(h) + 0x30, 52);           /* the flags word: 32-bit */
    ros_st32(ros_addr(h) + 52, 1);
    m->base = ros_addr(h);
}

static void native_header(struct ros_module *m)
{
    uint8_t *h = ros_rma_alloc(native_header_size(m));
    if (h)
        native_header_in(m, h);
}

uint32_t ros_module_header_at(struct ros_module *m, uint32_t addr)
{
    native_header_in(m, ros_ptr(addr));
    return native_header_size(m);
}

static int in_chain(const struct ros_module *m)
{
    for (struct ros_module *x = chain; x; x = x->next)
        if (x == m)
            return 1;
    return 0;
}

static void link_module(struct ros_module *m)
{
    struct ros_module **p = &chain;
    while (*p)
        p = &(*p)->next;
    m->next = NULL;
    *p = m;
}

static void delink_module(struct ros_module *m)
{
    for (struct ros_module **p = &chain; *p; p = &(*p)->next)
        if (*p == m) {
            *p = m->next;
            m->next = NULL;
            return;
        }
}

static os_error *kill_incarnation(struct ros_module *m, uint32_t node, uint32_t prev);

/* Kill every incarnation of m. Keep going after an error, and return the
 * last one. */
static os_error *kill_all(struct ros_module *m)
{
    os_error *e = NULL;
    while (!e && m->incarnations)
        e = kill_incarnation(m, m->incarnations, 0);
    return e;
}

/* A module's last incarnation is gone. A shadow leaves its twin. A chain
 * module with a shadow gives the shadow its place in the chain. The title
 * has no native module now, so the ARM one serves every caller. The
 * exception is the built-in SharedCLibrary, which native programs cannot
 * use. It is killed with its twin. Any other module leaves the chain. */
static void out_of_chain(struct ros_module *m)
{
    if (m->twin) {
        if (m->twin->shadow == m)
            m->twin->shadow = NULL;
        m->twin = NULL;
        return;
    }
    struct ros_module *sh = m->shadow;
    if (!sh || !in_chain(m)) {
        delink_module(m);
        return;
    }
    m->shadow = NULL;
    sh->twin = NULL;
    if (policy_of(m) == ROS_SHADOW_BUILTIN) {
        delink_module(m);
        kill_all(sh);
        return;
    }
    for (struct ros_module **p = &chain; *p; p = &(*p)->next)   /* the twin's place */
        if (*p == m) {
            sh->next = m->next;
            *p = sh;
            m->next = NULL;
            return;
        }
}

/* A native module joining the chain while an ARM module holds its title.
 * If the title is shadowable, the ARM module is demoted to its shadow. It
 * leaves the chain, keeps its incarnations and workspace, and is neither
 * finalised nor initialised. If the title is never shadowable, the ARM
 * module is killed, as an RMLoad of the title always killed it. */
static void native_joins(struct ros_module *m)
{
    struct ros_module *c = chain;
    while (c && (c == m || !c->arm || !same_title(c->title, m->title)))
        c = c->next;
    if (!c)
        return;
    if (policy_of(m) == ROS_SHADOW_NEVER) {
        kill_all(c);
        return;
    }
    if (m->shadow)                      /* a title has one shadow at most */
        kill_all(m->shadow);
    delink_module(c);
    c->twin = m;
    m->shadow = c;
}

/* Add a new incarnation. Postfix and env are arena strings. It is
 * initialised, then put first in the list as the preferred one. A new
 * module goes into the chain. If its twin is set, it is linked as that
 * module's shadow instead. */
static os_error *add_incarnation(struct ros_module *m, uint32_t postfix, uint32_t env)
{
    uint32_t node = new_node(postfix);
    if (!node)
        return ros_error(ERR_MH_NO_ROOM,
                         "The area of memory reserved for relocatable modules is full");
    int first = !m->incarnations;
    if (first)
        m->base_incarnation = node;
    os_error *e = call_init(m, node, env);
    if (e) {
        ros_rma_free(ros_ptr(node));
        if (first)
            m->base_incarnation = 0;
        return e;
    }
    ros_st32(node, m->incarnations);
    m->incarnations = node;
    prefer_first(m);
    if (first && m->twin) {
        m->twin->shadow = m;
    } else if (first) {
        if (!m->arm) {
            m->prefer_arm = (uint8_t)is_preferred(m->title);
            native_joins(m);
        }
        link_module(m);
    }
    post_service(m, node, SERVICE_MODULE_POST_INIT);
    return NULL;
}

/* Unload a module loaded from a file that is now out of the chain. Its
 * image is freed and its code range forgotten. The descriptor itself stays,
 * because a caller may still hold it. It is inert and nothing finds it
 * again. */
static void unload(struct ros_module *m)
{
    if (!m->loaded)
        return;
    ros_capp_code_remove(m->base);
    ros_armrun_code_remove(m->base);
    ros_rma_free(m->loaded);
    m->loaded = NULL;
    free((void *)m->swi_names);
    m->swi_names = NULL;
    free(m->pristine);
    m->pristine = NULL;
    m->swi_count = 0;
    m->swi_chunk = 0;
    m->base = 0;
    m->title = m->help = "";
    m->init_addr = m->final_addr = m->service_addr = m->swi_addr = 0;
    m->command_table = 0;
}

/* Kill one incarnation (prev is the node before it, or 0). Killing the last
 * incarnation takes the module out of the chain. */
static os_error *kill_incarnation(struct ros_module *m, uint32_t node, uint32_t prev)
{
    os_error *e = call_final(m, node, 1);
    if (e)
        return e;
    if (prev)
        ros_st32(prev, inc_next(node));
    else
        m->incarnations = inc_next(node);
    prefer_first(m);
    post_service(m, node, SERVICE_MODULE_POST_FINAL);
    if (node == m->base_incarnation)
        m->base_incarnation = 0;
    ros_rma_free(ros_ptr(node));
    if (!m->incarnations) {
        out_of_chain(m);
        unload(m);
    }
    return NULL;
}

static uint32_t arena_string(const char *s)
{
    size_t n = strlen(s) + 1;
    uint8_t *b = ros_rma_alloc((uint32_t)n);
    if (!b)
        return 0;
    memcpy(b, s, n);
    return ros_addr(b);
}

void ros_module_register_rom(struct ros_module *m)
{
    if (rom_count < sizeof rom / sizeof rom[0]) {
        rom[rom_count++] = m;
        return;
    }
    /* Never silent. A module off the end would be missing from the
     * enumeration, dormant but unfindable. DeskMeter found this the day the
     * sixty-fifth module joined. */
    ros_console_printf("rosgd: module %s does not fit the ROM table\n",
                       m->title ? m->title : "?");
}

void ros_module_unregister_rom(struct ros_module *m)
{
    for (unsigned i = 0; i < rom_count; i++)
        if (rom[i] == m) {
            memmove(&rom[i], &rom[i + 1], (rom_count - i - 1) * sizeof rom[0]);
            rom_count--;
            return;
        }
}

os_error *ros_module_add(struct ros_module *m, const char *tail)
{
    if (!m->base)
        native_header(m);
    uint32_t env = arena_string(tail ? tail : ""), base = arena_string("Base");
    if (!env || !base)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    os_error *e = add_incarnation(m, base, env);
    ros_rma_free(ros_ptr(env));
    ros_rma_free(ros_ptr(base));
    return e;
}

os_error *ros_module_from_rom(struct ros_module *m, uint32_t base)
{
    memset(m, 0, sizeof *m);
    uint32_t init = ros_ld32(base + 0x04), final = ros_ld32(base + 0x08);
    uint32_t title = ros_ld32(base + 0x10), help = ros_ld32(base + 0x14);
    uint32_t chunk = ros_ld32(base + 0x1C), handler = ros_ld32(base + 0x20);
    uint32_t table = ros_ld32(base + 0x24), flags = ros_ld32(base + 0x30);
    uint32_t commands = ros_ld32(base + 0x18);

    /* Only 32-bit modules are accepted. The module's flags word says
     * whether it is one. */
    if (!flags || !(ros_ld32(base + flags) & 1))
        return ros_error(ROS_ERR_RM_HEADER, "Module at &%08X is not 32-bit compatible", base);
    if (handler && (!chunk || chunk & 0x3F || chunk & (ROS_X_BIT | 0xFF000000u)))
        return ros_error(ROS_ERR_RM_HEADER, "Illegal header field in module at &%08X", base);

    m->base = base;
    m->command_table = commands ? base + commands : 0;
    m->title = title ? (const char *)ros_ptr(base + title) : "";
    m->help = help ? (const char *)ros_ptr(base + help) : "";
    m->init_addr = init ? base + init : 0;
    final &= 0x7FFFFFFFu;               /* bit 31: cannot be RMCleared (modsqz sets it) */
    m->final_addr = final ? base + final : 0;
    uint32_t service = ros_ld32(base + 0x0C);
    m->service_addr = service ? base + service : 0;
    if (handler) {
        m->swi_chunk = chunk;
        m->swi_addr = base + handler;
    }
    if (table) {
        /* The decoding table: the prefix, then each SWI's name, then "". */
        const char *p = ros_ptr(base + table);
        m->swi_prefix = p;
        for (p += strlen(p) + 1; *p; p += strlen(p) + 1)
            m->swi_count++;
        const char **names = calloc(m->swi_count ? m->swi_count : 1, sizeof *names);
        if (!names)
            return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for module %s", m->title);
        p = m->swi_prefix;
        for (uint32_t i = 0; i < m->swi_count; i++) {
            p += strlen(p) + 1;
            names[i] = p;
        }
        m->swi_names = names;
    }
    return NULL;
}

/* ---- service calls ------------------------------------------------------------------ */

/* Which services a shadow is offered. Shadows are not in the chain. After
 * every chain module has had a service and none has claimed it, a second
 * pass offers it to each chain module's shadow, by the service's class:
 *   notify    delivered. A shadow's claim is ignored (R1 is put back), so
 *             every shadow sees it.
 *   provide   offered only when issued on behalf of an ARM caller
 *             (ros_caller_kind()). A shadow may claim it.
 *   withhold  never offered. This includes any service not listed here,
 *             until a row says otherwise.
 * The table below is the designed one. The services the box issues beyond
 * it are marked as such. */
enum { SVC_WITHHOLD, SVC_NOTIFY, SVC_PROVIDE };

static int service_class(const struct ros_cpu *s)
{
    switch (s->r[1]) {
    case 0x27: case 0x45:               /* Reset, PreReset */
    case 0x46: case 0x89:               /* ModeChange, ModeChanging */
    case 0x4E:                          /* MemoryMoved */
    case 0x90: case 0x91: case 0x92:    /* DynamicArea Create, Remove, Renumber */
    case 0xDA: case 0xDB:               /* ModulePostInit, PostFinal (a shadow's own: scoped) */
    case 0x59: case 0x5A: case 0x5E:    /* ResourceFSStarted, Dying; MessageFileClosed */
    case 0x60:                          /* ResourceFSStarting (its files go under: resourcefs.c) */
    case 0x75:                          /* TerritoryStarted */
    case 0x6E: case 0x5B: case 0x82:    /* FontsChanged, CalibrationChanged, InvalidateCache */
    case 0x85: case 0x5D:               /* WimpSpritesMoved, WimpPalette */
    case 0x7A: case 0x7B: case 0xA9:    /* ScreenBlanked, ScreenRestored, ScreenBlanking */
    case 0x72:                          /* SwitchingOutputToSprite */
    case 0x53:                          /* WimpCloseDown */
    case 0x2A:                          /* NewApplication: it may watch, never refuse */
    case 0x57:                          /* WimpReportError */
    case 0x400C0: case 0x400C1: case 0x400C2:   /* ErrorStarting, ButtonPressed, Ending */
    case 0x87: case 0x88: case 0x6F:    /* FilterManagerInstalled, Dying; BufferStarting */
    /* Beyond the designed table. */
    case 0x06:                          /* Error: one was raised, as information */
    case 0x81080:                       /* Territory's time zone changed: its clock follows */
    case 0x44EC5:                       /* WindowDeleted (ColourPicker's dialogues), as information */
        return SVC_NOTIFY;
    /* Withheld beyond the table. &64 TerritoryManagerLoaded and &93
     * ColourPickerLoaded ask modules to register with the one issuing them.
     * Those that answer (territories, colour models) are never shadowed.
     * &7C DesktopWelcome is claimed to replace the machine's welcome
     * screen. */
    case 0x43:                          /* International: Define, Keyboard and DefineUCS
                                           notify. The lookups (0-4, 8, 9) are the territory's. */
        return s->r[2] == 5 || s->r[2] == 6 || s->r[2] == 7 ? SVC_NOTIFY : SVC_WITHHOLD;
    case 0x04: case 0x07: case 0x08:    /* UKCommand, UKByte, UKWord */
    case 0x09: case 0x8C:               /* Help, SyntaxError */
    case 0x28: case 0x29:               /* UKConfig, UKStatus (the machine's CMOS: native) */
    case 0x6D:                          /* ValidateAddress */
        return SVC_PROVIDE;
    default:
        return SVC_WITHHOLD;
    }
}

/* Offer the service to every incarnation of a module, until one claims it.
 * With notify set, a claim is ignored: R1 is put back and the next
 * incarnation is offered the service. */
static void offer(struct ros_module *m, struct ros_cpu *s, int notify)
{
    uint32_t service = s->r[1];
    for (uint32_t node = m->incarnations, after; node && s->r[1]; node = after) {
        after = inc_next(node);
        if (m->service) {
            uint32_t keep = m->private_word;
            m->private_word = inc_word(node);
            m->service(m, s);
            m->private_word = keep;
        } else if (m->service_addr) {
            s->r[12] = inc_word(node);
            s->r[14] = ROS_RETURN_TO_NATIVE;
            ros_call(s, m->service_addr);
            if (s->r[15] != ROS_RETURN_TO_NATIVE)
                ros_bad_return(s, ROS_RETURN_TO_NATIVE);
        }
        if (notify)
            s->r[1] = service;
    }
}

/* The second pass offers the service to each chain module's shadow, by the
 * service's class. With shadows_only set, the service is a shadow's own
 * PostInit or PostFinal, which is class notify. */
static void offer_shadows(struct ros_cpu *s, int shadows_only)
{
    int class = shadows_only ? SVC_NOTIFY : service_class(s);
    if (class == SVC_WITHHOLD)
        return;
    /* provide: for an ARM caller, or to a preferred shadow (*ARMPrefer) */
    int arm = class != SVC_PROVIDE || ros_caller_kind() == ROS_KIND_ARM;
    for (struct ros_module *m = chain, *next; m && s->r[1]; m = next) {
        next = m->next;
        if (m->shadow && (arm || m->prefer_arm))
            offer(m->shadow, s, class == SVC_NOTIFY);
    }
}

/* Offer the service to every incarnation of every module in turn, until one
 * claims it. Then offer it to the shadows. The handlers run inside the OS,
 * as enter()'s code does. Their SWIs are not the outermost, even when the
 * runtime itself makes the call (the ROM starting up). So background work
 * and callbacks wait. Otherwise a callback that issues a service call would
 * come in over this one. */
static void service_call(struct ros_cpu *s, int shadows_only)
{
    uint32_t r12 = s->r[12], lr = s->r[14];
    uint32_t depth = ros_call_depth;
    ros_call_depth = depth + 1;
    if (!shadows_only)
        for (struct ros_module *m = chain, *next; m && s->r[1]; m = next) {
            next = m->next;             /* a module may go while it is called */
            offer(m, s, 0);
        }
    if (s->r[1])
        offer_shadows(s, shadows_only);
    ros_call_depth = depth;
    s->r[12] = r12;
    s->r[14] = lr;
}

void ros_service_call(struct ros_cpu *s)
{
    service_call(s, 0);
}

void ros_thunk_OS_ServiceCall(struct ros_cpu *s)
{
    uint32_t r10 = s->r[10], r11 = s->r[11];
    uint32_t outer_sp = ros_svc_sp_enter(s);
    ros_service_call(s);
    ros_svc_sp = outer_sp;
    s->r[10] = r10;
    s->r[11] = r11;
    s->v = 0;
}

/* ---- names ------------------------------------------------------------------------------ */

/* The kernel's Module_StrCmp. It compares s against title without case,
 * and s may be abbreviated with ".". The string s ends at a control
 * character, a space or term. It returns 1 and sets *end to where s ended
 * (its terminator, or after the "."). If they do not match it returns 0. */
static int str_cmp(uint32_t s, const char *title, uint32_t term, uint32_t *end)
{
    uint32_t n = 0;
    for (;; n++) {
        uint32_t a = ros_ld8(s + n), b = (uint8_t)title[n];
        if ((a == term || a <= ' ') && b <= ' ')
            break;
        if (upper(a) == upper(b))
            continue;
        if (n == 0 || a != '.' || b < ' ')
            return 0;
        n++;
        break;
    }
    if (n == 0)
        return 0;
    *end = s + n;
    return 1;
}

struct found {
    struct ros_module *m;
    uint32_t node, prev;            /* the incarnation named, or the preferred */
    int named;                      /* a postfix was given */
    uint32_t after;                 /* after the name */
};

/* How lookup routes the chain module it finds. The routing is one of the
 * following. It is as a caller of a given kind does it (ROS_KIND_*: an ARM
 * caller reaches a shadow, with the built-in SharedCLibrary made for it). Or
 * it is as an ARM caller does it without making one. Or there is no routing
 * and the chain's own module is returned. */
#define LOOKUP_ARM_EXISTING 2
#define LOOKUP_CHAIN        3

/* The kernel's LookUp_Module and lookup_commoned. This finds a module and
 * the incarnation that "Title%Postfix" names, routed as how says. */
static os_error *lookup_as(uint32_t name, struct found *f, int how)
{
    memset(f, 0, sizeof *f);
    for (struct ros_module *c = chain; c; c = c->next) {
        uint32_t end;
        if (!str_cmp(name, c->title, '%', &end))
            continue;
        struct ros_module *m = how == LOOKUP_CHAIN ? c
                             : how == LOOKUP_ARM_EXISTING ? route(c, ROS_KIND_ARM, 0)
                             : route(c, how, 1);
        f->m = m;
        f->node = m->incarnations;
        f->after = end;
        if (ros_ld8(end) != '%')
            return NULL;
        uint32_t postfix = end + 1;
        if (ros_ld8(postfix) <= ' ')
            break;                      /* "Title%": no such */
        f->named = 1;
        for (uint32_t node = m->incarnations, prev = 0; node; prev = node, node = inc_next(node)) {
            uint32_t pend;
            if (str_cmp(postfix, ros_ptr(inc_postfix(node)), 0, &pend)) {
                f->node = node, f->prev = prev, f->after = pend;
                return NULL;
            }
        }
        return ros_error(ERR_INC_NOT_FOUND, "Incarnation not found");
    }
    /* The name as given, to a control character, is what MessageTrans puts
     * for %0 in the kernel's error. *RMEnsure's version comes with it. */
    char text[64];
    unsigned n = 0;
    for (uint32_t c; n < sizeof text - 1 && (c = ros_ld8(name + n)) >= ' '; n++)
        text[n] = (char)c;
    text[n] = 0;
    return ros_error(ERR_RM_NOT_FOUND, "Module %s not found", text);
}

/* The chain's own module of that name */
static os_error *lookup(uint32_t name, struct found *f)
{
    return lookup_as(name, f, LOOKUP_CHAIN);
}

/* The command tail after a module's name: past it, and past spaces. */
static uint32_t skip_name(uint32_t p)
{
    while (ros_ld8(p) > ' ')
        p++;
    while (ros_ld8(p) == ' ')
        p++;
    return p;
}

static struct ros_module *rom_named(uint32_t name, uint32_t *end)
{
    for (unsigned i = 0; i < rom_count; i++)
        if (str_cmp(name, rom[i]->title, '%', end))
            return rom[i];
    return NULL;
}

/* A ROM module not running, started: "Title" or "Title%Postfix". */
static os_error *start_from_rom(uint32_t name, int need_postfix)
{
    uint32_t end;
    struct ros_module *m = rom_named(name, &end);
    if (!m || in_chain(m)) {
        char text[64];
        unsigned n = 0;
        for (uint32_t c; n < sizeof text - 1 && (c = ros_ld8(name + n)) > ' '; n++)
            text[n] = (char)c;
        text[n] = 0;
        return ros_error(ERR_RM_NOT_FOUND, "Module %s not found", text);
    }
    uint32_t postfix = 0;
    if (ros_ld8(end) == '%' && ros_ld8(end + 1) > ' ')
        postfix = end + 1;
    else if (need_postfix)
        return ros_error(ERR_POSTFIX_NEEDED, "Postfix not specified");
    uint32_t base = postfix ? 0 : arena_string("Base");
    os_error *e = add_incarnation(m, postfix ? postfix : base, skip_name(name));
    if (base)
        ros_rma_free(ros_ptr(base));
    return e;
}

/* ---- the RMA, claimed as the kernel claims ------------------------------------------ */

static os_error *no_room(void)
{
    return ros_error(ERR_MH_NO_ROOM,
                     "The area of memory reserved for relocatable modules is full");
}

/* RMAClaim_Chunk: 32n bytes, header included. With no room, the RMA is
 * grown by what is missing and the claim is tried again (rma.c). */
static os_error *rma_claim(uint32_t size, uint32_t *addr)
{
    size = ((size + 31 + 4) & ~31u) - 4;    /* 32n bytes, header included */
    if (!ros_heap_get(ros_rma_heap(), size, addr))
        return NULL;
    return ros_rma_grow(size) || ros_heap_get(ros_rma_heap(), size, addr) ? no_room() : NULL;
}

static os_error *rma_claim_aligned(uint32_t size, uint32_t align, uint32_t *addr)
{
    if (!align || (align & (align - 1)))
        return ros_error(ERR_RM_ALIGNMENT, "Bad alignment request");
    if (align < 32)
        align = 32;
    size = ((size + 15) & ~31u) + 16;
    uint32_t hpd = ros_rma_heap();
    if (!ros_heap_get_aligned(hpd, size, align, 0, hpd, addr))
        return NULL;
    /* The worst case is the size plus the alignment. */
    return ros_rma_grow(size + align) || ros_heap_get_aligned(hpd, size, align, 0, hpd, addr)
               ? no_room() : NULL;
}

/* OS_Module 13: an aligned block always moves when it grows, keeping the
 * next claim's alignment, as the kernel does. */
static os_error *rma_extend(uint32_t *addr, int32_t by)
{
    int32_t d = (int32_t)(((uint32_t)by + 31) & ~31u);
    uint32_t hpd = ros_rma_heap();
    if (d < 0)
        return ros_heap_extend_block(hpd, addr, d);
    if (*addr & 31) {
        os_error *e = ros_heap_extend_block(hpd, addr, d);
        if (!e || ros_rma_grow(ros_ld32(*addr - 4) + (uint32_t)d))  /* the most it may need */
            return e;
        return ros_heap_extend_block(hpd, addr, d);
    }
    uint32_t size = ros_ld32(*addr - 4) - 4, fresh;
    os_error *e = rma_claim(size + (uint32_t)d, &fresh);
    if (e)
        return e;
    memmove(ros_ptr(fresh), ros_ptr(*addr), size);
    ros_heap_free(hpd, *addr);
    *addr = fresh;
    return NULL;
}

os_error *xos_module_claim(uint32_t size, void **block)
{
    uint32_t a;
    os_error *e = rma_claim(size, &a);
    *block = e ? NULL : ros_ptr(a);
    return e;
}

os_error *xos_module_free(void *block)
{
    return ros_heap_free(ros_rma_heap(), ros_addr(block));
}

/* ---- loading from a file (the kernel's Load_Module) --------------------------------- */

static os_error *file_swi(uint32_t r[6])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 6 * sizeof r[0]);
    ros_swi(&c, XOS_File);
    memcpy(r, c.r, 6 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* ---- squeezed modules (modsqz) ---------------------------------------------------------
 *
 * The kernel's CheckForSqueezedModule (Kernel/s/UnSqueeze, RCC 1987),
 * translated. A module that modsqz has squeezed has bit 31 of its
 * initialise offset set. The offset without that bit is the squeezed
 * image's size, and its last five words are
 *
 *     decoded size, encoded size, tables size, shorts, longs
 *
 * Below them are the two tables, encoded. "Shorts" are whole words, and
 * "longs" are the top 24 bits of words. Each entry is the one before it
 * (-1 at first) plus a delta. A byte says how the delta is given:
 *
 *     0         a literal delta follows, LSB first: 4 bytes in the shorts
 *               table, 3 in the longs
 *     1..9      that many entries, each the last plus 1
 *     10..91    the delta is byte - 10
 *     92..173   ((byte - 92) << 8) + the next byte
 *     174..255  ((byte - 174) << 16) + the next byte + (the one after << 8)
 *
 * Below the tables is the encoded image (the first part of the file,
 * header included). It is read down from its top. Each byte holds two
 * nibbles and makes two words. The words are stored down from the decoded
 * image's top, with the low nibble's word below the high one's. A nibble
 * means:
 *
 *     0         a zero word
 *     1         a literal: the next 4 bytes down, the first the low byte
 *     2..8      longs[(nibble - 2) << 8 | next byte] << 8 | the byte below
 *     9..15     shorts[(nibble - 9) << 8 | next byte]
 *
 * The decoded image is the module as it was, but its finalise offset may
 * keep bit 31, which modsqz sets to mean "cannot be RMCleared". The kernel
 * leaves it there, and so does this (ROOL's unmodsqz clears it). The
 * self-test's case 14 checks 5.30's ZLib against unmodsqz's output on
 * RISC OS 5.30, apart from that bit (boot/selftest_shadow.c). */

enum { SQZ_NIBS_LONG = 7, SQZ_MIN_SHORT = 2 + SQZ_NIBS_LONG, SQZ_MIN_LONG = 2 };

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t w)
{
    p[0] = (uint8_t)w, p[1] = (uint8_t)(w >> 8), p[2] = (uint8_t)(w >> 16), p[3] = (uint8_t)(w >> 24);
}

/* Read one table of n entries from *p (below end) into t. Returns 0 if the
 * input runs out. */
static int sqz_table(const uint8_t **p, const uint8_t *end, uint32_t *t, uint32_t n, int shorts)
{
    uint32_t last = 0xFFFFFFFFu, i = 0;
    const uint8_t *q = *p;
#define NEED(k) do { if ((size_t)(end - q) < (size_t)(k)) return 0; } while (0)
    while (i < n) {
        NEED(1);
        uint32_t b = *q++, d;
        if (b == 0) {
            NEED(shorts ? 4 : 3);
            d = q[0] | q[1] << 8 | q[2] << 16;
            if (shorts)
                d |= (uint32_t)q[3] << 24;
            q += shorts ? 4 : 3;
        } else if (b < 10) {
            if (b > n - i)
                return 0;
            while (b--)
                t[i++] = ++last;
            continue;
        } else if (b < 92) {
            d = b - 10;
        } else if (b < 174) {
            NEED(1);
            d = (b - 92) << 8 | q[0];
            q += 1;
        } else {
            NEED(2);
            d = (b - 174) << 16 | q[0] | q[1] << 8;
            q += 2;
        }
        t[i++] = last += d;
    }
#undef NEED
    *p = q;
    return 1;
}

/* Make one word for a nibble, reading down from *p (not below lo). Returns 0
 * if the input runs out. */
static int sqz_word(unsigned nib, const uint8_t **p, const uint8_t *lo, const uint32_t *shorts,
                    uint32_t ns, const uint32_t *longs, uint32_t nl, uint32_t *w)
{
    const uint8_t *q = *p;
    if (nib >= SQZ_MIN_SHORT) {
        if (q - lo < 1)
            return 0;
        uint32_t i = (nib - SQZ_MIN_SHORT) << 8 | *--q;
        if (i >= ns)
            return 0;
        *w = shorts[i];
    } else if (nib >= SQZ_MIN_LONG) {
        if (q - lo < 2)
            return 0;
        uint32_t i = (nib - SQZ_MIN_LONG) << 8 | *--q;
        if (i >= nl)
            return 0;
        *w = longs[i] << 8 | *--q;
    } else if (nib == 0) {
        *w = 0;
    } else {
        if (q - lo < 4)
            return 0;
        q -= 4;
        *w = q[3] | q[2] << 8 | q[1] << 16 | (uint32_t)q[0] << 24;
    }
    *p = q;
    return 1;
}

/* (module.h) */
os_error *ros_module_unsqueeze(uint8_t **image, uint32_t *size)
{
    const uint8_t *in = *image;
    if (*size < 0x20 + 20 || !(rd32(in + 4) & 0x80000000u))
        return NULL;
    uint32_t end = rd32(in + 4) & 0x7FFFFFFFu;
    if (end > *size || end < 20 + 8)
        return ros_error(ROS_ERR_RM_HEADER, "Squeezed module is damaged");
    uint32_t dec = rd32(in + end - 20), enc = rd32(in + end - 16), tabs = rd32(in + end - 12);
    uint32_t ns = rd32(in + end - 8), nl = rd32(in + end - 4);
    if ((uint64_t)enc + tabs > end - 20 || dec & 7 || ns > 7 << 8 || nl > 7 << 8 || !dec)
        return ros_error(ROS_ERR_RM_HEADER, "Squeezed module is damaged");
    const uint8_t *top = in + end - 20 - tabs, *lo = top - enc;   /* the encoded image */
    uint32_t *shorts = malloc((ns + nl + 1) * sizeof *shorts);
    uint8_t *out = shorts ? ros_rma_alloc(dec) : NULL;
    if (!out) {
        free(shorts);
        return no_room();
    }
    uint32_t *longs = shorts + ns;
    const uint8_t *p = top;
    int ok = sqz_table(&p, in + end - 20, shorts, ns, 1) && sqz_table(&p, in + end - 20, longs, nl, 0);
    uint8_t *o = out + dec;
    for (p = top; ok && p > lo;) {
        unsigned b = *--p;
        uint32_t w0, w1;
        ok = o - out >= 8 && sqz_word(b & 15, &p, lo, shorts, ns, longs, nl, &w0) &&
             sqz_word(b >> 4, &p, lo, shorts, ns, longs, nl, &w1);
        if (ok) {
            o -= 8;
            wr32(o, w0), wr32(o + 4, w1);
        }
    }
    free(shorts);
    if (!ok || o != out) {              /* the kernel would not notice a short image */
        ros_rma_free(out);
        return ros_error(ROS_ERR_RM_HEADER, "Squeezed module is damaged");
    }
    ros_rma_free(*image);
    *image = out;
    *size = dec;
    return NULL;
}

static os_error *place_module(uint8_t *image, uint32_t size, const char *what, uint32_t tail,
                              struct ros_module **out);

/* The first half of OS_Module 0 and 1. The file R1 names, up to a space, is
 * read, placed and started. The rest of the string is the tail that its
 * initialise entry gets. A module of the same title is killed first. A
 * postfix in the new module's title is refused. *out is set to the module
 * and *env to the tail. */
static os_error *load_module(uint32_t name, struct ros_module **out, uint32_t *env)
{
    uint32_t n = 0;
    while (ros_ld8(name + n) > ' ')
        n++;
    char *file = ros_rma_alloc(n + 1);
    if (!file)
        return no_room();
    memcpy(file, ros_ptr(name), n);
    file[n] = 0;
    uint32_t r[6] = { 5, ros_addr(file) };              /* OSFile_ReadInfo */
    os_error *e = file_swi(r);
    uint32_t size = r[4];
    if (!e && r[0] != 1) {                              /* not a file: FileSwitch's error */
        uint32_t m[6] = { 19, ros_addr(file), r[0] };
        e = file_swi(m);
    } else if (!e && (r[2] & 0xFFFFFF00u) != 0xFFFFFA00u) {
        e = ros_error(ERR_NOT_MOD, "This is not a relocatable module");
    }
    uint8_t *image = NULL;
    if (!e && !(image = ros_rma_alloc(size ? size : 4)))
        e = no_room();
    if (!e) {
        uint32_t l[6] = { 255, ros_addr(file), ros_addr(image), 0 };   /* load it there */
        e = file_swi(l);
    }
    *env = skip_name(name);
    if (!e)
        e = place_module(image, size, file, *env, out);
    else if (image)
        ros_rma_free(image);
    ros_rma_free(file);
    return e;
}

/* The second half of OS_Module 0, 1, 10 and 11. The image is an RMA block
 * of size bytes. It is taken, and either kept as the module's or freed. It
 * is placed and started, as load_module says. The string what names it in
 * errors, and tail is its initialise entry's R10. *out is NULL when an ARM
 * module is absorbed. */
static os_error *place_module(uint8_t *image, uint32_t size, const char *what, uint32_t tail,
                              struct ros_module **out)
{
    os_error *e = NULL;
    if (size < 0x34) {                  /* not even a header */
        ros_rma_free(image);
        return ros_error(ERR_NOT_MOD, "This is not a relocatable module");
    }
    /* The image is one of three things. It may be the ELF32 that roscc link
     * --module writes, for this machine. Or it may be a RISC OS module of
     * ARM code, which the ARM container runs where it was loaded. Its
     * entries, through ros_call, reach the engine. Or it may be one of
     * RISC OS 5.30's ROM modules, wrapped with the list of words to move. */
    static const uint16_t machine = ROS_CAPP_A64 ? 183 : 62;   /* EM_AARCH64, EM_X86_64 */
    int arm = (size < 52 || memcmp(image, "\x7F" "ELF", 4) || image[4] != 1);
    if (!e && !arm && (uint16_t)(image[18] | image[19] << 8) != machine)
        e = ros_error(ROS_ERR_RM_HEADER, "'%s' is a module for another machine", what);
    uint32_t base = 0, placed = 0;
    void *block = NULL;
    if (!e && arm && size >= 20 && !memcmp(image, "ROSGDROM", 8)) {
        /* RISC OS 5.30's own ROM module, wrapped by tools/romwrap.py. It is
         * moved here from the address it is linked at, and its listed
         * address words are moved by as much. */
        uint32_t link, msize, n, nl, nb, clib = 0;
        memcpy(&link, image + 8, 4), memcpy(&msize, image + 12, 4), memcpy(&n, image + 16, 4);
        memcpy(&nl, image + 20, 4), memcpy(&nb, image + 24, 4);
        uint64_t head = 28 + 4 * (uint64_t)n + 8 * ((uint64_t)nl + nb);
        uint8_t *mod = NULL;
        if (head + msize > size)
            e = ros_error(ERR_NOT_MOD, "This is not a relocatable module");
        else if ((nl || nb) && !(clib = ros_armrun_clib()))
            e = ros_error(ERR_NOT_MOD, "SharedCLibrary for ARM modules has not started");
        else if (!(mod = ros_rma_alloc(msize ? msize : 4)))
            e = no_room();
        if (!e) {
            memcpy(mod, image + head, msize);
            uint32_t delta = ros_addr(mod) - link;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t off, w;
                memcpy(&off, image + 28 + 4 * k, 4);
                if (off <= msize && msize - off >= 4) {
                    memcpy(&w, mod + off, 4);
                    w += delta;
                    memcpy(mod + off, &w, 4);
                }
            }
            const uint8_t *pairs = image + 28 + 4 * n;
            for (uint32_t k = 0; k < nl + nb; k++) {   /* the words that point into the emulated SharedCLibrary */
                uint32_t off, to;
                memcpy(&off, pairs + 8 * k, 4), memcpy(&to, pairs + 8 * k + 4, 4);
                if (off > msize || msize - off < 4)
                    continue;
                uint32_t w = clib + to;                 /* an address word */
                if (k >= nl) {                          /* a B or BL, with its condition kept, aimed here */
                    uint32_t op;
                    memcpy(&op, mod + off, 4);
                    int32_t d = (int32_t)(clib + to - (ros_addr(mod) + off + 8)) >> 2;
                    w = (op & 0xFF000000u) | ((uint32_t)d & 0xFFFFFFu);
                }
                memcpy(mod + off, &w, 4);
            }
            ros_rma_free(image);
            image = mod;
            size = msize;
        }
    }
    /* Unsqueeze a squeezed module (modsqz), as the kernel does before it
     * initialises one (#162). */
    if (!e && arm)
        e = ros_module_unsqueeze(&image, &size);
    if (!e && arm) {
        base = ros_addr(image);
        block = image;
        image = NULL;
        if (!ros_armrun_code_add(base, base + size))
            e = no_room();
    } else if (!e) {
        e = ros_module_image_load(image, size, what, &base, &block, &placed);
    }
    if (image)
        ros_rma_free(image);
    struct ros_module *m = NULL;
    if (!e && !(m = calloc(1, sizeof *m)))
        e = no_room();
    if (!e)
        e = ros_module_from_rom(m, base);
    if (!e && !arm)
        ros_module_keep_pristine(m, placed);
    if (m) {
        m->arm = (uint8_t)arm;
        m->image_end = arm ? base + size : 0;
    }

    /* The same title may already be in the chain. An ARM module over a
     * native one becomes its shadow, and the old shadow is killed. If the
     * title is never shadowed, the ARM module is absorbed instead. Any
     * other module of the title is killed, with every incarnation. A native
     * module's shadow is then promoted, and the new native module demotes
     * it again as it joins (native_joins). */
    struct found f;
    int absorbed = 0;
    if (!e && !lookup(base + ros_ld32(base + 0x10), &f)) {
        if (strchr(m->title, '%'))
            e = ros_error(ERR_MODULE_POSTFIX, "'%%' in module title");
        else if (arm && !f.m->arm && policy_of(f.m) == ROS_SHADOW_NEVER)
            absorbed = 1;
        else if (arm && !f.m->arm) {
            m->twin = f.m;
            if (f.m->shadow)
                e = kill_all(f.m->shadow);
        } else if (arm || !f.m->arm)
            e = kill_all(f.m);
    }
    if (absorbed) {
        ros_console_printf("rosgd: ARM %s not loaded: the box's serves ARM code\n", m->title);
        ros_armrun_code_remove(base);
        ros_rma_free(block);
        free((void *)m->swi_names);
        free(m);
        *out = NULL;
        return NULL;
    }
    if (!e) {
        uint32_t base_name = arena_string("Base");
        e = base_name ? add_incarnation(m, base_name, tail) : no_room();
        if (base_name)
            ros_rma_free(ros_ptr(base_name));
    }
    if (!e)
        m->loaded = block;
    if (e) {
        if (block) {                    /* it was never linked, so the image goes */
            ros_capp_code_remove(base);
            ros_armrun_code_remove(base);
            ros_rma_free(block);
        }
        if (m) {
            free((void *)m->swi_names);
            free(m->pristine);
            free(m);
        }
        return e;
    }
    *out = m;
    return NULL;
}

/* Make a shadow from an ARM module already in the RMA (the built-in
 * SharedCLibrary, runtime/armrun/box.c). Its descriptor is made from its
 * header, its base incarnation is initialised, and it is linked as twin's
 * shadow. */
os_error *ros_module_make_shadow(struct ros_module *twin, uint32_t base, uint32_t size,
                                 void *block, struct ros_module **out)
{
    struct ros_module *m = calloc(1, sizeof *m);
    if (!m)
        return no_room();
    os_error *e = ros_module_from_rom(m, base);
    uint32_t env = e ? 0 : arena_string(""), base_name = e ? 0 : arena_string("Base");
    if (!e && (!env || !base_name))
        e = no_room();
    if (!e) {
        m->arm = 1;
        m->twin = twin;
        m->image_end = base + size;
        e = add_incarnation(m, base_name, env);
    }
    if (env)
        ros_rma_free(ros_ptr(env));
    if (base_name)
        ros_rma_free(ros_ptr(base_name));
    if (e) {
        free((void *)m->swi_names);
        free(m);
        return e;
    }
    m->loaded = block;
    *out = m;
    return NULL;
}

/* ---- OS_Module ------------------------------------------------------------------------- */

static os_error *cannot(const char *what)
{
    return ros_error(ROS_ERR_UNIMPLEMENTED, "%s: ROSGD runs no ARM code", what);
}

/* Where an entered application's exit lands, and that frame. These belong
 * to the thread, as each task is a thread. A Wimp task entering BASIC must
 * not take the first program's place. */
static _Thread_local jmp_buf app_return;
static _Thread_local int app_return_set;
static _Thread_local const void *app_frame;

/* The start entry, run as the application (ros_module_run_as_application).
 * It runs in user mode with R0 the command tail, R12 -> the private word,
 * and the SVC stack flat. A native module's start entry runs instead in C,
 * in the same place. */
struct start_entry {
    struct ros_module *m;
    uint32_t entry, tail;
};

static void run_start_entry(void *arg)
{
    const struct start_entry *se = arg;
    if (se->m->start) {
        se->m->start(se->m, se->tail);          /* a native module's */
        return;
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = se->tail;
    c.r[12] = inc_word(se->m->incarnations);
    c.mode = ROS_MODE_USR;
    ros_call(&c, se->entry);
}

/* OS_Module 2, Enter (the kernel's Module_Enter and EnterIt): the module
 * becomes the application. The incarnation named is made the preferred
 * one. A module with no start entry just returns. FileSwitch's
 * StartApplication (OS_FSControl 2) announces the new application, which
 * anything may refuse, and sets the command line and the CAO. Then the
 * stacks are flattened, and the start entry runs in user mode with R0 the
 * command tail and R12 -> the private word. A native module's start entry
 * runs instead in C, in the same place. This does not return to the
 * caller. A program ends with OS_Exit, and one that returns has exited. */
static os_error *enter_module(struct found *f, uint32_t tail)
{
    if (f->named && f->prev) {
        ros_st32(f->prev, inc_next(f->node));
        ros_st32(f->node, f->m->incarnations);
        f->m->incarnations = f->node;
        prefer_first(f->m);
    }
    uint32_t base = f->m->base, start = base ? ros_ld32(base) : 0;
    if (!start && !f->m->start)
        return NULL;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 2, c.r[1] = tail, c.r[2] = base, c.r[3] = base + ros_ld32(base + 0x10);
    ros_swi(&c, XOS_FSControl);
    if (c.v)
        return ros_ptr(c.r[0]);
    /* The language runs as the task's application. Give the task a slot
     * (*WimpSlot sized it) if it has none. */
    os_error *slot = ros_task_give_slot();
    if (slot)
        return slot;
    /* A start word with a condition field is an instruction (B startit).
     * It is run where it stands. */
    struct start_entry se = { f->m, (start & 0xF0000000u) ? base : base + start, tail };
    ros_module_run_as_application(run_start_entry, &se);
    return NULL;
}

/* Run code as the application. This is used for a module's start entry
 * (enter_module) and for a C application (capp.c). The code runs with these
 * conditions:
 *   - The SVC stack is flat.
 *   - Its SWIs are the outermost, as the kernel's stack reset leaves the
 *     SWIs that entered it. Their exits run callbacks, and its errors reach
 *     its handler with its R10-R12.
 *   - Its errors unwind within its own frames (environment.h).
 *   - Its end (OS_Exit, or the exit handler's end_program) comes back here,
 *     as the kernel's EnterIt comes back to the command that entered it.
 *     The environment and the SVC stack pointer are as they were.
 * An application that it enters in turn returns here, and then it goes on.
 * So do the alias expansions it was started under, which keep their lines
 * as it starts (oscli.c). Each start nests, so the room on the stacks is
 * asked for first. FileSwitch's StartApplication, which every entry calls
 * before this, refuses a start without that room (task.h). */
void ros_module_run_as_application(void (*run)(void *arg), void *arg)
{
    ros_oscli_application_starts();
    /* The application space is the new application's. Code that an earlier
     * application's assembler wrote there is not there any more. */
    ros_capp_code_forget_assembled(ROS_APP_BASE, ROS_RMA_BASE);
    uint32_t svc = ros_svc_sp;
    ros_svc_sp = ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE;
    struct ros_environment saved;
    ros_env_save(&saved);
    uint32_t depth = ros_call_depth;
    ros_call_depth = 0;
    struct ros_env_base outer_base = { .active = 0 };
    ros_env_base_swap(&outer_base);
    jmp_buf outer_return;
    memcpy(outer_return, app_return, sizeof app_return);
    int outer_set = app_return_set;
    const void *outer_frame = app_frame;
    struct ros_swi_frame *trace = ros_switrace_top();
    ros_obey_catch_exit();              /* restored with the environment */
    ros_task_app_enter();
    if (setjmp(app_return) == 0) {
        app_return_set = 1;
        app_frame = __builtin_frame_address(0);
        run(arg);
    }
    ros_task_app_leave();
    ros_switrace_unwind(trace);         /* the SWIs its exit left have ended */
    memcpy(app_return, outer_return, sizeof app_return);
    app_return_set = outer_set;
    app_frame = outer_frame;
    ros_env_base_swap(&outer_base);
    ros_call_depth = depth;
    ros_svc_sp = svc;
    ros_env_load(&saved);
    /* At the desktop's end, the space the Wimp put back is the command
     * line's now, all of it (task.h). */
    uint32_t took = ros_task_took_ended_space();
    if (took)
        ros_env_set_memory_limit(ROS_APP_BASE + took);
}

void ros_module_app_exit(void)
{
    if (app_return_set) {
        app_return_set = 0;
        ros_resume_unwind(app_frame);           /* unwind the frames it leaves */
        longjmp(app_return, 1);
    }
}

static int module_number(const struct ros_module *m)
{
    int n = 0;
    for (struct ros_module *x = chain; x && x != m; x = x->next)
        n++;
    return n;
}

/* OS_Module 12 and 22: module R1, incarnation R2, then the next. An ARM
 * caller sees each shadow in its twin's place. A title appears once and the
 * numbers are unmoved. */
static os_error *get_names(struct ros_cpu *s, int kind)
{
    struct ros_module *m = chain;
    for (uint32_t i = s->r[1]; m && i; i--)
        m = m->next;
    if (!m)
        return ros_error(ERR_NO_MORE_MODULES, "No more modules");
    m = route(m, kind, 0);
    uint32_t node = m->incarnations;
    for (uint32_t i = s->r[2]; node && i; i--)
        node = inc_next(node);
    s->r[3] = m->base;
    if (!node) {
        if (s->r[2] != 0)
            return ros_error(ERR_NO_MORE_INCS, "No more incarnations of that module");
        s->r[4] = s->r[0] == 12 ? 0xDEADDEADu : 0;
        s->r[1]++;
        return NULL;
    }
    s->r[4] = s->r[0] == 12 ? ros_ld32(inc_word(node)) : inc_word(node);
    s->r[5] = inc_postfix(node);
    if (inc_next(node)) {
        s->r[2]++;
    } else {
        s->r[2] = 0;
        s->r[1]++;
    }
    return NULL;
}

/* A module that RMClear leaves alone. This is an ARM module whose finalise
 * offset has bit 31 set (the kernel's MHC_StepOn), as modsqz sets it on a
 * squeezed module. */
static int invincible(const struct ros_module *m)
{
    return m->arm && m->base && ros_ld32(m->base + 8) & 0x80000000u;
}

/* OS_Module, for a caller of a given kind. An ARM caller's lookups (2, 3,
 * 18), enumeration (12, 22) and kill (4) reach shadows. */
static os_error *module_op(struct ros_cpu *s, int kind)
{
    struct found f;
    os_error *e;
    uint32_t hpd = ros_rma_heap();
    switch (s->r[0]) {
    case 0: {                           /* Run: loaded, then entered */
        struct ros_module *m;
        uint32_t env;
        if ((e = load_module(s->r[1], &m, &env)))
            return e;
        if (!m)
            return NULL;                /* absorbed: nothing loaded to enter */
        f = (struct found){ .m = m, .node = m->incarnations };
        return enter_module(&f, env);
    }
    case 1: {                           /* Load */
        struct ros_module *m;
        uint32_t env;
        return load_module(s->r[1], &m, &env);
    }
    case 2:                             /* Enter, as the application */
        if ((e = lookup_as(s->r[1], &f, kind)))
            return e;
        return enter_module(&f, s->r[2]);

    case 3:                             /* ReInit: finalise, then initialise again */
        if ((e = lookup_as(s->r[1], &f, kind)))
            return e->errnum == ERR_RM_NOT_FOUND ? start_from_rom(s->r[1], 0) : e;
        /* This is a native caller's ReInit of a title that an ARM module
         * holds, while the ROM's module of that title is dormant. The ARM
         * module is either a promoted shadow, or one loaded after the
         * native module was killed. The ROM module is started. That
         * demotes the ARM one to its shadow again, workspace kept
         * (native_joins). */
        if (kind != ROS_KIND_ARM && f.m->arm && !f.named) {
            uint32_t end;
            struct ros_module *r = rom_named(s->r[1], &end);
            if (r && !in_chain(r))
                return start_from_rom(s->r[1], 0);
        }
        if ((e = call_final(f.m, f.node, 1)))
            return e;
        post_service(f.m, f.node, SERVICE_MODULE_POST_FINAL);
        if ((e = call_init(f.m, f.node, skip_name(s->r[1])))) {
            if (f.prev)                 /* it failed, so the incarnation goes */
                ros_st32(f.prev, inc_next(f.node));
            else
                f.m->incarnations = inc_next(f.node);
            prefer_first(f.m);
            ros_rma_free(ros_ptr(f.node));
            if (!f.m->incarnations)
                out_of_chain(f.m);
            return e;
        }
        post_service(f.m, f.node, SERVICE_MODULE_POST_INIT);
        return NULL;

    case 4:                             /* Delete */
        /* An ARM caller kills a shadow, or an ARM module in the chain. It
         * never kills the box's native module from under native callers.
         * In that case nothing happens, quietly. */
        if (kind == ROS_KIND_ARM) {
            if ((e = lookup(s->r[1], &f)))
                return e;
            if (!f.m->shadow && !f.m->arm) {
                ros_console_printf("rosgd: ARM RMKill of %s ignored: the box's module serves "
                                   "native code\n", f.m->title);
                return NULL;
            }
            if (f.m->shadow && (e = lookup_as(s->r[1], &f, LOOKUP_ARM_EXISTING)))
                return e;
        } else if ((e = lookup(s->r[1], &f))) {
            return e;
        }
        return kill_incarnation(f.m, f.node, f.prev);

    case 5:
        return ros_heap_describe(hpd, &s->r[2], &s->r[3]);
    case 6:
        return rma_claim(s->r[3], &s->r[2]);
    case 7:
        return ros_heap_free(hpd, s->r[2]);
    case 8:                             /* Tidy: nothing to do, as on Medusa */
        return NULL;

    case 9: {                           /* Clear: every module not in ROM, last first */
        /* This depends on the kind of caller. A native caller clears every
         * module not in ROM, shadows included. An ARM caller clears the ARM
         * ones only, which are shadows and ARM-only modules. Shadows go
         * first, so none is promoted. The built-in SharedCLibrary stays, as
         * a ROM module does, because the ARM C program asking may be
         * running on it. */
        os_error *last = NULL;
        for (struct ros_module *m = chain; m; m = m->next)
            if (m->shadow && policy_of(m) != ROS_SHADOW_BUILTIN && !invincible(m->shadow) &&
                (e = kill_all(m->shadow)))
                last = e;
        for (;;) {
            struct ros_module *victim = NULL;
            for (struct ros_module *m = chain; m; m = m->next) {
                int is_rom = 0;
                for (unsigned i = 0; i < rom_count; i++)
                    is_rom |= rom[i] == m;
                if (!is_rom && !m->cleared && (kind != ROS_KIND_ARM || m->arm) && !invincible(m))
                    victim = m;
            }
            if (!victim)
                break;
            victim->cleared = 1;
            if (victim->shadow && policy_of(victim) != ROS_SHADOW_BUILTIN && !invincible(victim->shadow))
                kill_all(victim->shadow);   /* one whose kill failed above */
            while (victim->incarnations)
                if ((e = kill_incarnation(victim, victim->incarnations, 0))) {
                    last = e;
                    break;
                }
        }
        for (struct ros_module *m = chain; m; m = m->next)
            m->cleared = 0;
        return last;
    }

    case 10:                            /* Insert from memory: R1 an RMA block, taken */
    case 11: {                          /* Insert and move: R1, R2 bytes, copied to the RMA */
        /* The image is placed as OS_Module 1 places a file's image
         * (place_module), with no tail. In call 10 the block is the
         * module's from now on. The box must know its size, so it must be
         * an RMA block (one from OS_Module 6). */
        uint32_t size = s->r[2], bsize = 0;
        uint8_t *image;
        if (s->r[0] == 10) {
            if ((e = ros_heap_block_size(hpd, s->r[1], &bsize)))
                return e;
            image = ros_ptr(s->r[1]);
            size = bsize - 4;
        } else {
            if (size < 0x34 || size > 0x4000000u)
                return ros_error(ERR_NOT_MOD, "This is not a relocatable module");
            if (!(image = ros_rma_alloc(size)))
                return no_room();
            memcpy(image, ros_ptr(s->r[1]), size);
        }
        uint32_t tail = arena_string("");
        if (!tail) {
            if (s->r[0] == 11)
                ros_rma_free(image);
            return no_room();
        }
        struct ros_module *m;
        e = place_module(image, size, s->r[0] == 10 ? "Module in memory" : "Module image", tail, &m);
        ros_rma_free(ros_ptr(tail));
        return e;
    }

    case 12:
    case 22:
        return get_names(s, kind);

    case 13:
        return rma_extend(&s->r[2], (int32_t)s->r[3]);

    case 14: {                          /* NewIncarnation: "Title%Postfix" */
        e = lookup(s->r[1], &f);
        if (e && e->errnum == ERR_INC_NOT_FOUND) {
            uint32_t end;
            str_cmp(s->r[1], f.m ? f.m->title : "", '%', &end);
            return add_incarnation(f.m, end + 1, skip_name(s->r[1]));
        }
        if (e)
            return start_from_rom(s->r[1], 1);
        if (!f.named)
            return ros_error(ERR_POSTFIX_NEEDED, "Postfix not specified");
        return ros_error(ERR_INC_EXISTS, "Incarnation already exists");
    }

    case 15: {                          /* RenameIncarnation: R2 the new postfix */
        if ((e = lookup(s->r[1], &f)))
            return e;
        for (uint32_t node = f.m->incarnations; node; node = inc_next(node)) {
            uint32_t end;
            if (str_cmp(s->r[2], ros_ptr(inc_postfix(node)), 0, &end))
                return ros_error(ERR_INC_EXISTS, "Incarnation already exists");
        }
        uint32_t node = new_node(s->r[2]);
        if (!node)
            return no_room();
        ros_st32(node, inc_next(f.node));
        ros_st32(inc_word(node), ros_ld32(inc_word(f.node)));
        if (f.prev)
            ros_st32(f.prev, node);
        else
            f.m->incarnations = node;
        if (f.m->base_incarnation == f.node)
            f.m->base_incarnation = node;
        ros_rma_free(ros_ptr(f.node));
        prefer_first(f.m);
        return NULL;
    }

    case 16:                            /* MakePreferred */
        if ((e = lookup(s->r[1], &f)))
            return e;
        if (f.named && f.prev) {
            ros_st32(f.prev, inc_next(f.node));
            ros_st32(f.node, f.m->incarnations);
            f.m->incarnations = f.node;
            prefer_first(f.m);
        }
        return NULL;

    case 17:
        return cannot("Podule modules");

    case 18:                            /* LookupName: a shadow by its twin's number */
        if ((e = lookup_as(s->r[1], &f, kind)))
            return e;
        s->r[1] = (uint32_t)module_number(f.m->twin ? f.m->twin : f.m);
        {
            uint32_t n = 0;
            for (uint32_t node = f.m->incarnations; node && node != f.node; node = inc_next(node))
                n++;
            s->r[2] = n;
        }
        s->r[3] = f.m->base;
        s->r[4] = ros_ld32(inc_word(f.node));
        s->r[5] = inc_postfix(f.node);
        return NULL;

    case 19:
    case 20:                            /* EnumerateROM_Modules(WithInfo) */
        if (s->r[2] != 0xFFFFFFFFu || s->r[1] >= rom_count)
            return ros_error(ERR_NO_MORE_MODULES, "No more modules");
        {
            struct ros_module *m = rom[s->r[1]++];
            s->r[3] = m->base ? m->base + ros_ld32(m->base + 0x10) : 0;
            s->r[4] = in_chain(m) ? 1 : 0;
            if (s->r[0] == 20)
                s->r[6] = help_version(m->help);
        }
        return NULL;

    case 21:                            /* FindEndOfROM_ModuleChain */
        if (s->r[1] != 0xFFFFFFFFu)
            return ros_error(ERR_BAD_PARAMETERS, "Parameters not recognised");
        s->r[2] = ROS_ROM_BASE + ROS_ROM_SIZE;
        return NULL;

    case 24:
        return rma_claim_aligned(s->r[3], s->r[4], &s->r[2]);

    default:                            /* 23, UnplugInsert, as the kernel: naff */
        return ros_error(ERR_BAD_MODULE_REASON, "Unknown OS_Module call");
    }
}

void ros_thunk_OS_Module(struct ros_cpu *s)
{
    uint32_t outer = ros_svc_sp_enter(s);
    s->v = 0;
    os_error *e = module_op(s, ros_caller_kind());
    if (e)
        ros_swi_fail(s, e);
    ros_svc_sp = outer;
}

/* For *commands: the module's version from its help string. */
uint32_t ros_module_version(const struct ros_module *m)
{
    return help_version(m->help);
}

/* The ROM's modules, in order. */
struct ros_module *ros_module_rom(unsigned i)
{
    return i < rom_count ? rom[i] : NULL;
}
