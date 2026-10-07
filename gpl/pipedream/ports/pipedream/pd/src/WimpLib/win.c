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

/* Title: c.win
 * Purpose: central control of window sytem events.
 * History: IDJ: 07-Feb-92: prepared for source release
 *
 */

/*
 *   03-Apr-89 SKS made window allocation stuff dynamic
 *   07-Jun-91 SKS added pane handling facilities
 *   27-Mar-92 SKS after PD 4.12 changed BAD_WIMP_Ws to 0
*/

#include "include.h" /* for SKS_ACW */

#define BOOL int
#define TRUE 1
#define FALSE 0

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "trace.h"
#include "os.h"
#include "wimp.h"
#include "werr.h"
#include "wimpt.h"
#include "event.h"
#include "msgs.h"
#include "VerIntern/messages.h"

#include "win.h"

/* -------- Keeping Track of All Windows. -------- */

#ifndef WIN_WINDOW_LIST
/* At the moment, a simple linear array of all windows. */

typedef struct {
  wimp_w w;
  win_event_handler proc;
  void *handle;
  void *menuh;
} win__str;

#define MAXWINDOWS (32)
#define DUD (-1)

static win__str *win__allwindows; /*[MAXWINDOWS]*/
static int win__lim = 0; /* first unused index of win__allwindows */
#else
#define DUD (-1)
#endif /* WIN_WINDOW_LIST */

static void win__register(wimp_w w, win_event_handler proc, void *handle)
{
#ifndef WIN_WINDOW_LIST
  int i;
  for (i=0;; i++) {
    if (i == win__lim)
    {
       win__lim++;
       if (win__lim % MAXWINDOWS == 0)
       {  win__str *tmp_windows;
          if ((tmp_windows = realloc(win__allwindows, (win__lim+MAXWINDOWS)*sizeof(win__str))) == 0)
          {
             werr(0, msgs_lookup(MSGS_win1));
             /* return here to avoid trampling past end of win__allwindows */
             win__lim--;
             return;
          }
          win__allwindows = tmp_windows;
       }
       break;
    }

    if (win__allwindows[i].w == w) {break;}; /* found prev definition */
    if (win__allwindows[i].w == DUD) {break;}; /* found hole */
  }
  win__allwindows[i].w = w;
  win__allwindows[i].proc = proc;
  win__allwindows[i].handle = handle;
  win__allwindows[i].menuh = 0;
#else
  winx__register_new(w, (winx_new_event_handler) proc, handle, 0 /* for old binaries */);
#endif /* WIN_WINDOW_LIST */
}

static void win__discard(wimp_w w)
{
#ifndef WIN_WINDOW_LIST
  int i;
  for (i=0; i<win__lim; i++) {
    if (win__allwindows[i].w == w) {
      win__allwindows[i].w = DUD;
      break;
    };
  }
  while (win__lim > 0 && win__allwindows[win__lim-1].w == DUD) {
    /* decrease the limit if you can. */
    win__lim--;
  };
#else
  win__str *pp = (win__str *) &win__window_list;
  win__str *cp;

  while ((cp = pp->link) != NULL) {
    if (cp->window_handle != w) {
      pp = cp;
      continue;
    }

    myassert1x(cp->children == NULL, "win__deregister(&%p): window still has children", w);
    pp->link = cp->link;
    free(cp);
    return;
  }
#endif /* WIN_WINDOW_LIST */
}

static win__str *win__find(wimp_w w)
{
#ifndef WIN_WINDOW_LIST
  int i;
  for (i=0; i<win__lim; i++) {
    if (win__allwindows[i].w == w) {
      return(&win__allwindows[i]);
    };
  };
#else
  win__str *p = (win__str *) &win__window_list;
  while ((p = p->link) != NULL) {
    if (p->window_handle == w) {
       return(p);
    }
  }
#endif /* WIN_WINDOW_LIST */
  return(0);
}

/* -------- Claiming Events. -------- */

void win_register_event_handler(
  wimp_w w, win_event_handler proc, void *handle)
{
  if (proc == 0) {
    win__discard(w);
  } else {
    win__register(w, proc, handle);
  }
}

typedef struct unknown_previewer
{
 struct unknown_previewer *link ;
 win_unknown_event_processor proc ;
 void *handle ;
} unknown_previewer ;

static wimp_w win__idle = win_IDLE_OFF;
#define win__unknown_flag ((wimp_w) (('U'<<24)+('N'<<16)+('K'<<8)+'N'))

static wimp_w win__unknown = DUD;
static unknown_previewer *win__unknown_previewer_list = 0 ;

void win_claim_idle_events(wimp_w w)
{
  tracef1("idle events -> %d\n",w) ;
  win__idle = w;
}

wimp_w win_idle_event_claimer(void)
{
  return(win__idle);
}

void win_claim_unknown_events(wimp_w w)
{
  win__unknown = w;
}

wimp_w win_unknown_event_claimer(void)
{
  return win__unknown;
}

void win_add_unknown_event_processor(win_unknown_event_processor p, void *h)
{
 unknown_previewer *block, *b;

 /* first check to see if fn already there */
 b = win__unknown_previewer_list ;
 while (b != 0)
 {
    if (b->proc == p && b->handle == h) return;
    b = b->link;
 }

 block = malloc(sizeof(unknown_previewer));
 if (block != 0)
 {
  block->link = win__unknown_previewer_list ;
  block->proc = p ;
  block->handle = h ;
  win__unknown_previewer_list = block ;
 }
}

void win_remove_unknown_event_processor(win_unknown_event_processor p,
                                        void * h)
{
   unknown_previewer *b, **pb;

   pb = &win__unknown_previewer_list ;

   while ((b = *pb) != 0)
   {
      if (b->proc == p && b->handle == h)
      {
         *pb = b->link;
         free(b);
         return;
      }
      pb = &(b->link);
   }
}

/* -------- Menus. -------- */

void win_setmenuh(wimp_w w, void *handle)
{
  win__str *p = win__find(w);
  if (p != 0) {p->menuh = handle;}
}

void *win_getmenuh(wimp_w w) /* 0 if not set */
{
  win__str *p = win__find(w);
  return(p==0 ? 0 : p->menuh);
}

/* -------- Processing Events. -------- */

BOOL win_processevent(wimp_eventstr *e)
{
  wimp_w w;
  win__str *p;

  if(e->e != Wimp_ERedrawWindow)
  {
    /* note that the Window Manager cannot cope with any window opening etc issued
     * between the receipt of the Wimp_ERedrawWindow and the SWI Wimp_RedrawWindow
    */
    if(winx_statics.submenu_window_handle != 0)
    {
      if(winx_submenu_query_closed())
      {
        /* Window Manager must have gone and closed the window the sly rat */
        /* so send our client a close request NOW */
        wimp_eventstr ev;

        ev.e = wimp_ECLOSE;
        ev.data.o.w = winx_statics.submenu_window_handle;

        winx_statics.submenu_window_handle = 0;

        (void) win_processevent(&ev);
      }
    }
  }

  switch(e->e) {

    case wimp_ENULL:
      w = win__idle;
      break;

    case wimp_EUSERDRAG:
      w = win__unknown_flag ;
      if(winx_statics.drag_window_handle != 0)
      { /* user drag comes but once per winx_drag_box */
        w = winx_statics.drag_window_handle;
        winx_statics.drag_window_handle = 0;
      }
      break;

    case wimp_EREDRAW: case wimp_ECLOSE: case wimp_EOPEN:
    case wimp_EPTRLEAVE: case wimp_EPTRENTER: case wimp_EKEY:
    case wimp_ESCROLL:
    case wimp_EGAINCARET:
    case wimp_ELOSECARET:
      w = e->data.o.w;
      break;

    case wimp_EBUT:
      w = e->data.but.m.w;
      if (w <= (wimp_w) -1) w = win_ICONBAR;
      break;

    case wimp_ESEND:
    case wimp_ESENDWANTACK:
    /* Some standard messages we understand, and give to the right guy. */
      switch (e->data.msg.hdr.action) {
        case wimp_MDATALOAD:
        case wimp_MDATASAVE:
          tracef1("data %s message arriving.\n",
             (int) (e->data.msg.hdr.action == wimp_MDATASAVE ? "save" : "load"));

          /* Note that we're assuming the window handle's the same position in
             both messages */
          if (e->data.msg.data.dataload.w < 0)
          {
            tracef0("data message to the icon bar.\n");
            w = win_ICONBARLOAD ;
          } else {
            w = e->data.msg.data.dataload.w;
          }
          break;

        case wimp_MHELPREQUEST:
          tracef1("help request for window %i.\n", e->data.msg.data.helprequest.m.w);
          w = e->data.msg.data.helprequest.m.w;
          if (w < 0) w = win_ICONBARLOAD;
          break;

        case wimp_MWINDOWINFO:
          w = ((wimp_msgwindowinfo *) &e->data.msg.data)->w;
          break;

        default:
            tracef1("unknown message arriving: %s\n", report_wimp_message(&e->data.msg, FALSE));
            w = win__unknown_flag;
            if (w < 0) w = win_ICONBARLOAD;
            break;
      }
      break;

    default:
      w = win__unknown_flag;
      break;
  }

  if (w==win__unknown_flag)
  {
   unknown_previewer *pr ;
   for (pr = win__unknown_previewer_list; pr != 0; pr = pr->link)
   {
    if (pr->proc(e, pr->handle)) return TRUE ;
   }

   w = win__unknown ;
  }

  p = ((w == DUD) ? 0 : win__find(w));
  if (p != 0) {
      BOOL processed = (* p->proc) (e, p->handle); /* may be garbage result if called an old void-return binary */

      if(!p->new_proc)
        processed = TRUE;

      return(processed);
  } else {
    return FALSE;
  }
}

#ifndef SKS_ACW

/* -------- Termination. -------- */

static int win__active = 0;

void win_activeinc(void)
{
  win__active++;
}

void win_activedec(void)
{
  win__active--;
}

int win_activeno(void)
{
  return win__active;
}

/* -------- Giving away the caret. -------- */

void win_give_away_caret(void)
/* Whatever window is on top, just "open" it at its current position.
This should make it grab the caret, if it is interested. It doesn't really
matter if this routine has no effect, it just means that the user has to
click somewhere. */
{
  int i;
  for (i=0; i<win__lim; i++) {
    if (win__allwindows[i].w != DUD) {/* found a window */
      wimp_wstate s;
      wimp_eventstr e;
      tracef1("get state of window %i.", win__allwindows[i].w);
      (void) wimp_get_wind_state(win__allwindows[i].w, &s);
      tracef2("behind=%i flags=%i.\n", s.o.behind, s.flags);
      if (s.o.behind == DUD && (s.flags & wimp_WOPEN) != 0) {
        /* w is the top window */
        /* if it wants the caret, it will grab it. */
        tracef0("Opening it.\n");
        e.e = wimp_EOPEN;
        e.data.o = s.o;
        wimpt_fake_event(&e);
        break;
      }
    }
  }
}

/* ----------- reading event handler for window ----------- */
/* useful for intercepting events to txt windows */

BOOL win_read_eventhandler(wimp_w w, win_event_handler *p, void **handle)
{
  int i;
  for (i=0; i<win__lim; i++) {
    if (win__allwindows[i].w == w) {
      *p = win__allwindows[i].proc;
      *handle = win__allwindows[i].handle;
      return(TRUE);
    }
  }
  return(FALSE);
}

#endif /* SKS_ACW */

/* ----------- setting a window title ------------ */

void win_settitle(wimp_w w, char *newtitle)
{
  wimp_winfo *winfo;
  char *str;
  size_t count;

  /* SKS - no malloc() needed */
#define MAX_N_ICONS 200
  char winfo_buf[sizeof(wimp_winfo) + MAX_N_ICONS * sizeof(wimp_icon)]; /*BPDM!*/
    winfo = (wimp_winfo *) &winfo_buf;

  /* --- get the window's details --- */
    winfo->w = w;
    if(NULL != wimp_get_wind_info(winfo))
        return;

    myassert1x(winfo->info.nicons <= MAX_N_ICONS, "win_settitle: reading info for %d icons corrupted stack", winfo->info.nicons);

  /* --- put the new title string in the title icon's buffer --- */
    if((winfo->info.titleflags & wimp_INDIRECT) == 0)
    {
        myassert0x(0, "win_settitle: non-indirected title bar icon");
        return;
    }
    str   = winfo->info.title.indirecttext.buffer;
    count = winfo->info.title.indirecttext.bufflen;
    *str  = CH_NULL;
    strncat(str, newtitle, count - 1);

#if 1
    winx_changedtitle(w);
#else
  /* --- invalidate the title bar in absolute coords --- */
    wimp_redrawstr r;
    r.w = (wimp_w) -1;    /* absolute screen coords */
    r.box = winfo->info.box;
    if(winfo->info.flags & wimp_WOPEN) /* only if open! */
    {
        const int dy = wimpt_dy();
        r.box.y0 = r.box.y0 + dy; /* title bar starts one raster up */
        r.box.y1 = r.box.y0 + wimptx_title_height() - 2*dy;
        (void) wimpt_complain(tbl_wimp_force_redraw(r.w, r.box.x0, r.box.y0. r.box.x1, r.box.y1));
    }
#endif

  /* --- free space used to window info --- */
  /* SKS - no free() needed */
}

BOOL win_init(void)
{
#ifndef WIN_WINDOW_LIST
    if (win__allwindows == 0)
    {
        return((win__allwindows = malloc(MAXWINDOWS*sizeof(win__str)))!=0);
    }
    else
#endif
        return TRUE;
}

/* end */
