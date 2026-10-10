/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* scrmodes.c: ScreenModes, a native module. It gives the monitor's name
 * to the clients that ask for it. The Display Manager uses the name in its
 * window title.
 *
 * On RISC OS, ScreenModes reads monitor description files and owns the
 * mode list (Video/UserI/ScrModes). The enumeration of modes and the taking
 * of a mode by OS_ScreenMode are its services. ROSGD folds that work into
 * the runtime. runtime/vdu/modes.c enumerates the display's own list
 * (platform.h). What is left is the module's SWI chunk. The main call is
 * ScreenModes_ReadInfo. Its first reason code returns the monitor's name,
 * which the Display Manager's window title shows. The name is the
 * display's own, that of the DRM connector ("Virtual-1"). A monitor
 * description file's name would play the same part. There are no monitor
 * files, so *LoadModeFile and *SaveModeFile are not here, and Features
 * reports no EDID.
 *
 * The chunk's numbers and register roles are RISC OS's (cmhg/ScrModesv,
 * c/ScrModes). Its errors are its own, from &BE0 up. The workspace keeps
 * the name at an address that can be given to a client. ScreenModes keeps
 * the name in its monitor description in the same way.
 */
#include <stdio.h>
#include <string.h>

#include "drmvideo.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "scrmodes.h"

#define READINFO_MONITORNAME 0u
#define READINFO_DPMS        1u
#define READINFO_SPEAKERMASK 2u

#define ERR_NOMODEFILE  (0xBE0u + 20u)   /* "No monitor description file loaded" */
#define ERR_BADREADINFO (0xBE0u + 22u)   /* "Unknown ScreenModes_ReadInfo call" */

/* The workspace: the monitor's name, at an address a client may keep. */
struct ws {
    char name[sizeof ((struct ros_display *)0)->name];
};

static struct ws *workspace(struct ros_module *m)
{
    uint32_t pw = ros_ld32(m->private_word);
    return (int32_t)pw > 0 ? ros_ptr(pw) : NULL;
}

/* The workspace is claimed when a display is there, and the name is
 * written into it from the display. Without a display there is no monitor,
 * as with ScreenModes when no monitor description file is loaded. DRMVideo
 * opens the display and starts before this module does. */
static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    const struct ros_display *d = drmvideo_display();
    if (!d)
        return NULL;
    struct ws *w;
    if (xos_module_claim(sizeof *w, (void **)&w))
        return NULL;
    snprintf(w->name, sizeof w->name, "%s", d->name);
    ros_st32(m->private_word, ros_addr(w));
    return NULL;
}

static os_error *no_monitor(struct ros_cpu *s)
{
    s->r[0] = ros_addr(ros_error(ERR_NOMODEFILE, "No monitor description file loaded"));
    s->v = 1;
    return NULL;
}

/* ScreenModes_ReadInfo: R0 holds the item wanted on entry and its answer on exit. */
void ros_thunk_ScreenModes_ReadInfo(struct ros_cpu *s)
{
    struct ws *w = workspace(&scrmodes_module);
    s->v = 0;
    switch (s->r[0]) {
    case READINFO_MONITORNAME:
        if (!w) {
            no_monitor(s);
            return;
        }
        s->r[0] = ros_addr(w->name);
        return;
    case READINFO_DPMS:
        if (!w) {
            no_monitor(s);
            return;
        }
        s->r[0] = 0;                      /* a virtual display cannot sleep */
        return;
    case READINFO_SPEAKERMASK:
        if (!w) {
            no_monitor(s);
            return;
        }
        s->r[0] = 0;                      /* no speakers */
        s->r[1] = 0;                      /* none of the bits valid */
        return;
    default:
        s->r[0] = ros_addr(ros_error(ERR_BADREADINFO, "Unknown ScreenModes_ReadInfo call"));
        s->v = 1;
        return;
    }
}

/* No audio formats. The list ends at once, with R1 = -1 and R2 = -1. */
void ros_thunk_ScreenModes_EnumerateAudioFormats(struct ros_cpu *s)
{
    s->v = 0;
    s->r[1] = 0xFFFFFFFFu;
    s->r[2] = 0xFFFFFFFFu;
}

/* There are no features beyond the module being there. In particular there is no EDID. */
os_error *xscreenmodes_features(uint32_t *features)
{
    *features = 0;
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    uint32_t pw = ros_ld32(m->private_word);
    if ((int32_t)pw > 0) {
        xos_module_free(ros_ptr(pw));
        ros_st32(m->private_word, 0);
    }
    return NULL;
}

struct ros_module scrmodes_module = {
    .title = "ScreenModes",
    .help = "ScreenModes\t1.65 (ROSGD native)",
    .init = init,
    .final = final,
    .swi_chunk = 0x487C0,
    .swi_thunks = ros_swi_thunks_ScreenModes,
    .swi_names = ros_swi_names_ScreenModes,
    .swi_prefix = "ScreenModes",
};

/* The SWI count is known only at run time (api_gen.c). This constructor
 * stores it so that the runtime can read it when the module is added. */
__attribute__((constructor)) static void count(void)
{
    scrmodes_module.swi_count = ros_swi_count_ScreenModes;
}
