/* Copyright RISC OS Open Ltd and others
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
 * This file is derived from RISC OS Open's source and from other code.
 * The Apache licence of RISC OS Open's source applies to it.
 */

/* modulewrap.c: the modules for the ROM's C applications, native. They are
 * "!Edit", "!Draw" and "!Paint", and "Filer_Action".
 *
 * RISC OS's ROM build links a C application (BuildSys/Makefiles/CApp) into
 * a module with RISC_OSLib's s/modulewrap. The module is titled "!<Name>".
 * Its help string is "!<Name>", a tab (two for a short name) and
 * "<Module_MajorVersion> (<Module_Date>)". It has one command,
 * *Desktop_<Name>. The command enters the module as the application, with
 * the command's parameters. The application's code stays in the ROM and
 * its statics are in the RMA (the library's -zM model). The ROM !Run in
 * Resources:$.Apps.!<Name> ends with that command. Its lines are
 * "RMEnsure !<Name> 0.00 RMReInit !<Name>", then the WimpSlot that the
 * ROM's code needs, then "Desktop_<Name> %*0".
 *
 * ROSGD's program is an x32 image (roscc link --clib). The ROM carries the
 * image as a file beside the application's resources, as
 * Resources:$.Resources.<Name>.!RunImage (tools/mkromapps.py). The module
 * here has ModuleWrap's shape around that image. It has the same title and
 * help string (tools/mkromapps.py --modwrap makes them from the
 * component's VersionNum). Its command is *Desktop_<Name> [<args>]. The
 * command gives the task the slot that an x32 image needs, which is
 * "WimpSlot -min 640k -max 640k", as in the disc !Run. It then runs the
 * image with the arguments: "Run Resources:$.Resources.<Name>.!RunImage
 * <args>". The command runs the image by its own name, as ModuleWrap
 * enters its own code whatever the variables say. It does not go through
 * <Name>$Path. The <Obey$Dir> of that path is whichever Obey file ran
 * last, so after another application's !Run the wrong directory's
 * !RunImage would run. Keeping the code in ROM and the statics in the RMA
 * would make the ROM !Run's 40K slot right. That is not done yet. Each
 * command line is on the SVC stack, not in the RMA. The program never
 * comes back to this frame when its task ends, and its start flattens
 * that stack.
 *
 * ModuleWrap does several other things. They are not needed here and are
 * not done. It registers the application directory's files with ResourceFS
 * (ResourceFS_RegisterFiles at initialisation, and again on
 * Service_ResourceFSStarting, and it deregisters them at finalisation).
 * Here the ROM's one ResourceFS block holds them. It claims
 * Service_Memory for its own code. It refuses to die while its statics are
 * in use as the application. It makes numbered incarnations (!<Name>%W0
 * ...) when a second copy runs. Here the image runs in the task's own
 * application space, so a second copy is a second task, and killing the
 * module leaves a running copy alone. So the module has no workspace, as
 * ModuleWrap's has none until the application runs. *Modules shows "!Edit"
 * and 00000000, and RMKill and RMReInit work on it freely.
 *
 * The command's help and syntax are ModuleWrap's International_Help
 * tokens, <Name>Help and <Name>Syntax, in the application's Messages. The
 * kernel looks them up in place and pretty prints them up to a NUL. The
 * texts here are what that prints (tools/mkromapps.py --modwrap). For
 * Draw, whose Messages has no NUL there, the texts include the rest of the
 * file, as 5.30's *Help Desktop_Draw shows.
 */
#include <stdio.h>
#include <string.h>

#include "modulewrap.h"
#include "modwrap_draw.h"
#include "modwrap_edit.h"
#include "modwrap_paint.h"
#include "modwrap_fileract.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

/* Runs one command line through OS_CLI. The line is placed on the SVC stack
 * below the SWIs in progress, and what OS_CLI calls builds below the line.
 * Returns the error, or NULL. */
static os_error *cli(const char *line)
{
    uint32_t n = (uint32_t)strlen(line), outer = ros_svc_sp, buf = (outer - n - 8) & ~7u;
    memcpy(ros_ptr(buf), line, n);
    ros_st8(buf + n, 13);
    ros_svc_sp = buf;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = buf;
    ros_swi(&s, XOS_CLI);
    ros_svc_sp = outer;
    return s.v ? ros_ptr(s.r[0]) : NULL;
}

/* *Desktop_<Name> [<args>]: sets the slot, then runs the image with the
 * arguments as the program the task runs. It returns when the program ends. */
static os_error *desktop_app(const char *image, uint32_t slot_k, uint32_t tail)
{
    char line[1024];
    snprintf(line, sizeof line, "WimpSlot -min %uk -max %uk", slot_k, slot_k);
    os_error *e = cli(line);
    if (e)
        return e;
    int n = snprintf(line, sizeof line, "Run %s", image);
    uint32_t p = tail;
    while (ros_ld8(p) == ' ')
        p++;
    if (ros_ld8(p) >= ' ' && n < (int)sizeof line - 1)
        line[n++] = ' ';
    for (uint32_t c; (c = ros_ld8(p)) >= ' ' && n < (int)sizeof line - 1; p++)
        line[n++] = (char)c;
    line[n] = 0;
    return cli(line);
}

static os_error *cmd_desktop_edit(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return desktop_app(MODWRAP_EDIT_IMAGE, MODULEWRAP_EDIT_SLOT_K, tail);
}

static os_error *cmd_desktop_draw(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return desktop_app(MODWRAP_DRAW_IMAGE, MODULEWRAP_DRAW_SLOT_K, tail);
}

static os_error *cmd_desktop_paint(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return desktop_app(MODWRAP_PAINT_IMAGE, MODULEWRAP_PAINT_SLOT_K, tail);
}

/* The command as ModuleWrap's table has it. It takes any number of
 * parameters and none are GSTransed. Its help and syntax are what the
 * kernel prints from the application's Messages (EditHelp and EditSyntax,
 * which are ModuleWrap's International_Help tokens). */
static const struct ros_command edit_commands[] = {
    { "Desktop_" MODWRAP_EDIT_NAME, ROS_CMD_INFO(0, 255, 0, 0), MODWRAP_EDIT_CMDSYNTAX, MODWRAP_EDIT_CMDHELP,
      cmd_desktop_edit },
    { 0 },
};

struct ros_module edit_module = {
    .title = MODWRAP_EDIT_TITLE,
    .help = MODWRAP_EDIT_HELP,
    .commands = edit_commands,
};

/* Draw's (DrawHelp, DrawSyntax) */
static const struct ros_command draw_commands[] = {
    { "Desktop_" MODWRAP_DRAW_NAME, ROS_CMD_INFO(0, 255, 0, 0), MODWRAP_DRAW_CMDSYNTAX, MODWRAP_DRAW_CMDHELP,
      cmd_desktop_draw },
    { 0 },
};

struct ros_module drawapp_module = {
    .title = MODWRAP_DRAW_TITLE,
    .help = MODWRAP_DRAW_HELP,
    .commands = draw_commands,
};

/* Paint's (PaintHelp, PaintSyntax) */
static const struct ros_command paint_commands[] = {
    { "Desktop_" MODWRAP_PAINT_NAME, ROS_CMD_INFO(0, 255, 0, 0), MODWRAP_PAINT_CMDSYNTAX, MODWRAP_PAINT_CMDHELP,
      cmd_desktop_paint },
    { 0 },
};

struct ros_module paintapp_module = {
    .title = MODWRAP_PAINT_TITLE,
    .help = MODWRAP_PAINT_HELP,
    .commands = paint_commands,
};

/* Filer_Action is ModuleWrap's other shape, its FilerAct switch
 * (RISC_OSLib's s/modulewrap and Desktop/FilerAct's s/AppName). The module
 * is titled Filer_Action. Its help string is "Filer_Action", a tab and the
 * version. Its one command, *Filer_Action, is what the Filer's
 * StartActionWindow passes to Wimp_StartTask for each copy, move, delete,
 * count, access, set type, stamp and find. The Filer first looks the
 * module up by that name. If there is no module, the Filer does the
 * operation itself, with *Copy and the other commands in a command window.
 * The task that is started is told what to do by FilerSWIs' messages. The
 * command gives the task its slot and runs the image,
 * Resources:$.Resources.FilerAct.!RunImage. ModuleWrap's "*WimpSlot -min
 * 40k -max 40k" is for the ROM code. The x32 image needs the slot
 * MODULEWRAP_FILERACT_SLOT_K. Each operation is a task in its own
 * application space, as ModuleWrap's numbered incarnations
 * (Filer_Action%W0 ...) are on RISC OS. The command's help and syntax are
 * HFACFAC and SFACFAC in FilerAct's Messages. ModuleWrap's initialisation
 * sets FilerAct$Path to Resources:$.Resources.FilerAct. unless it is
 * already set. The program finds its Messages and Templates (FilerAct:)
 * there. This initialisation does the same. */
static os_error *fileract_init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    size_t nn = strlen(MODWRAP_FILERACT_PATH_VAR), vn = strlen(MODWRAP_FILERACT_PATH);
    char *b = ros_rma_alloc((uint32_t)(nn + vn + 2));
    if (!b)
        return NULL;
    memcpy(b, MODWRAP_FILERACT_PATH_VAR, nn + 1);
    memcpy(b + nn + 1, MODWRAP_FILERACT_PATH, vn + 1);
    struct ros_cpu s;
    ros_cpu_enter(&s);                          /* R2 < 0 asks whether it is set. R2 is 0 if not. */
    s.r[0] = ros_addr(b), s.r[1] = 0, s.r[2] = (uint32_t)-1, s.r[3] = 0, s.r[4] = 3;
    ros_swi(&s, XOS_ReadVarVal);
    if (s.r[2] == 0) {
        ros_cpu_enter(&s);
        s.r[0] = ros_addr(b), s.r[1] = ros_addr(b + nn + 1), s.r[2] = (uint32_t)vn, s.r[3] = 0, s.r[4] = 0;
        ros_swi(&s, XOS_SetVarVal);
    }
    ros_rma_free(b);
    return NULL;                                /* errors are ignored, as in ModuleWrap (CLRV) */
}

static os_error *cmd_filer_action(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    return desktop_app(MODWRAP_FILERACT_IMAGE, MODULEWRAP_FILERACT_SLOT_K, tail);
}

static const struct ros_command fileract_commands[] = {
    { MODWRAP_FILERACT_COMMAND, ROS_CMD_INFO(0, 255, 0, 0), MODWRAP_FILERACT_CMDSYNTAX, MODWRAP_FILERACT_CMDHELP,
      cmd_filer_action },
    { 0 },
};

struct ros_module fileract_module = {
    .title = MODWRAP_FILERACT_TITLE,
    .help = MODWRAP_FILERACT_HELP,
    .init = fileract_init,
    .commands = fileract_commands,
};
