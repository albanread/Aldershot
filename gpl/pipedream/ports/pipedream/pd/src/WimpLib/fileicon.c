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
 */
/************************************************************************/
/* © Acorn Computers Ltd, 1992.                                         */
/*                                                                      */
/* This file forms part of an unsupported source release of RISC_OSLib. */
/*                                                                      */
/* It may be freely used to create executable images for saleable       */
/* products but cannot be sold in source form or as an object library   */
/* without the prior written consent of Acorn Computers Ltd.            */
/*                                                                      */
/* If this file is re-distributed (even if modified) it should retain   */
/* this copyright notice.                                               */
/*                                                                      */
/************************************************************************/

/* Title: c.fileicon
 * Purpose: general display of a file icon in a dialog box.
 * History: IDJ: 06-Feb-92: prepared for source release
 */

#define BOOL int
#define TRUE 1
#define FALSE 0

#include <stdio.h>

#include "os.h"
#include "wimp.h"
#include "wimpt.h"
#include "sprite.h"
#include "trace.h"
#include "cs-fileicon.h"
#include "werr.h"

void fileicon(wimp_w w, wimp_i ii, int filetype)
{
  wimp_icreate i;
  wimp_i iii;

  /* Set up a sprite icon with the icon that represents the relevant file
  type. The user may now drag this icon away, if he so desires. */
  i.w = w;
  wimpt_complain(wimp_get_icon_info(i.w, ii, &i.i));
  wimpt_complain(wimp_delete_icon(w, ii));
  /* the icon must initially be an indirect text icon. */
  i.i.flags &= ~wimp_ITEXT;        /* set not text */
  i.i.flags |= wimp_ISPRITE + wimp_INDIRECT;       /* set sprite */
  fileicon_name(i.i.data.indirectsprite.name, filetype);
  i.i.data.indirectsprite.spritearea = (void*) 1;
  tracef1("sprite name is %s.\n", (int) i.i.data.indirectsprite.name);
  wimp_create_icon(&i, &iii); /* will recreate with same number. */
#if TRACE
  if (iii != ii)
  {
    werr(FALSE, "INTERNAL: fi-1");
  }
#endif
  tracef2("new icon no %i (should be %i).\n", iii, ii);
}

/* end */
