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

/* Title: c.wimpt
 * Purpose: provides low-level Wimp functionality
 * History: IDJ: 07-Feb-92: prepared for source release
 *          JAB: 22-Apr-92: added wimpt_messages and appended wimpt_init
 *                          accordingly
 *
 */

#define BOOL int
#define TRUE 1
#define FALSE 0

#include <stdarg.h>
#include <swis.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>

#include "akbd.h"
#include "os.h"
#include "bbc.h"
#include "wimp.h"
#include "wimpt.h"
#include "trace.h"
#include "werr.h"
#include "alarm.h"
#include "event.h"
#include "msgs.h"
#include "win.h"
#include "VerIntern/messages.h"

static int wimpt__fake_waiting = 0;
#ifdef SHARED_C_LIBRARY
static wimp_eventstr wimpt__fake_event = {0};
static wimp_eventstr wimpt__last_event = {0};
#else
static wimp_eventstr wimpt__fake_event;
static wimp_eventstr wimpt__last_event;
#endif

os_error * wimpt_poll(wimp_emask mask, wimp_eventstr * result)
{
#ifdef SKS_ACW
  tracef3("\n[wimpt_poll(&%X, &%p): fake_waiting = %s]\n", mask, result, report_boolstring(wimpt__fake_waiting));
  g_host_must_die = FALSE;
#endif /* SKS_ACW */

  if (wimpt__fake_waiting != 0) {
    *result = wimpt__fake_event;
    wimpt__fake_waiting = 0;
#ifndef SKS_ACW
    wimpt__last_event = wimpt__fake_event;
#endif /* SKS_ACW */
    return(0);
  } else {
    os_error *r;

#ifndef SKS_ACW
    int next_alarm_time;
    if (alarm_next(&next_alarm_time) != 0 && ((event_getmask() & wimp_EMNULL))!=0)
    {
      tracef0("Polling idle\n");
      tracef2("Mask = %d   %d\n", mask & ~wimp_EMNULL, event_getmask());
      r = wimp_pollidle(mask & ~wimp_EMNULL, result, next_alarm_time);
    }
    else
#else /* SKS_ACW */
    if(wimptx__atexitproc) /* SKS pragmatic fix - make general in due course */
      wimptx__atexitproc(result);
#endif /* SKS_ACW */

    if(win_idle_event_claimer() != win_IDLE_OFF)
    {
      /* ensure we get null events */
      mask = (wimp_emask) (mask & ~Wimp_Poll_NullMask);
      tracef1("[wimpt_poll must get idle requests: mask := %X]\n", mask);
    }

    for(;;)
    {
      tracef0("Polling busy\n");
      tracef2("[calling wimp_poll_coltsoft(&%X, &%p)]\n", mask, result);
      if(NULL == (r = wimp_poll_coltsoft(mask, &result->data, NULL, (int *) &result->e)))
          break;

      tracef0("--- ERROR returned from wimp_poll()!!! (Ta Neil)\n");
      (void) wimpt_complain(r);
    }

#ifdef SKS_ACW
    /*if(wimptx__atentryproc)*/ /* SKS pragmatic fix - make general in due course */
        /*wimptx__atentryproc(result);*/

    tracef1("[wimpt_poll returns real event %s]\n", report_wimp_event(result->e, &result->data));
#endif /* SKS_ACW */

    wimpt__last_event = *result;
    return(r);
  }
}

void wimpt_fake_event(wimp_eventstr * e)
{
  if (wimpt__fake_waiting == 0) {
    wimpt__fake_waiting = 1;
    wimpt__fake_event = *e;
  } else {
    tracef1("double fake event, event of type %i dropped.\n", e->e);
  }
}

wimp_eventstr *wimpt_last_event(void)
{
  return &wimpt__last_event;
}

int wimpt_last_event_was_a_key(void)
{
  return(wimpt__last_event.e == wimp_EKEY);
}

/* -------- Control of graphics environment -------- */

#ifndef SKS_ACW

static int wimpt__mode = 12;
static int wimpt__dx;
static int wimpt__dy;
static int wimpt__bpp;

#else /* SKS_ACW */

int wimpt__mode  = 12; /* expose this set for fast access via macros */
int wimpt__dx    = 2;
int wimpt__dy    = 4;
int wimpt__bpp   = 1;

#endif /* SKS_ACW */

static int wimpt__read_screen_mode(void)
{
  int x, y;
  (void) os_byte(135, &x, &y);
  return y;
}

BOOL wimpt_checkmode(void) {
  /* txtar was not noticing Medusa mode changes
     because txtar only checks at a redraw event, so if Edit has no
     windows open over two mode changes the info may then be wrong
     (because the pointer to the mode descriptor is unchanged). */

  BOOL changed = FALSE;
  int old = wimpt__mode;
  /* A fairly careful check mainly to help Edit, now that 16bpp and 32bpp modes
  can be different without their mode value changing. */

  wimpt__mode = wimpt__read_screen_mode();
  if (old != wimpt__mode) changed = TRUE;

  old = wimpt__dx;
#ifndef SKS_ACW
  wimpt__dx = 1 << bbc_vduvar(bbc_XEigFactor);
#else /* SKS_ACW - cache EIG and size too */
  wimptx__XEigFactor  = bbc_vduvar(bbc_XEigFactor);
  wimpt__dx = 1 << wimptx__XEigFactor;
  wimptx__display_size_cx = (bbc_vduvar(bbc_XWindLimit) + 1) << wimptx__XEigFactor;
#endif /* SKS_ACW */
  if (old != wimpt__dx) changed = TRUE;

  old = wimpt__dy;
#ifndef SKS_ACW
  wimpt__dy = 1 << bbc_vduvar(bbc_YEigFactor);
#else /* SKS_ACW - cache EIG and size too */
  wimptx__YEigFactor  = bbc_vduvar(bbc_YEigFactor);
  wimpt__dy = 1 << wimptx__YEigFactor;
  wimptx__display_size_cy = (bbc_vduvar(bbc_YWindLimit) + 1) << wimptx__YEigFactor;
#endif /* SKS_ACW */
  if (old != wimpt__dy) changed = TRUE;

  old = wimpt__bpp;
  wimpt__bpp = 1 << bbc_vduvar(bbc_Log2BPP);
  if (old != wimpt__bpp) changed = TRUE;

#ifdef SKS_ACW
  wimptx__title_height   = 40;
  wimptx__hscroll_height = 40;
  wimptx__vscroll_width  = 40;
  if(0) /* <<< test needed here */
  {
    wimptx__title_height   += wimpt__dy;
    wimptx__hscroll_height += wimpt__dy;
    wimptx__vscroll_width  += wimpt__dx;
  }
#endif /* SKS_ACW */

  return changed;
  /* This will now return TRUE if any interesting aspects of the
  window appearance have changed. */
}

#ifndef UROM
void wimpt_forceredraw(void)
{
  wimp_redrawstr r;
  r.w = (wimp_w) -1;
  r.box.x0 = 0;
  r.box.y0 = 0;
  r.box.x1 = (1 + bbc_vduvar(bbc_XWindLimit)) *
             (1 << bbc_vduvar(bbc_XEigFactor));
  r.box.y1 = (1 + bbc_vduvar(bbc_YWindLimit)) *
             (1 << bbc_vduvar(bbc_YEigFactor));
#ifdef SKS_ACW
  (void) tbl_wimp_force_redraw(r.w,
                               r.box.x0, r.box.y0,
                               r.box.x1, r.box.y1);
#else
  (void) wimp_force_redraw(&r);
#endif /* SKS_ACW */
}
#endif

#undef wimpt_mode
#undef wimpt_dx
#undef wimpt_dy
#undef wimpt_bpp

int wimpt_mode(void)
{
  return(wimpt__mode);
}

int wimpt_dx(void)
{
  return(wimpt__dx);
}

int wimpt_dy(void)
{
  return(wimpt__dy);
}

int wimpt_bpp(void)
{
  return(wimpt__bpp);
}

static wimp_t wimpt__task = 0;

static void wimpt__exit(void)
{
  wimpt_complain(wimp_taskclose(wimpt__task));
}

static char *programname = 0;

char *wimpt_programname(void)
{
  return programname;
}

#ifndef SKS_ACW

void wimpt_reporterror(os_error *e, wimp_errflags f)
{
  if (!programname)
      f |= 1 << 4;
  wimp_reporterror(e,f,programname);
}

os_error *wimpt_complain(os_error *e) {
  if (e != 0)
    wimpt_reporterror(e, 0);
  return e;
}

#endif /* SKS_ACW */

typedef void SignalHandler(int);

#ifndef SKS_ACW
static SignalHandler *oldhandler;
#endif /* SKS_ACW */

static void escape_handler(int sig)
{
  sig = sig; /* avoid compiler warning */
  (void) signal(SIGINT, &escape_handler);
}

#ifndef SKS_ACW

static void handler(int signal)
{
  os_error er;

  er.errnum = 0;
  sprintf(
      er.errmess,
      msgs_lookup(MSGS_wimpt1),
      _kernel_last_oserror()->errmess);
  wimpt_reporterror(&er, 0);
  exit(1);
}

static void errhandler(int signal)
{
    wimpt_reporterror((os_error *)_kernel_last_oserror(), 0);
    exit(1);
}

#endif /* SKS_ACW */

static int wimpversion = 0;


void wimpt_wimpversion(int version)
{
  wimpversion = version;
}

static wimp_msgaction *Messages = NULL;

void wimpt_messages(const wimp_msgaction *messages)
{
   /*Sets the list of messages to be passed to the WIMP.*/
    Messages = messages;
}

static int signal_handlers_installed;

void wimpt_install_signal_handlers(void)
{
  if (signal_handlers_installed) return;
  signal(SIGABRT, &handler);
  signal(SIGFPE, &handler);
  signal(SIGILL, &handler);
  signal(SIGINT, &escape_handler);
  signal(SIGSEGV, &handler);
  signal(SIGTERM, &handler);
#ifdef SKS_ACW
  signal(SIGSTAK, &wimptx_internal_stack_overflow_handler);
  signal(SIGUSR1, &handler);
  signal(SIGUSR2, &handler);
#endif /* SKS_ACW */
  signal(SIGOSERROR, &errhandler);
  signal_handlers_installed = 1;
}

int wimpt_init(char *progname)
{
  os_regset r;
  wimp_msgaction quit = wimp_MCLOSEDOWN;

  wimpt_install_signal_handlers();
  programname = progname;
  if (wimpversion == 0) wimpversion = Messages == NULL? 200: 300;
  if (wimpversion == 300 && Messages == NULL) Messages = &quit;

  r.r [0] = wimpversion;
  r.r [1] = *(int *) "TASK";
  r.r [2] = (int) programname;
  r.r [3] = (int) Messages;

  #if TRACE
     {  int i;

        for (i = 0; Messages [i] != wimp_MCLOSEDOWN; i++)
           tracef1 ("message %d wanted\n", Messages [i]);
     }
  #endif

  wimpt_noerr (os_swix (Wimp_Initialise, &r));

  wimpversion = r.r [0];
  wimpt__task = r.r [1];

  wimpt_checkmode();
  atexit(wimpt__exit);
#ifndef SKS_ACW
  if (!win_init()) werr(TRUE, msgs_lookup(MSGS_wimpt3));
#endif /* SKS_ACW */
  return wimpversion;
}

wimp_t wimpt_task (void) {return wimpt__task;}

void wimpt_noerr (os_error *e)
{
  if (e != 0) (void) os_swi1 (OS_GenerateError, (int) e);
}

/* end */
