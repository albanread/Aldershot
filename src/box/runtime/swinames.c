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
 * This file is a reimplementation in C of RISC OS Open's Kernel source
 * (Sources/Kernel: s.SWINaming).
 */
/* swinames.c -- OS_SWINumberToString and OS_SWINumberFromString, as the
 * kernel names SWIs (Kernel/s/SWINaming).
 *
 * The kernel's own SWIs are named by its tables: the core ones, the
 * conversions from &D0, the two date conversions at &C0, and OS_WriteI.
 * Every module's SWIs are named by its header: its SWI chunk, prefix and
 * names, or else "<title>_<number>". BASIC's SYS "name" comes through
 * here. Native modules have headers too (runtime/module.c), so they are
 * named in the same way. A compiled module's name-decoding code is not
 * called yet.
 *
 * Both conversions go by the caller's kind (#149). An ARM caller sees the
 * names of a module's ARM shadow where it has one. That includes the
 * shadow's extra SWIs and a chunk that is not its twin's, just as its SWIs
 * by number are seen. A native caller sees the names of the module chain.
 */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"

#define X_BIT 0x20000u

/* System_Swi_Names: OS_WriteC (0) to OS_HeapSort32 */
static const char *const core[] = {
    "WriteC", "WriteS", "Write0", "NewLine", "ReadC", "CLI", "Byte", "Word", "File", "Args",
    "BGet", "BPut", "GBPB", "Find", "ReadLine", "Control", "GetEnv", "Exit", "SetEnv", "IntOn",
    "IntOff", "CallBack", "EnterOS", "BreakPt", "BreakCtrl", "UnusedSWI", "UpdateMEMC",
    "SetCallBack", "Mouse", "Heap", "Module", "Claim", "Release", "ReadUnsigned",
    "GenerateEvent", "ReadVarVal", "SetVarVal", "GSInit", "GSRead", "GSTrans",
    "BinaryToDecimal", "FSControl", "ChangeDynamicArea", "GenerateError", "ReadEscapeState",
    "EvaluateExpression", "SpriteOp", "ReadPalette", "ServiceCall", "ReadVduVariables",
    "ReadPoint", "UpCall", "CallAVector", "ReadModeVariable", "RemoveCursors",
    "RestoreCursors", "SWINumberToString", "SWINumberFromString", "ValidateAddress",
    "CallAfter", "CallEvery", "RemoveTickerEvent", "InstallKeyHandler", "CheckModeValid",
    "ChangeEnvironment", "ClaimScreenMemory", "ReadMonotonicTime", "SubstituteArgs",
    "PrettyPrint", "Plot", "WriteN", "AddToVector", "WriteEnv", "ReadArgs", "ReadRAMFsLimits",
    "ClaimDeviceVector", "ReleaseDeviceVector", "DelinkApplication", "RelinkApplication",
    "HeapSort", "ExitAndDie", "ReadMemMapInfo", "ReadMemMapEntries", "SetMemMapEntries",
    "AddCallBack", "ReadDefaultHandler", "SetECFOrigin", "SerialOp", "ReadSysInfo", "Confirm",
    "ChangedBox", "CRC", "ReadDynamicArea", "PrintChar", "ChangeRedirection",
    "RemoveCallBack", "FindMemMapEntries", "SetColour", "ClaimSWI", "ReleaseSWI", "Pointer",
    "ScreenMode", "DynamicArea", "AbortTrap", "Memory", "ClaimProcessorVector", "Reset",
    "MMUControl", "ResyncTime", "PlatformFeatures", "SynchroniseCodeAreas", "CallASWI",
    "AMBControl", "CallASWIR12", "SpecialControl", "EnterUSR32", "EnterUSR26", "VIDCDivider",
    "NVMemory", "ClaimOSSWI", "TaskControl", "DeviceDriver", "Hardware", "IICOp", "LeaveOS",
    "ReadLine32", "SubstituteArgs32", "HeapSort32", NULL,
};

/* Conversion_Swi_Names, from OS_ConvertHex1 (&D0) */
static const char *const conv[] = {
    "ConvertHex1", "ConvertHex2", "ConvertHex4", "ConvertHex6", "ConvertHex8",
    "ConvertCardinal1", "ConvertCardinal2", "ConvertCardinal3", "ConvertCardinal4",
    "ConvertInteger1", "ConvertInteger2", "ConvertInteger3", "ConvertInteger4",
    "ConvertBinary1", "ConvertBinary2", "ConvertBinary3", "ConvertBinary4",
    "ConvertSpacedCardinal1", "ConvertSpacedCardinal2", "ConvertSpacedCardinal3",
    "ConvertSpacedCardinal4", "ConvertSpacedInteger1", "ConvertSpacedInteger2",
    "ConvertSpacedInteger3", "ConvertSpacedInteger4", "ConvertFixedNetStation",
    "ConvertNetStation", "ConvertFixedFileSize", "ConvertFileSize", "ConvertVariform", NULL,
};

static const char *const dates[] = { "ConvertStandardDateAndTime", "ConvertDateAndTime", NULL };
static const char *const writei[] = { "WriteI", NULL };

#define NCORESWIS (sizeof core / sizeof core[0] - 1)

/* ---- from a string ------------------------------------------------------------------------ */

/* A name ends at a character <= ' ' */
static int end(uint8_t c)
{
    return c <= ' ';
}

/* LookForSwiName: the name at p against a prefix and its names. The mode
 * numeric is 0 for names only (the kernel's), 1 for <prefix>_<decimal>
 * only, and 2 for either. *n gets the index or number. The result is 1 if
 * the name matched. */
static int look(const uint8_t *p, const char *prefix, const char *const *names,
                uint32_t names_at, int numeric, uint32_t *n)
{
    size_t k = 0;
    for (; prefix[k]; k++)
        if (end(p[k]) || p[k] != (uint8_t)prefix[k])
            return 0;
    if (p[k] != '_')
        return 0;
    p += k + 1;
    if (numeric != 1) {
        /* The names are a table of C strings, or the header's in the arena. */
        uint32_t at = names_at;
        for (uint32_t i = 0;; i++) {
            const char *name = names ? names[i] : ros_ptr(at);
            if (!name || !*name)
                break;
            size_t j = 0;
            while (name[j] && p[j] == (uint8_t)name[j])
                j++;
            if (!name[j] && end(p[j])) {
                *n = i;
                return 1;
            }
            at += (uint32_t)strlen(name) + 1;
        }
    }
    if (numeric == 0)
        return 0;
    /* CheckForNumericPostFix: OS_ReadUnsigned in decimal, at most 63.
     * What follows the number is not checked. */
    uint32_t v = 0;
    size_t j = 0;
    while (p[j] >= '0' && p[j] <= '9' && v <= 63)
        v = v * 10 + (uint32_t)(p[j++] - '0');
    if (!j || v > 63)
        return 0;
    *n = v;
    return 1;
}

/* A module's header: its SWI chunk, and the names after its prefix */
static int header_swis(const struct ros_module *m, uint32_t *chunk, uint32_t *table)
{
    if (!m->base)
        return 0;
    uint32_t c = ros_ld32(m->base + 0x1C) & ~X_BIT;
    if (!c || (c & 63) || (c & 0xFF000000u))
        return 0;
    *chunk = c;
    uint32_t off = ros_ld32(m->base + 0x24);
    *table = off ? m->base + off : 0;
    return 1;
}

/* The module whose SWI chunk holds number, for a caller of a kind. This is
 * like ros_module_for_swi_kind, but it never makes SharedCLibrary's
 * built-in shadow just to name a SWI. */
static struct ros_module *for_swi(uint32_t number, int kind)
{
    for (struct ros_module *m = ros_module_first(); m; m = m->next) {
        if (m->swi_chunk && number - m->swi_chunk < 64)
            return ros_module_route_existing(m, kind);
        if (kind == ROS_KIND_ARM && m->shadow && m->shadow->swi_chunk &&
            number - m->shadow->swi_chunk < 64)
            return m->shadow;
    }
    return NULL;
}

static int number_from_string(const char *s, uint32_t *number, int kind)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t x = 0, n;
    if (*p == 'X')
        x = X_BIT, p++;
    if (look(p, "OS", core, 0, 0, &n)) {
        *number = x | n;
        return 1;
    }
    if (look(p, "OS", dates, 0, 0, &n)) {
        *number = x | (0xC0 + n);
        return 1;
    }
    if (look(p, "OS", conv, 0, 0, &n)) {
        *number = x | (0xD0 + n);
        return 1;
    }
    if (look(p, "OS", writei, 0, 0, &n)) {
        *number = x | 0x100;
        return 1;
    }
    for (struct ros_module *c = ros_module_first(); c; c = c->next) {
        struct ros_module *m = ros_module_route_existing(c, kind);
        uint32_t chunk, table;
        if (!header_swis(m, &chunk, &table))
            continue;
        if (table) {
            const char *prefix = ros_ptr(table);
            uint32_t names = table + (uint32_t)strlen(prefix) + 1;
            if (look(p, prefix, NULL, names, 2, &n)) {
                *number = x | chunk | n;
                return 1;
            }
        }
        /* A compiled module's name-decoding code would be asked here. */
        const char *title = ros_ptr(m->base + ros_ld32(m->base + 0x10));
        if (look(p, title, NULL, 0, 1, &n)) {
            *number = x | chunk | n;
            return 1;
        }
    }
    return 0;
}

void ros_thunk_OS_SWINumberFromString(struct ros_cpu *s)
{
    uint32_t n;
    char name[256];
    const uint8_t *p = ros_ptr(s->r[1]);
    size_t k = 0;
    while (k + 1 < sizeof name && p[k] > ' ')
        k++;
    memcpy(name, p, k);
    name[k] = 0;
    if (number_from_string(name, &n, ros_caller_kind())) {
        s->r[0] = n;
        s->v = 0;
    } else {
        ros_swi_fail(s, ros_error(0x1E6, "SWI name not known"));
    }
}

/* ---- to a string ------------------------------------------------------------------------------ */

struct out {
    uint32_t at, n, max;
    int over;
};

static void add_char(struct out *o, char c)            /* AddChar */
{
    if (o->n >= o->max) {
        o->over = 1;
        return;
    }
    ros_st8(o->at + o->n++, (uint8_t)c);
}

static void add(struct out *o, const char *s)          /* AddString */
{
    while (*s)
        add_char(o, *s++);
}

void ros_thunk_OS_SWINumberToString(struct ros_cpu *s)
{
    uint32_t number = s->r[0];
    struct out o = { s->r[1], 0, s->r[2], 0 };
    char num[16];
    if (number & X_BIT)
        add_char(&o, 'X');
    uint32_t n = number & ~X_BIT;
    if (n < 512) {
        add(&o, "OS_");
        if (n >= 256) {
            add(&o, "WriteI+");
            uint32_t c = n & 255;
            if (c < 32 || c >= 127) {
                snprintf(num, sizeof num, "%u", c);
                add(&o, num);
            } else {
                add_char(&o, '"'), add_char(&o, (char)c), add_char(&o, '"');
            }
        } else if (n < NCORESWIS) {
            add(&o, core[n]);
        } else if (n - 0xD0 <= 0xED - 0xD0) {
            add(&o, conv[n - 0xD0]);
        } else {
            add(&o, n == 0xC0 ? dates[0] : n == 0xC1 ? dates[1] : "Undefined");
        }
    } else {
        struct ros_module *m = for_swi(n & ~63u, ros_caller_kind());
        uint32_t chunk, table;
        /* If a shadow is reached by its twin's chunk but has a chunk of
         * its own, the chunk's names are the twin's. */
        if (m && m->twin && header_swis(m, &chunk, &table) && chunk != (n & ~63u))
            m = m->twin;
        if (m && header_swis(m, &chunk, &table) && chunk == (n & ~63u)) {
            const char *name = NULL;
            if (table) {
                const char *prefix = ros_ptr(table);
                add(&o, prefix);
                add_char(&o, '_');
                uint32_t at = table + (uint32_t)strlen(prefix) + 1;
                for (uint32_t i = 0; ros_ld8(at); i++) {
                    const char *e = ros_ptr(at);
                    if (i == (n & 63)) {
                        name = e;
                        break;
                    }
                    at += (uint32_t)strlen(e) + 1;
                }
            } else {
                add(&o, ros_ptr(m->base + ros_ld32(m->base + 0x10)));
                add_char(&o, '_');
            }
            if (name) {
                add(&o, name);
            } else {
                snprintf(num, sizeof num, "%u", n & 63);
                add(&o, num);
            }
        } else {
            add(&o, "User");
        }
    }
    add_char(&o, 0);
    s->r[2] = o.n;
    if (o.over)
        ros_swi_fail(s, ros_error(0x1E4, "Buffer overflow"));
    else
        s->v = 0;
}
