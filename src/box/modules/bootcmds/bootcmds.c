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
 * This file is a reimplementation in C of RISC OS Open's BootCommands module
 * (Sources/Programmer/BootCmds: c.main, c.repeatcmd, cmhg.header, h.main).
 */

/* bootcmds.c: BootCommands, a native module.  It provides the * commands
 * that !Boot sequences are written in.
 *
 * RISC OS's BootCommands is a C module (Programmer/BootCmds, version 1.54
 * in the 5.31 sources).  This file is a rewrite of it for ROSGD, made to
 * the behaviour of that source.  It was written from it and held to
 * RISC OS 5.30's module on the farm (tests/desktop/bootcmds).  It keeps
 * everything that !Boot sequences rely on:
 *   - each command's parameters as its table gives them to the kernel,
 *     and the GSTrans that it asks for;
 *   - the text that it writes;
 *   - its errors, with their numbers and texts;
 *   - the ResourceFS files that *AddApp makes, byte for byte;
 *   - the layout of the CMOS file;
 *   - the return code and X$Error that *Repeat leaves.
 *
 * Where ROSGD differs underneath, the work differs too.
 *   - *Repeat runs inside this module.  RISC OS runs an application for
 *     it (Resources:$.Resources.BootCmds.Repeat), which loads at &8000 over
 *     its caller.
 *   - The memory commands work on ROSGD's memory.  The RMA is ROSGD's whole
 *     reservation from the start.  So *AppSize and *ShrinkRMA have nothing
 *     to move, and *AddToRMA only looks for the room.  The application
 *     space is the Wimp's slot.  *AppSlot and *FreePool set it with
 *     Wimp_SlotSize, after the kernel's check that nothing is running in
 *     it.
 *   - In RISC OS, *SafeLogon first checks whether the user is already
 *     logged on.  That check belongs to NetFS and ROSGD has no NetFS, so
 *     *SafeLogon always does *%Logon.
 *
 * The module's one piece of state is the C module's command buffer, in its
 * workspace in the RMA.  *Do GSTranses into it.  *IfThere and *SafeLogon
 * build their commands there, as they did in RISC OS.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bootcmds.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"

#define BUF   1025u                 /* os_CLI_LIMIT_RO4 + 1: a command line */
#define NAME  257u                  /* os_FILE_NAME_LIMIT + 1: a leaf */

/* SWIs the API does not name */
#define XDDEUtils_FlushCL  0x6258Bu
#define XBootFX_BarUpdate  0x79140u

#define ERR_FILE_NOT_FOUND  0x0D6u
#define ERR_SYNTAX          0x0DCu
#define ERR_CDA_CAO         0x1C0u  /* ChDynamCAO */
#define ERR_NOT_ALL_MOVED   0x1C1u  /* ChDynamNotAllMoved */
#define ERR_APL_IN_USE      0x1C2u  /* AplWSpaceInUse */
#define ERR_BUFF_OVERFLOW   0x1E4u
#define ERR_BAD_RETURN_CODE 0x800E06u   /* the C library's, from exit() */

#define OBJ_NOTHING 0u
#define OBJ_FILE    1u
#define OBJ_DIR     2u
#define OBJ_IMAGE   3u
#define TYPE_OBEY   0xFEBu
#define TYPE_CONFIG 0xFF2u

#define SERVICE_MEMORY       0x11u
#define UPCALL_MOVING_MEMORY 257u
#define UPCALL_CLAIMED       0u

#define CMOS_CHECKSUM 239u          /* osbyte_CONFIGURE_CHECKSUM */
#define CMOS_DST      220u          /* osbyte_CONFIGURE_DST.  Its DST bit keeps the current value. */
#define CMOS_DST_MASK 0x80u
#define CMOS_YEAR0    128u          /* osbyte_CONFIGURE_YEAR0 and _YEAR1.  They are never loaded. */
#define CMOS_YEAR1    129u
#define CMOS_SEED     1u            /* CMOSxseed */
#define BOOT_VERSION  370           /* the value used when Boot$OSVersion is not set */

#define BOOTFX_HANDLE 0xB007CED5u
#define TASK          0x4B534154u   /* "TASK" */

/* ---- the workspace ------------------------------------------------------------ */

struct workspace {
    char buffer[BUF];               /* the C module's static buffer */
};

static struct workspace *ws(void)
{
    uint32_t w = bootcmds_module.private_word ? ros_ld32(bootcmds_module.private_word) : 0;
    return w ? ros_ptr(w) : NULL;
}

/* ---- calling SWIs ------------------------------------------------------------- */

/* Blocks in the RMA for what a command hands to SWIs, freed together */
struct pool {
    void *p[24];
    unsigned n;
};

static void *pool_alloc(struct pool *pl, uint32_t size)
{
    if (pl->n == sizeof pl->p / sizeof pl->p[0])
        return NULL;
    void *b = ros_rma_alloc(size ? size : 1);
    if (b) {
        memset(b, 0, size ? size : 1);
        pl->p[pl->n++] = b;
    }
    return b;
}

/* A string in the arena: its address, or 0 */
static uint32_t pool_str(struct pool *pl, const char *s)
{
    size_t n = strlen(s) + 1;
    char *b = pool_alloc(pl, (uint32_t)n);
    if (!b)
        return 0;
    memcpy(b, s, n);
    return ros_addr(b);
}

static void pool_free(struct pool *pl)
{
    while (pl->n)
        ros_rma_free(pl->p[--pl->n]);
}

static os_error *no_room(void)
{
    return ros_error(BOOTCMDS_ERR_NO_MEM, "Not enough memory");
}

/* Calls a SWI with up to seven registers loaded from r.  It returns the error,
 * or NULL.  The registers come back in r. */
static os_error *swi(uint32_t number, uint32_t *r, int n)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, (size_t)n * 4);
    ros_swi(&c, number);
    memcpy(r, c.r, 8 * 4);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* Copies an error into a block of the runtime's own, so that it survives later SWIs. */
static os_error *keep(const os_error *e)
{
    return e ? ros_error(e->errnum, "%s", e->errmess) : NULL;
}

/* Copies the string at an arena address into out, up to its control-character terminator. */
static void from_arena(uint32_t a, char *out, size_t max)
{
    size_t i = 0;
    for (uint32_t c; i + 1 < max && (c = ros_ld8(a + (uint32_t)i)) >= ' '; i++)
        out[i] = (char)c;
    out[i] = 0;
}

/* These write text and a new line, as the C library's stderr does. */
static os_error *write_s(const char *s)
{
    os_error *e = NULL;
    for (; *s && !e; s++)
        e = xos_write_c((uint8_t)*s);
    return e;
}

static os_error *write_line(const char *s)
{
    os_error *e = write_s(s);
    return e ? e : xos_new_line();
}

/* Like getenv().  It reads a variable's value, expanded, into out, which may
 * be NULL.  It returns 1 if the variable exists and 0 if not.  When it does
 * not exist, *e gets the error, as the C library records it. */
static int getenv_(const char *name, char *out, size_t max, os_error **e)
{
    struct pool pl = { 0 };
    uint32_t n = pool_str(&pl, name), b = ros_addr(pool_alloc(&pl, 1024));
    int got = 0;
    if (n && b) {
        uint32_t r[8] = { n, b, 1023, 0, 3 };
        os_error *x = swi(XOS_ReadVarVal, r, 5);
        if (x) {
            if (e)
                *e = keep(x);
        } else {
            got = 1;
            if (out) {
                size_t len = r[2] < max ? r[2] : max - 1;
                memcpy(out, ros_ptr(b), len);
                out[len] = 0;
            }
        }
    }
    pool_free(&pl);
    return got;
}

/* OS_SetVarVal of a string.  The type is VarType_String, so the value is GSTransed. */
static os_error *set_var(const char *name, const char *value, size_t len)
{
    struct pool pl = { 0 };
    uint32_t n = pool_str(&pl, name);
    char *v = pool_alloc(&pl, (uint32_t)len + 1);
    os_error *e = NULL;
    if (!n || !v) {
        e = no_room();
    } else {
        memcpy(v, value, len);
        uint32_t r[8] = { n, ros_addr(v), (uint32_t)len, 0, 0 };
        e = keep(swi(XOS_SetVarVal, r, 5));
    }
    pool_free(&pl);
    return e;
}

/* OS_File 23 gives an object's type (0 when there is none), length and file
 * type.  It does not search a path.  The result is the error if the call
 * fails. */
static os_error *stat_(const char *name, uint32_t *type, uint32_t *length, uint32_t *ftype)
{
    struct pool pl = { 0 };
    uint32_t n = pool_str(&pl, name);
    os_error *e = NULL;
    if (!n) {
        e = no_room();
    } else {
        uint32_t r[8] = { 23, n };
        e = keep(swi(XOS_File, r, 2));
        if (!e) {
            *type = r[0];
            if (length)
                *length = r[4];
            if (ftype)
                *ftype = r[6];
        }
    }
    pool_free(&pl);
    return e;
}

/* OS_File 19: FileSwitch's error for what a name turned out to be */
static os_error *make_error(const char *name, uint32_t type)
{
    struct pool pl = { 0 };
    uint32_t n = pool_str(&pl, name);
    os_error *e = no_room();
    if (n) {
        uint32_t r[8] = { 19, n, type };
        e = keep(swi(XOS_File, r, 3));
    }
    pool_free(&pl);
    return e;
}

/* OS_FSControl 37: a name canonicalised into out */
static os_error *canonicalise(const char *name, char *out, size_t max)
{
    struct pool pl = { 0 };
    uint32_t n = pool_str(&pl, name);
    char *b = pool_alloc(&pl, (uint32_t)max);
    os_error *e = NULL;
    if (!n || !b) {
        e = no_room();
    } else {
        uint32_t r[8] = { 37, n, ros_addr(b), 0, 0, (uint32_t)max };
        e = keep(swi(XOS_FSControl, r, 6));
        if (!e) {
            memcpy(out, b, max);
            out[max - 1] = 0;
        }
    }
    pool_free(&pl);
    return e;
}

/* Reads a size at p with OS_ReadUnsigned, in base 10 unless the text gives
 * another base, then applies a K or M suffix.  The value wraps at 32 bits,
 * as the module's unsigned int does. */
static os_error *read_size(uint32_t p, uint32_t *value)
{
    while (ros_ld8(p) == ' ')
        p++;
    uint32_t r[8] = { 0, p };
    os_error *e = swi(XOS_ReadUnsigned, r, 2);
    if (e)
        return e;
    uint32_t v = r[2];
    switch (ros_ld8(r[1])) {
    case 'm': case 'M':
        v *= 1024;
        /* fall through */
    case 'k': case 'K':
        v *= 1024;
        break;
    }
    *value = v;
    return NULL;
}

/* Reads the tail with OS_ReadArgs into a block of the pool.  The block holds the item pointers. */
static os_error *read_args(struct pool *pl, const char *keys, uint32_t tail, uint32_t **items)
{
    uint32_t k = pool_str(pl, keys);
    uint8_t *out = pool_alloc(pl, BUF + 64);
    if (!k || !out)
        return no_room();
    uint32_t r[8] = { k, tail, ros_addr(out), BUF + 64 };
    os_error *e = swi(XOS_ReadArgs, r, 4);
    *items = (uint32_t *)out;
    return e;
}

/* Converts an OS_ReadArgs string item, an arena address that is 0 when absent, to a C pointer or NULL. */
static const char *item(uint32_t a)
{
    return a ? (const char *)ros_ptr(a) : NULL;
}

/* ---- *AddApp ------------------------------------------------------------------- */

#define ALIGN4(n) (((n) + 3u) & ~3u)

/* One file of a ResourceFS block.  It returns the file's size, and writes the file at f when f is not NULL. */
static uint32_t rfs_file(uint8_t *f, const char *name, const char *data, uint32_t load,
                         uint32_t exec)
{
    uint32_t nlen = (uint32_t)strlen(name), dlen = (uint32_t)strlen(data);
    uint32_t hsize = 20 + ALIGN4(nlen + 1), dsize = 4 + ALIGN4(dlen);
    if (f) {
        uint32_t a = ros_addr(f);
        ros_st32(a, hsize + dsize);             /* to the next file */
        ros_st32(a + 4, load);
        ros_st32(a + 8, exec);
        ros_st32(a + 12, dlen);
        ros_st32(a + 16, 3);                    /* owner read and write */
        memcpy(f + 20, name, nlen + 1);
        ros_st32(a + hsize, dlen + 4);
        memcpy(f + hsize + 4, data, dlen);
    }
    return hsize + dsize;
}

/* The stubs for one application.  There is a !Boot if it has a !Boot or a
 * !Sprites, a !Help if it has one, and a !Run always.  They are Obey files
 * stamped now, which run or load the real ones.  They are registered with
 * ResourceFS in a block of the RMA that stays theirs. */
static os_error *register_app(const char *canon, const char *entry)
{
    char name[BUF + 16], boot[BUF + 32], help[BUF + 32], run[BUF + 32];
    char boot_name[BUF], help_name[BUF], run_name[BUF];
    uint32_t boot_type, sprites_type, help_type;
    os_error *e;

    snprintf(name, sizeof name, "%s.!Boot", canon);
    if ((e = stat_(name, &boot_type, NULL, NULL)) != NULL)
        return e;
    snprintf(name, sizeof name, "%s.!Sprites", canon);
    if ((e = stat_(name, &sprites_type, NULL, NULL)) != NULL)
        return e;
    int has_boot = boot_type != OBJ_NOTHING || sprites_type != OBJ_NOTHING;
    if (boot_type != OBJ_NOTHING)
        snprintf(boot, sizeof boot, "/%s.!Boot %%*0\n", canon);
    else
        snprintf(boot, sizeof boot, "IconSprites %s.!Sprites\n", canon);
    snprintf(boot_name, sizeof boot_name, "Apps.%s.!Boot", entry);

    snprintf(name, sizeof name, "%s.!Help", canon);
    if ((e = stat_(name, &help_type, NULL, NULL)) != NULL)
        return e;
    snprintf(help, sizeof help, "Filer_Run %s.!Help\n", canon);
    snprintf(help_name, sizeof help_name, "Apps.%s.!Help", entry);

    snprintf(run, sizeof run, "/%s %%*0\n", canon);
    snprintf(run_name, sizeof run_name, "Apps.%s.!Run", entry);

    uint32_t size = 4;                          /* the terminating word */
    if (has_boot)
        size += rfs_file(NULL, boot_name, boot, 0, 0);
    if (help_type != OBJ_NOTHING)
        size += rfs_file(NULL, help_name, help, 0, 0);
    size += rfs_file(NULL, run_name, run, 0, 0);

    void *block;
    if ((e = xos_module_claim(size, &block)) != NULL)
        return e;

    /* All the files share one load and execution address.  The file type is
     * Obey and the time stamp is the time that the current program started,
     * as OS_GetEnv gives it. */
    uint32_t r[8] = { 0 };
    if ((e = swi(XOS_GetEnv, r, 0)) != NULL)
        return e;
    uint32_t exec = ros_ld32(r[2]);
    uint32_t load = 0xFFF00000u | TYPE_OBEY << 8 | ros_ld8(r[2] + 4);

    uint8_t *f = block;
    if (has_boot)
        f += rfs_file(f, boot_name, boot, load, exec);
    if (help_type != OBJ_NOTHING)
        f += rfs_file(f, help_name, help, load, exec);
    f += rfs_file(f, run_name, run, load, exec);
    ros_st32(ros_addr(f), 0);

    uint32_t rr[8] = { ros_addr(block) };
    return keep(swi(XResourceFS_RegisterFiles, rr, 1));
}

/* Whether the canonical directory is ResourceFS's Apps itself.  An
 * application already there would get stubs that run themselves.  The test
 * is Territory_Collate ignoring case, as the module asks.  Where no
 * territory answers, the same test is made on ASCII. */
static os_error *is_resource_apps(const char *canon, int *same)
{
    static const char apps[] = "Resources:$.Apps";
    struct pool pl = { 0 };
    uint32_t a = pool_str(&pl, canon), b = pool_str(&pl, apps);
    os_error *e = NULL;
    if (!a || !b) {
        e = no_room();
    } else {
        uint32_t r[8] = { (uint32_t)-1, a, b, 1 };
        os_error *x = swi(XTerritory_Collate, r, 4);
        if (x && x->errnum == ROS_ERR_NO_SUCH_SWI)
            *same = strcasecmp(canon, apps) == 0;
        else if (x)
            e = keep(x);
        else
            *same = r[0] == 0;
    }
    pool_free(&pl);
    return e;
}

os_error *bootcmds_add_app(const char *spec)
{
    char dir[BUF], canon[BUF], app[BUF + NAME], entry[NAME];
    const char *leaf, *concat = "%s.%s";
    os_error *e;

    const char *dot = strrchr(spec, '.');
    if (!dot)
        dot = strrchr(spec, ':');
    if (dot) {
        snprintf(dir, sizeof dir, "%.*s", (int)(dot - spec), spec);
        if (*dot == ':') {                      /* a path: kept in path form */
            concat = "%s%s";
            strncat(dir, ":", sizeof dir - strlen(dir) - 1);
        }
        leaf = dot + 1;
    } else {
        strcpy(dir, "@");
        leaf = spec;
    }
    int quiet = strpbrk(leaf, "*#") != NULL;    /* a wildcard may find none */

    if ((e = canonicalise(dir, canon, sizeof canon)) != NULL)
        return e;
    int same = 0;
    if ((e = is_resource_apps(canon, &same)) != NULL || same)
        return e;

    struct pool pl = { 0 };
    uint32_t d = pool_str(&pl, dir), l = pool_str(&pl, leaf);
    char *buf = pool_alloc(&pl, NAME);
    if (!d || !l || !buf) {
        pool_free(&pl);
        return no_room();
    }
    for (uint32_t context = 0; context != (uint32_t)-1;) {
        uint32_t r[8] = { 9, d, ros_addr(buf), 1, context, NAME, l };
        os_error *x = swi(XOS_GBPB, r, 7);
        if (x && x->errnum == ERR_FILE_NOT_FOUND) {
            e = keep(x);            /* stop; the report below names the leaf unless it is a wildcard */
            break;
        }
        if (x) {
            e = keep(x);
            pool_free(&pl);
            return e;
        }
        context = r[4];
        if (r[3] != 1)
            continue;
        snprintf(entry, sizeof entry, "%s", buf);
        snprintf(app, sizeof app, concat, dir, entry);
        if ((e = canonicalise(app, canon, sizeof canon)) != NULL ||
            (e = register_app(canon, entry)) != NULL) {
            pool_free(&pl);
            return e;
        }
        quiet = 1;
    }
    pool_free(&pl);
    if (!quiet)
        e = make_error(leaf, OBJ_NOTHING + 0x100);  /* "Directory '<leaf>' not found" */
    return e;
}

/* ---- *AppSize, *AppSlot, *AddToRMA, *ShrinkRMA, *FreePool ----------------------- */

/* The kernel's CheckAppSpace, run before the application space shrinks.
 * A current object outside the space is asked with Service_Memory, which a
 * program still using the space claims.  One inside the space is asked
 * with UpCall_MovingMemory, which it must claim to agree. */
static os_error *check_app_space(int32_t change)
{
    uint32_t r[8] = { 14, 0, 0, 0 };                /* AplWorkSize */
    os_error *e = swi(XOS_ChangeEnvironment, r, 4);
    if (e)
        return e;
    uint32_t top = r[1];
    uint32_t c[8] = { 15, 0, 0, 0 };                /* Curr_Active_Object */
    if ((e = swi(XOS_ChangeEnvironment, c, 4)) != NULL)
        return e;
    uint32_t cao = c[1];
    if (cao > top) {
        uint32_t s[8] = { (uint32_t)change, SERVICE_MEMORY, cao };
        swi(XOS_ServiceCall, s, 3);
        return s[1] == 0 ? ros_error(ERR_APL_IN_USE, "Application memory area in use") : NULL;
    }
    uint32_t u[8] = { UPCALL_MOVING_MEMORY, (uint32_t)(change < 0 ? -change : change) };
    swi(XOS_UpCall, u, 2);
    return u[0] == UPCALL_CLAIMED ? NULL
                                  : ros_error(ERR_CDA_CAO, "Can't change memory area "
                                                           "(application running)");
}

/* Sets the application space to size bytes, from have, the size it has now.
 * Outside the desktop, a program's own space is resized.  Inside it, a
 * task's slot is set with Wimp_SlotSize. */
static os_error *set_app_space(uint32_t have, uint32_t size)
{
    int32_t move = (int32_t)(have - size);
    if (move == 0)
        return NULL;
    os_error *e;
    if (move > 0 && (e = check_app_space(-move)) != NULL)
        return e;
    if (ros_task_resize_space(size) == 0)
        return NULL;
    uint32_t r[8] = { size, (uint32_t)-1 };
    if ((e = swi(XWimp_SlotSize, r, 2)) != NULL)
        return e;
    uint32_t g[8] = { 0 };
    if ((e = swi(XOS_GetEnv, g, 0)) != NULL)
        return e;
    if (move < 0 && g[1] - ROS_APP_BASE < ((size + 4095) & ~4095u))
        return ros_error(ERR_NOT_ALL_MOVED, "Memory cannot be moved");
    return NULL;
}

os_error *bootcmds_app_slot(uint32_t size)
{
    uint32_t r[8] = { 0 };
    os_error *e = swi(XOS_GetEnv, r, 0);
    return e ? e : set_app_space(r[1] - ROS_APP_BASE, size);
}

static os_error *cmd_app_size(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t value, r[8] = { 0 };
    os_error *e = read_size(tail, &value);
    if (!e)
        e = swi(XOS_GetEnv, r, 0);
    /* RISC OS moves the RMA by the difference between the application
     * space and the size, and does not care whether it can.  ROSGD's RMA is
     * its whole reservation already, so there is nothing to move. */
    return e;
}

static os_error *cmd_app_slot(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t value;
    os_error *e = read_size(tail, &value);
    return e ? e : bootcmds_app_slot(value);
}

static os_error *cmd_add_to_rma(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t value;
    os_error *e = read_size(tail, &value);
    if (e)
        return e;
    /* Add_To_RMA: grows the RMA, dynamic area 1, by that many bytes.  Its
     * error is passed back unchanged. */
    uint32_t r[8] = { 1, value };
    return swi(XOS_ChangeDynamicArea, r, 2);
}

/* Shrink_RMA: shrinks the RMA by its size, which takes it as far as it will
 * go, to its top block.  The error from the shrink is ignored. */
static os_error *cmd_shrink_rma(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    uint32_t r[8] = { 1 };
    os_error *e = swi(XOS_ReadDynamicArea, r, 1);
    if (e)
        return e;
    uint32_t c[8] = { 1, (uint32_t)-(int32_t)r[1] };
    swi(XOS_ChangeDynamicArea, c, 2);
    return NULL;
}

/* Makes the application space the next slot's size.  The free pool takes all
 * but that, less the &8000 that the module's arithmetic leaves out.  The
 * module takes the space's end for its size. */
static os_error *cmd_free_pool(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    uint32_t r[8] = { (uint32_t)-1, (uint32_t)-1 };
    os_error *e = swi(XWimp_SlotSize, r, 2);
    if (e)
        return e;
    uint32_t next = r[1];
    uint32_t c[8] = { 14, 0, 0, 0 };
    if ((e = swi(XOS_ChangeEnvironment, c, 4)) != NULL)
        return e;
    uint32_t have = c[1] - ROS_APP_BASE;
    return set_app_space(have, next > ROS_APP_BASE ? next - ROS_APP_BASE : 0);
}

/* ---- *Do, *IfThere, *X, *SafeLogon ----------------------------------------------- */

static os_error *cli(uint32_t line)
{
    uint32_t r[8] = { line };
    return swi(XOS_CLI, r, 1);
}

static os_error *cli_s(const char *line)
{
    struct pool pl = { 0 };
    uint32_t l = pool_str(&pl, line);
    os_error *e = l ? keep(cli(l)) : no_room();
    pool_free(&pl);
    return e;
}

/* The tail is GSTransed into the buffer and then run.  GSTrans does not
 * clear the buffer.  It marks the end of its output with the character that
 * it stopped at. */
static os_error *do_tail(uint32_t tail)
{
    uint32_t buf = ros_addr(ws()->buffer);
    uint32_t r[8] = { tail, buf, BUF };
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, sizeof r);
    ros_swi(&c, XOS_GSTrans);
    if (c.v)
        return ros_ptr(c.r[0]);
    if (c.c)
        return ros_error(ERR_BUFF_OVERFLOW, "Buffer overflow");
    return cli(buf);
}

os_error *bootcmds_do(const char *line)
{
    struct pool pl = { 0 };
    uint32_t l = pool_str(&pl, line);
    os_error *e = l ? keep(do_tail(l)) : no_room();
    pool_free(&pl);
    return e;
}

static os_error *cmd_do(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return do_tail(tail);
}

int bootcmds_there(const char *name)
{
    uint32_t type;
    return stat_(name, &type, NULL, NULL) == NULL && type != OBJ_NOTHING;
}

/* Builds "If 1 <the rest>" or "If 0 <the rest>", so that *If parses Then and
 * Else.  Its syntax error becomes *IfThere's own. */
static os_error *cmd_if_there(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char *b = ws()->buffer, name[BUF];
    uint32_t p = tail;
    while (ros_ld8(p) == ' ')
        p++;
    size_t i = 0;
    while (ros_ld8(p) > ' ' && i < BUF - 1)
        name[i++] = (char)ros_ld8(p++);
    name[i] = 0;
    char rest[BUF];
    from_arena(p, rest, sizeof rest);
    snprintf(b, BUF, "If %d%s", bootcmds_there(name), rest);
    for (i = strlen(b); i > 0 && b[i - 1] == ' ';)
        b[--i] = 0;
    os_error *e = cli(ros_addr(b));
    if (e && e->errnum == ERR_SYNTAX)
        e = cli_s("IfThere");
    return e;
}

os_error *bootcmds_x(const char *command)
{
    os_error *e = cli_s(command);
    if (!e)
        return NULL;
    if (getenv_("X$Error", NULL, 0, NULL))
        return NULL;
    char text[256];
    snprintf(text, sizeof text, "%s", e->errmess);
    return set_var("X$Error", text, strlen(text));
}

static os_error *cmd_x(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    os_error *e = cli(tail);
    if (!e)
        return NULL;
    char text[256];
    snprintf(text, sizeof text, "%s", e->errmess);
    if (getenv_("X$Error", NULL, 0, NULL))
        return NULL;
    return set_var("X$Error", text, strlen(text));
}

static os_error *cmd_safe_logon(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char *b = ws()->buffer, rest[BUF];
    from_arena(tail, rest, sizeof rest);
    snprintf(b, BUF, "%%Logon %s", rest);
    return cli(ros_addr(b));
}

/* ---- *LoadCMOS, *SaveCMOS -------------------------------------------------------- */

/* How many bytes of settings the files hold.  This is OS_NVMemory's size, or
 * the 240 bytes of CMOS where OS_NVMemory gives none. */
static uint32_t nvm_size(void)
{
    uint32_t r[8] = { 0 };
    if (swi(XOS_NVMemory, r, 1))
        return CMOS_CHECKSUM + 1;
    return r[0] ? CMOS_CHECKSUM + 1 : r[1];
}

/* Reads Boot$OSVersion, a decimal number, into *version.  When the variable
 * is not set, *set is 0 and *version is left alone. */
static os_error *boot_version(int *version, int *set)
{
    char v[64];
    *set = getenv_("Boot$OSVersion", v, sizeof v, NULL);
    if (!*set)
        return NULL;
    struct pool pl = { 0 };
    uint32_t a = pool_str(&pl, v);
    os_error *e = no_room();
    if (a) {
        uint32_t r[8] = { 0x80000000u | 10, a };
        e = keep(swi(XOS_ReadUnsigned, r, 2));
        if (!e)
            *version = (int)r[2];
    }
    pool_free(&pl);
    return e;
}

/* A file open for the CMOS commands.  It is read one byte at a time, as the
 * module reads it.  At the end of the file nothing is read, and the byte
 * variable keeps the value that it had. */
struct cfile {
    uint32_t handle, byte;
    uint32_t scratch;               /* 8 bytes in the RMA the reads go to */
};

static os_error *c_read(struct cfile *f, uint32_t *into)
{
    uint32_t r[8] = { 4, f->handle, f->scratch, 1 };
    os_error *e = swi(XOS_GBPB, r, 4);
    if (!e && r[3] == 0)
        *into = (*into & ~0xFFu) | ros_ld8(f->scratch);
    return e;
}

static os_error *c_seek(struct cfile *f, uint32_t at)
{
    uint32_t r[8] = { 1, f->handle, at };
    return swi(XOS_Args, r, 3);
}

static os_error *c_close(struct cfile *f)
{
    uint32_t r[8] = { 0, f->handle };
    return swi(XOS_Find, r, 2);
}

static os_error *cmos_byte(uint32_t reason, uint32_t address, uint32_t value, uint32_t *out)
{
    uint32_t r[8] = { reason, address, value };
    os_error *e = swi(XOS_Byte, r, 3);
    if (!e && out)
        *out = r[2];
    return e;
}

static os_error *bad_file(void)
{
    return ros_error(BOOTCMDS_ERR_BAD_FILE, "Corrupt CMOS file");
}

static os_error *bad_version(void)
{
    return ros_error(BOOTCMDS_ERR_BAD_VER, "CMOS file is for a different OS version");
}

/* The file is checked first: its size, its OS version word and its
 * checksum.  Only then are its settings written, all but the DST bit and
 * the year. */
static os_error *load_cmos(const char *file, struct cfile *f, int *open)
{
    uint32_t nvm = nvm_size(), type, size, ftype;
    int version = BOOT_VERSION, set;
    os_error *e;
    if ((e = stat_(file, &type, &size, &ftype)) != NULL)
        return e;
    if (type != OBJ_FILE)
        return make_error(file, type);

    struct pool pl = { 0 };
    uint32_t n = pool_str(&pl, file);
    uint32_t r[8] = { 0x40 | 0x08 | 0x04 | 0x03, n };   /* no path, errors if absent or a directory */
    e = n ? keep(swi(XOS_Find, r, 2)) : no_room();
    pool_free(&pl);
    if (e)
        return e;
    f->handle = r[0];
    *open = 1;

    if (size != 240 && size != 244 && size != nvm + 4)
        return bad_file();
    if ((e = boot_version(&version, &set)) != NULL)
        return e;
    if (size == 240 && version > BOOT_VERSION)
        return bad_version();
    if (size == nvm + 4) {
        uint32_t g[8] = { 3, f->handle, f->scratch, 4, nvm };
        if ((e = swi(XOS_GBPB, g, 5)) != NULL)
            return e;
        if ((int)ros_ld32(f->scratch) != version)
            return bad_version();
        if ((e = c_seek(f, 0)) != NULL)
            return e;
    }

    uint32_t sum = CMOS_SEED, file_sum = 0, i;
    for (i = 0; i < CMOS_CHECKSUM; i++) {
        if ((e = c_read(f, &f->byte)) != NULL)
            return e;
        sum += f->byte & 0xFF;
    }
    if ((e = c_read(f, &file_sum)) != NULL)
        return e;
    if (nvm > 256) {
        for (i = 240; i <= 255; i++)            /* not in the checksum */
            if ((e = c_read(f, &f->byte)) != NULL)
                return e;
        for (i = 256; i < nvm; i++) {
            if ((e = c_read(f, &f->byte)) != NULL)
                return e;
            sum += f->byte & 0xFF;
        }
    }
    if ((sum & 0xFF) != (file_sum & 0xFF))
        return bad_file();
    if ((e = c_seek(f, 0)) != NULL)
        return e;

    for (i = 0; i < nvm; i++) {
        uint32_t now;
        if ((e = c_read(f, &f->byte)) != NULL)
            return e;
        uint32_t w = f->byte;
        switch (i) {
        case CMOS_DST:
            if ((e = cmos_byte(161, i, 0, &now)) != NULL ||
                (e = cmos_byte(162, i, (w & ~CMOS_DST_MASK) | (now & CMOS_DST_MASK), NULL)))
                return e;
            file_sum -= w & CMOS_DST_MASK;       /* the checksum follows the bit kept */
            file_sum += now & CMOS_DST_MASK;
            break;
        case CMOS_YEAR0:
        case CMOS_YEAR1:
            if ((e = cmos_byte(161, i, 0, &now)) != NULL)
                return e;
            file_sum -= w & 0xFF;
            file_sum += now & 0xFF;
            break;
        default:
            if ((e = cmos_byte(162, i, w & 0xFF, NULL)) != NULL)
                return e;
            break;
        }
    }
    return cmos_byte(162, CMOS_CHECKSUM, file_sum & 0xFF, NULL);
}

os_error *bootcmds_load_cmos(const char *file)
{
    void *scratch = ros_rma_alloc(8);
    if (!scratch)
        return no_room();
    struct cfile f = { 0, 0, ros_addr(scratch) };
    int open = 0;
    os_error *e = load_cmos(file, &f, &open);
    if (open) {
        os_error *e1 = c_close(&f);
        if (!e)
            e = keep(e1);
    }
    ros_rma_free(scratch);
    return e;
}

/* Never an error: it would only stop a boot sequence.  What went wrong is
 * written instead. */
static os_error *cmd_load_cmos(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct pool pl = { 0 };
    uint32_t *items;
    os_error *e = read_args(&pl, "file/a", tail, &items);
    if (!e)
        e = bootcmds_load_cmos(item(items[0]));
    if (e) {
        char text[256];
        snprintf(text, sizeof text, "%s", e->errmess);
        write_line(text);
    }
    pool_free(&pl);
    return NULL;
}

/* Writes the settings, then the OS version, then sets the file type.  The
 * OS version is Boot$OSVersion.  If that is not set, it is worked out from
 * the kernel's version as BootVars does, on RISC OS 5 only.  The error from
 * setting the type decides the error returned. */
os_error *bootcmds_save_cmos(const char *file)
{
    uint32_t nvm = nvm_size();
    struct pool pl = { 0 };
    uint32_t n = pool_str(&pl, file), one = ros_addr(pool_alloc(&pl, 4));
    if (!n || !one) {
        pool_free(&pl);
        return no_room();
    }
    uint32_t r[8] = { 0x80 | 0x04 | 0x03, n };        /* no path, error if a directory */
    os_error *e = keep(swi(XOS_Find, r, 2));
    if (e) {
        pool_free(&pl);
        return e;
    }
    struct cfile f = { r[0], 0, 0 };

    for (uint32_t i = 0; i < nvm && !e; i++) {
        uint32_t v;
        if ((e = keep(cmos_byte(161, i, 0, &v))) != NULL)
            break;
        ros_st32(one, v);
        uint32_t w[8] = { 2, f.handle, one, 1 };
        e = keep(swi(XOS_GBPB, w, 4));
    }
    if (!e) {
        int version = 0, set;
        e = boot_version(&version, &set);
        if (!e && !set) {
            uint32_t m[8] = { 20, 0, (uint32_t)-1 };   /* the kernel, first in the ROM */
            e = keep(swi(XOS_Module, m, 3));
            if (!e && m[6] >> 16 == 5)
                version = (int)((m[6] & 0xF000) >> 12) * 10 + 500;
            else
                set = -1;
        }
        if (!e && set >= 0) {
            ros_st32(one, (uint32_t)version);
            uint32_t w[8] = { 2, f.handle, one, 4 };
            e = keep(swi(XOS_GBPB, w, 4));
        }
    }
    (void)e;                        /* the module loses it to the type's error */
    os_error *e1 = keep(c_close(&f));
    uint32_t t[8] = { 18, n, TYPE_CONFIG };
    e = keep(swi(XOS_File, t, 3));
    pool_free(&pl);
    return e ? e : e1;
}

static os_error *cmd_save_cmos(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct pool pl = { 0 };
    uint32_t *items;
    os_error *e = read_args(&pl, "file/a", tail, &items);
    if (!e)
        e = bootcmds_save_cmos(item(items[0]));
    pool_free(&pl);
    return e;
}

/* ---- *AppPath, *PrepPath, *RemPath, *Canonical ----------------------------------- */

static os_error *want_string(void)
{
    return ros_error(BOOTCMDS_ERR_WANT_STR, "System variable must contain a string");
}

/* A variable's type and length, unexpanded; 0 when it is not there */
static int var_info(const char *name, uint32_t *type, uint32_t *length)
{
    struct pool pl = { 0 };
    uint32_t n = pool_str(&pl, name);
    int found = 0;
    if (n) {
        uint32_t r[8] = { n, 0, (uint32_t)-1, 0, 0 };
        swi(XOS_ReadVarVal, r, 5);
        found = r[2] != 0;
        *type = r[4];
        *length = ~r[2];
    }
    pool_free(&pl);
    return found;
}

/* A variable's value as it is stored, copied into out */
static os_error *var_value(const char *name, char *out, size_t max, size_t *length)
{
    struct pool pl = { 0 };
    uint32_t n = pool_str(&pl, name);
    char *b = pool_alloc(&pl, (uint32_t)max);
    os_error *e = no_room();
    if (n && b) {
        uint32_t r[8] = { n, ros_addr(b), (uint32_t)max, 0, 0 };
        e = keep(swi(XOS_ReadVarVal, r, 5));
        if (!e) {
            *length = r[2] < max ? r[2] : max - 1;
            memcpy(out, b, *length);
            out[*length] = 0;
        }
    }
    pool_free(&pl);
    return e;
}

static int stricmp_(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x == 0)
            return -y;
        if (x != y) {
            int d = tolower(x) - tolower(y);
            if (d)
                return d;
        }
    }
}

os_error *bootcmds_path(const char *variable, const char *element, enum bootcmds_path_edit how)
{
    uint32_t type = 0, existing = 0;
    if (!var_info(variable, &type, &existing)) {
        if (how == BOOTCMDS_REMOVE)
            return NULL;                        /* nothing to take out of */
        how = BOOTCMDS_APPEND;
        existing = 0;
    } else if (type != 0) {
        return want_string();
    }
    size_t extra = how != BOOTCMDS_REMOVE ? strlen(element) + 1 : 0;
    char *path = malloc(existing + 1 + extra);
    if (!path)
        return no_room();
    size_t len = 0;
    if (existing) {
        os_error *e = var_value(variable, path, existing + 1, &len);
        if (e) {
            free(path);
            return e;
        }
    }
    path[existing] = 0;

    /* Removes every element that is the same as this one, ignoring case */
    for (char *next = path; *next;) {
        char *comma = strchr(next, ',');
        if (comma)
            *comma = 0;
        size_t n = strlen(next) + (comma != NULL);
        if (stricmp_(next, element) == 0) {
            if (!comma && next > path) {
                next[-1] = 0;                   /* the last: its comma goes too */
                break;
            }
            memmove(next, next + n, strlen(next + n) + 1);
        } else {
            next += n;
            if (comma)
                *comma = ',';
        }
    }

    if (how != BOOTCMDS_REMOVE) {
        len = strlen(path);
        if (len == 0 || how == BOOTCMDS_APPEND) {
            if (len)
                strcat(path, ",");
            strcat(path, element);
        } else {
            memmove(path + extra, path, len + 1);
            memcpy(path, element, extra - 1);
            path[extra - 1] = ',';
        }
    }
    os_error *e = set_var(variable, path, strlen(path));
    free(path);
    return e;
}

static os_error *path_command(uint32_t tail, enum bootcmds_path_edit how)
{
    struct pool pl = { 0 };
    uint32_t *items;
    os_error *e = read_args(&pl, "v/a,e/a", tail, &items);
    if (!e)
        e = bootcmds_path(item(items[0]), item(items[1]), how);
    pool_free(&pl);
    return e;
}

static os_error *cmd_app_path(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return path_command(tail, BOOTCMDS_APPEND);
}

static os_error *cmd_prep_path(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return path_command(tail, BOOTCMDS_PREPEND);
}

static os_error *cmd_rem_path(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return path_command(tail, BOOTCMDS_REMOVE);
}

/* Replaces a string or macro variable's stored value by its canonical form, set as a string */
os_error *bootcmds_canonical(const char *variable)
{
    uint32_t type = 0, length;
    var_info(variable, &type, &length);
    if (type == 1)
        return want_string();
    char *b = ws()->buffer, canon[BUF];
    size_t len;
    os_error *e = var_value(variable, b, BUF, &len);
    if (e)
        return e;
    if ((e = canonicalise(b, canon, sizeof canon)) != NULL)
        return e;
    memcpy(b, canon, BUF);
    return set_var(variable, b, strlen(b));
}

static os_error *cmd_canonical(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct pool pl = { 0 };
    uint32_t *items;
    os_error *e = read_args(&pl, "v/a", tail, &items);
    if (!e)
        e = bootcmds_canonical(item(items[0]));
    pool_free(&pl);
    return e;
}

/* ---- *Repeat --------------------------------------------------------------------- */

/* The objects of a directory, OS_GBPB 12's records in one block of the
 * RMA, and an array of pointers to their names for OS_HeapSort32 */
struct listing {
    uint8_t *block;
    uint32_t *names;
    uint32_t count;
};

static os_error *list_dir(const char *dir, struct listing *l)
{
    struct pool pl = { 0 };
    uint32_t d = pool_str(&pl, dir);
    uint8_t *chunk = pool_alloc(&pl, 4096);
    uint32_t size = 0, used = 0, count = 0;
    uint8_t *block = NULL;
    os_error *e = NULL;
    if (!d || !chunk)
        e = no_room();
    for (uint32_t context = 0; !e && context != (uint32_t)-1;) {
        uint32_t r[8] = { 12, d, ros_addr(chunk), 255, context, 4096, 0 };
        if ((e = keep(swi(XOS_GBPB, r, 7))) != NULL)
            break;
        context = r[4];
        uint32_t off = 0;
        for (uint32_t i = 0; i < r[3]; i++) {
            uint32_t n = (24 + (uint32_t)strlen((char *)chunk + off + 24) + 1 + 3) & ~3u;
            if (used + n > size) {
                uint32_t grow = size ? size * 2 : 8192;
                uint8_t *b = ros_rma_alloc(grow);
                if (!b) {
                    e = no_room();
                    break;
                }
                if (block) {
                    memcpy(b, block, used);
                    ros_rma_free(block);
                }
                block = b, size = grow;
            }
            memcpy(block + used, chunk + off, n);
            used += n, off += n, count++;
        }
    }
    pool_free(&pl);
    if (!e && count) {
        l->names = ros_rma_alloc(count * 4);
        if (!l->names)
            e = no_room();
    }
    if (e) {
        if (block)
            ros_rma_free(block);
        return e;
    }
    l->block = block, l->count = count;
    for (uint32_t i = 0, off = 0; i < count; i++) {
        l->names[i] = ros_addr(block + off + 24);
        off += (24 + (uint32_t)strlen((char *)block + off + 24) + 1 + 3) & ~3u;
    }
    return NULL;
}

static void list_free(struct listing *l)
{
    if (l->block)
        ros_rma_free(l->block);
    if (l->names)
        ros_rma_free(l->names);
}

/* Whether the options choose this object */
static int chosen(const struct bootcmds_repeat *r, uint32_t info)
{
    uint32_t type = ros_ld32(info + 16), load = ros_ld32(info), ftype = ros_ld32(info + 20);
    uint32_t f = r->flags;
    int by_type = f & BOOTCMDS_REPEAT_TYPE;
    if (type == OBJ_FILE)
        return (f & BOOTCMDS_REPEAT_FILES) || (by_type && ftype == r->type);
    if (type == OBJ_DIR)
        return (f & BOOTCMDS_REPEAT_DIRECTORIES) ||
               ((f & BOOTCMDS_REPEAT_APPLICATIONS) && ros_ld8(info + 24) == '!');
    if (type == OBJ_IMAGE)          /* an image file's type is 0x1000 or 0x2000, so its file type is taken from its load address */
        return ((f & BOOTCMDS_REPEAT_FILES) && (f & BOOTCMDS_REPEAT_DIRECTORIES)) ||
               (by_type && ((load >> 8) & 0xFFF) == r->type);
    return 0;
}

/* Sys$ReturnCode, as system() reads it after a command */
static int return_code(void)
{
    char v[32];
    return getenv_("Sys$ReturnCode", v, sizeof v, NULL) ? atoi(v) : 0;
}

/* Wimp_ReportError, for -tasks.  With e NULL and the flags "close", it takes
 * away a box left open.  The report is by category when there is a
 * sprite. */
static void report(const os_error *e, uint32_t flags, const char *sprite)
{
    struct pool pl = { 0 };
    uint32_t name = e ? pool_str(&pl, "Repeat") : 0;
    uint32_t spr = sprite ? pool_str(&pl, sprite) : 0, dots = pool_str(&pl, "...");
    os_error *b = pool_alloc(&pl, sizeof *b);
    if (b && dots) {
        if (e)
            *b = *e;
        uint32_t r[8] = { e ? ros_addr(b) : 0, flags, name, spr, sprite ? 1 : 0,
                          sprite ? dots : 0 };
        swi(XWimp_ReportError, r, 6);
    }
    pool_free(&pl);
}

os_error *bootcmds_repeat(const struct bootcmds_repeat *r)
{
    struct listing l = { 0 };
    uint32_t handle = 0;
    os_error *e = NULL, last = { 0 }, failed;
    int have_last = 0;
    int initialised = 0;
    char cmd[BUF];

    if (r->flags & BOOTCMDS_REPEAT_TASKS) {
        struct pool pl = { 0 };
        uint32_t name = pool_str(&pl, "Repeat");
        uint32_t w[8] = { 200, TASK, name, 0 };
        e = name ? keep(swi(XWimp_Initialise, w, 4)) : no_room();
        pool_free(&pl);
        if (e)
            return e;
        initialised = 1;
        handle = w[1];
    }

    if ((e = list_dir(r->directory, &l)) == NULL && l.count) {
        if (r->flags & BOOTCMDS_REPEAT_SORT) {
            uint32_t s[8] = { l.count, ros_addr(l.names), 4 };
            e = keep(swi(XOS_HeapSort32, s, 3));
        }
        for (uint32_t i = 0; !e && i < l.count; i++) {
            if (r->flags & BOOTCMDS_REPEAT_PROGRESS && r->range > 0) {
                uint32_t b[8] = { 0, BOOTFX_HANDLE,
                                  (uint32_t)(r->start + (int)((uint32_t)r->range * (i + 1) /
                                                              l.count)) };
                swi(XBootFX_BarUpdate, b, 3);
            }
            uint32_t info = l.names[i] - 24;
            if (!chosen(r, info))
                continue;
            const char *leaf = ros_ptr(l.names[i]);
            if (r->tail)
                snprintf(cmd, sizeof cmd, "%s %s.%s %s", r->command, r->directory, leaf, r->tail);
            else
                snprintf(cmd, sizeof cmd, "%s %s.%s", r->command, r->directory, leaf);

            if (r->flags & BOOTCMDS_REPEAT_TASKS) {
                if (r->flags & BOOTCMDS_REPEAT_VERBOSE) {
                    os_error m = { 0 };
                    snprintf(m.errmess, sizeof m.errmess, "%s", cmd);
                    report(&m, 0x10 | 0x80 | 0x20 | 1u << 9, "information");
                    report(NULL, 0x40, NULL);   /* and then closed */
                }
                struct pool pl = { 0 };
                uint32_t c = pool_str(&pl, cmd);
                uint32_t t[8] = { c };
                e = c ? keep(swi(XWimp_StartTask, t, 1)) : no_room();
                pool_free(&pl);
                continue;
            }

            /* This is what the application's system() did.  X$Error is
             * looked at first, DDEUtils' command line is flushed, and the
             * command is run.  A return code left non-zero fails it too,
             * with the last error that the C library saw.  On RISC OS 5.30
             * that error is DDEUtils_FlushCL's. */
            os_error *x = NULL;
            int unset = !getenv_("X$Error", NULL, 0, &x);
            if (x)
                last = *x, have_last = 1;
            if (r->flags & BOOTCMDS_REPEAT_VERBOSE) {
                write_s("Repeat: ");
                write_line(cmd);
            }
            uint32_t fl[8] = { 0 };
            if ((x = swi(XDDEUtils_FlushCL, fl, 0)) != NULL)
                last = *x, have_last = 1;
            int fails = 0, known = 0;
            if ((x = cli_s(cmd)) != NULL) {
                failed = *x, fails = known = 1;
            } else if (return_code() != 0) {
                failed = last, fails = 1, known = have_last;
            }
            have_last = 0;              /* _kernel_last_oserror() clears it */
            if (!fails)
                continue;
            if (r->flags & BOOTCMDS_REPEAT_CONTINUE) {
                if (unset && known)
                    set_var("X$Error", failed.errmess, strlen(failed.errmess));
                continue;
            }
            if (known)                  /* with none known it stops and says nothing */
                e = ros_error(failed.errnum, "%s", failed.errmess);
            break;
        }
    }
    list_free(&l);
    if (initialised) {
        struct pool pl = { 0 };
        uint32_t w[8] = { handle, TASK };
        os_error *e1 = keep(swi(XWimp_CloseDown, w, 2));
        pool_free(&pl);
        if (!e)
            e = e1;
    }
    return e;
}

/* The application's own ending.  Its error is written, or shown if it ran as
 * a task.  Its return code is left as exit() leaves it, which is 1 after an
 * error.  The error "Return code too large" is given if the code is past
 * Sys$RCLimit. */
static os_error *repeat_end(const os_error *e, int tasks)
{
    char text[256];
    if (e) {
        snprintf(text, sizeof text, "Repeat: %s", e->errmess);
        if (tasks)
            report(e, 0, NULL);
        else
            write_line(text);
    }
    if (e) {
        char limit[32];
        long rcl = getenv_("Sys$RCLimit", limit, sizeof limit, NULL) ? atol(limit) : 256;
        if (rcl < 1)
            return ros_error(ERR_BAD_RETURN_CODE, "Return code too large");
    }
    return set_var("Sys$ReturnCode", e ? "1" : "0", 1);
}

static os_error *cmd_repeat(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct pool pl = { 0 };
    uint32_t *a;
    struct bootcmds_repeat r = { 0 };
    os_error *e = read_args(&pl, "command/a,directory/a,directories/s,applications/s,files/s,"
                                 "type/k,tail,tasks/s,verbose/s,sort/s,continue=stb/s,progress/k",
                            tail, &a);
    if (!e) {
        r.command = item(a[0]);
        r.directory = item(a[1]);
        r.tail = item(a[6]);
        r.flags = (a[2] ? BOOTCMDS_REPEAT_DIRECTORIES : 0) |
                  (a[3] ? BOOTCMDS_REPEAT_APPLICATIONS : 0) | (a[4] ? BOOTCMDS_REPEAT_FILES : 0) |
                  (a[7] ? BOOTCMDS_REPEAT_TASKS : 0) | (a[8] ? BOOTCMDS_REPEAT_VERBOSE : 0) |
                  (a[9] ? BOOTCMDS_REPEAT_SORT : 0) | (a[10] ? BOOTCMDS_REPEAT_CONTINUE : 0);
        if (!(r.flags & (BOOTCMDS_REPEAT_DIRECTORIES | BOOTCMDS_REPEAT_APPLICATIONS |
                         BOOTCMDS_REPEAT_FILES)) && !a[5])
            r.flags |= BOOTCMDS_REPEAT_FILES | BOOTCMDS_REPEAT_DIRECTORIES;
        if (a[5]) {
            uint32_t t[8] = { 31, a[5] };
            e = keep(swi(XOS_FSControl, t, 2));
            r.flags |= BOOTCMDS_REPEAT_TYPE;
            r.type = t[2];
        }
        unsigned start, range;
        if (!e && a[11] && sscanf(item(a[11]), "%u,%u", &start, &range) == 2) {
            if (start > 99)
                start = 99;
            if (start + range > 100)
                range = 100 - start;
            r.start = (int)start, r.range = (int)range;
            r.flags |= BOOTCMDS_REPEAT_PROGRESS;
        }
        if (!e)
            e = bootcmds_repeat(&r);
    }
    os_error *x = repeat_end(e, (r.flags & BOOTCMDS_REPEAT_TASKS) != 0);
    pool_free(&pl);
    return x;
}

static os_error *cmd_add_app(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    struct pool pl = { 0 };
    uint32_t *items;
    os_error *e = read_args(&pl, "applications/a", tail, &items);
    char spec[BUF];
    if (!e) {
        snprintf(spec, sizeof spec, "%s", item(items[0]));
        e = bootcmds_add_app(spec);
    }
    pool_free(&pl);
    return e;
}

/* ---- the module ------------------------------------------------------------------ */

/* Help and syntax as the module's Messages file has them (CmdHelp).  The
 * kernel pretty prints them.  NL is 27 1, the new line of its dictionary. */
#define NL "\x1B\x01"
#define C(n, min, max, gstrans, syntax, help, fn) \
    { n, ROS_CMD_INFO(min, max, gstrans, 0), "Syntax:\t*\x1B\x00" syntax, help, fn }

static const struct ros_command commands[] = {
    C("AddApp", 1, 1, 1, " <application>",
      "*AddApp creates a link from the Resources icon to an application", cmd_add_app),
    C("AppSize", 1, 1, 0, " <size>", "*AppSize reserves space in application workspace",
      cmd_app_size),
    C("Do", 0, 255, 0, " <command>", "*Do passes its argument to the command interpreter",
      cmd_do),
    C("IfThere", 1, 255, 1, " <file> then <command> else <command>",
      "*IfThere looks for a file and executes a choice of commands", cmd_if_there),
    C("LoadCMOS", 1, 1, 1, " <file>",
      "*LoadCMOS configures the computer from a configuration file", cmd_load_cmos),
    C("SaveCMOS", 1, 1, 1, " <file>",
      "*SaveCMOS saves the computer's configuration to a configuration file", cmd_save_cmos),
    C("Repeat", 0, 255, 2,
      " <command> <directory> [-directories | -files | -applications | -type <type>] <tail> "
      "[-tasks] [-verbose] [-sort] [-continue]",
      "*Repeat iterates over a directory, performing a command for each object found" NL
      "Options:" NL "directories\tlimit search to directories" NL
      "files\t\tlimit search to files" NL "applications\tlimit search to applications" NL
      "type <type>\tlimit search to files of a given type" NL
      "tasks\t\tstart each command as a separate task" NL
      "verbose\t\tshow each command before it is executed" NL
      "sort\t\tenumerate directories in ascending ASCII order" NL
      "continue\tput first error into X$Error then carry on (except when -tasks)",
      cmd_repeat),
    C("SafeLogon", 1, 255, 0,
      " [[:]<station number>|:<File server name>] <user name> [[:<CR>]<Password>]",
      "*SafeLogon initialises the current (or given) file server for your use, except that "
      "if you are already logged on, it does nothing", cmd_safe_logon),
    C("AppPath", 2, 2, 2, " <variable> <path element>",
      "*AppPath appends a path element to a path variable, ensuring there are no duplicates",
      cmd_app_path),
    C("PrepPath", 2, 2, 2, " <variable> <path element>",
      "*PrepPath prepends a path element to a path variable, ensuring there are no duplicates",
      cmd_prep_path),
    C("RemPath", 2, 2, 2, " <variable> <path element>",
      "*RemPath removes a path element from a path variable", cmd_rem_path),
    C("Canonical", 1, 1, 0, " <variable>",
      "*Canonical replaces a system variable with the canonicalised version of itself",
      cmd_canonical),
    C("FreePool", 0, 0, 0, "",
      "*FreePool moves all available memory except for the next slot into the free pool",
      cmd_free_pool),
    C("ShrinkRMA", 0, 0, 0, "",
      "*ShrinkRMA will try to shrink the relocatable module area to its minimum size",
      cmd_shrink_rma),
    C("AddToRMA", 1, 1, 0, " <size>", "*AddToRMA adds free space onto the end of the RMA",
      cmd_add_to_rma),
    C("AppSlot", 1, 1, 0, " <size>",
      "*AppSlot tries to achieve the specified application space size", cmd_app_slot),
    C("X", 0, 255, 0, " <command>",
      "*X passes its argument to the command interpreter, storing any error in system "
      "variable X$Error (if not already set)", cmd_x),
    { 0 },
};

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
    struct workspace *w = ws();
    if (w)
        ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

struct ros_module bootcmds_module = {
    .title = "BootCommands",
    .help = "Boot Commands\t1.54 (28 Aug 2024)",
    .init = init,
    .final = final,
    .commands = commands,
};
