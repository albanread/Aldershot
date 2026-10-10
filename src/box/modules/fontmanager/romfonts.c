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
 * This file is a reimplementation in C of RISC OS Open's ARM assembler source
 * (Sources/Video/Render/Fonts/ROMFonts: s.ROMFonts).
 */

/* romfonts.c: ROMFonts (Video/Render/Fonts/ROMFonts), written natively.
 *
 * The original registers the ROM's fonts with ResourceFS.  Here they are
 * already among the ROM's resources (resources/Fonts), so that step is
 * not needed.  The module then adds them to Font$Path. Unless something
 * has set a path of its own, Font$Path becomes
 * "<Font$Prefix>.,Resources:$.Fonts.".  If Font$Prefix is unset, it is
 * set to a space, so that !Fonts is not looked for.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "fm.h"

static void cli(const char *command)
{
    uint32_t a = ros_addr(ros_rma_alloc((uint32_t)strlen(command) + 1));
    strcpy(ros_ptr(a), command);
    uint32_t r[10] = { a };
    os_error *e;
    fm_swi(XOS_CLI, r, &e);
    ros_rma_free(ros_ptr(a));
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    uint32_t name = ros_addr(ros_rma_alloc(32)), buf = ros_addr(ros_rma_alloc(256));
    strcpy(ros_ptr(name), "Font$Path");
    uint32_t r[10] = { name, buf, 256, 0, 2 };          /* VarType_Macro: read it unexpanded */
    os_error *e;
    fm_swi(XOS_ReadVarVal, r, &e);
    int set = e != NULL;
    if (!e && r[4] == 2) {
        ((char *)ros_ptr(buf))[r[2] < 256 ? r[2] : 255] = 0;
        set = !strcmp(ros_ptr(buf), "<Font$Prefix>.");
    }
    if (set) {
        cli("SetMacro Font$Path <Font$Prefix>.,Resources:$.Fonts.");
        strcpy(ros_ptr(name), "Font$Prefix");
        uint32_t q[10] = { name, 0, 0xFFFFFFFFu, 0, 3 };  /* VarType_Expanded: only to see whether it exists */
        fm_swi(XOS_ReadVarVal, q, &e);
        if (q[2] == 0)
            cli("Set Font$Prefix \" \"");
    }
    ros_rma_free(ros_ptr(name)), ros_rma_free(ros_ptr(buf));
    return NULL;
}

struct ros_module romfonts_module = {
    .title = "ROMFonts",
    .help = "ROM Fonts\t0.78 ROSGD native",
    .init = init,
};
