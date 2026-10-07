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

/* Title: c.event
 * Purpose: central processing for window sytem events.
 * History: IDJ: 06-Feb-92: prepared for source release
 *
 */

#define BOOL int
#define TRUE 1
#define FALSE 0

#include <stdlib.h>

#include "trace.h"
#include "os.h"
#include "wimp.h"
#include "wimpt.h"
#include "win.h"
#include "menu.h"
#include "alarm.h"

#include "event.h"


/* --------- masking of events ---------- */

static wimp_emask event__mask = wimp_EMNULL;

void event_setmask (wimp_emask mask)
{
  event__mask = mask;
}


wimp_emask event_getmask (void)
{
  return (event__mask);
}


/* -------- attaching menus. -------- */

/* An event_w is in fact a wimp_w. */

typedef struct {
  menu m;
  event_menu_maker maker;
  event_menu_proc event;
  event_menu_ws_proc event_ws_proc;
  void *handle;
} mstr;

static BOOL event__attach(_HwndRef_ HOST_WND w /*window_handle*/,
                          int i /*icon_handle*/,
                          menu m,
                          event_menu_maker menumaker,
                          event_menu_proc eventproc,
                          event_menu_ws_proc event_ws_proc,
                          void *handle)
{
  mstr *p = winx_menu_get_handle(w, i);
  if (m == 0 && menumaker == 0)
  {
    /* cancelling */
    if (p != 0)
    { /* something to cancel */
      free(p);
      winx_menu_set_handle(w, i, NULL);
    }
  }
  else
  {
    if (p == 0) p = malloc(sizeof(mstr));
    if (p == 0) return FALSE;
    p->m = m;
    p->maker = menumaker;
    p->event = eventproc;
    p->event_ws_proc = event_ws_proc;
    p->handle = handle;
    winx_menu_set_handle(w, i, p);
  }
  return TRUE;
}

#ifndef SKS_ACW

BOOL event_attachmenu(event_w w, menu m, event_menu_proc eventproc, void* handle)
{
  return event__attach(w, m, 0, eventproc, handle);
}

BOOL event_attachmenumaker(event_w w, event_menu_maker menumaker, event_menu_proc eventproc, void *handle)
{
  return event__attach(w, 0, menumaker, eventproc, handle);
}

#else /* SKS_ACW */

BOOL event_attachmenu(event_w w, menu m, event_menu_proc eventproc, void *handle)
{
  return event__attach(w, -1, m, NULL, eventproc, NULL, handle);
}

BOOL event_attachmenumaker(event_w w, event_menu_maker menumaker, event_menu_proc eventproc, void *handle)
{
  return event__attach(w, -1, NULL, menumaker, eventproc, NULL, handle);
}

BOOL
event_attachmenumaker_to_w_i(
    _HwndRef_   HOST_WND window_handle,
    _InVal_     int icon_handle,
    event_menu_maker menumaker,
    event_menu_ws_proc event_ws_proc,
    void * handle)
{
    return event__attach((event_w) window_handle, icon_handle, NULL, menumaker, NULL, event_ws_proc, handle);
}

#endif /* SKS_ACW */

/* -------- Processing Events. -------- */

#ifdef SKS_ACW

static menu event__current_menu;

#else /* NOT SKS_ACW */

static wimp_w event__current_menu_window = 0;
  /* 0 if no menu currently visible */
static menu event__current_menu;
static BOOL event__current_destroy_after; /* set if we made the menu */
static int event__menux = 0;
static int event__menuy = 0;
static BOOL event__last_was_menu_hit = FALSE;
static BOOL event__recreation;

#endif /* SKS_ACW */

static BOOL event__recreation;

#ifdef SKS_ACW

static void event__createmenu(BOOL recreate)
{
    mstr *          p = NULL;
    wimp_menuhdr *  m;
    wimp_menuitem * mi;

    if(event_statics.menuclick.window_handle != win_ICONBAR)
        p = winx_menu_get_handle(event_statics.menuclick.window_handle, event_statics.menuclick.icon_handle);

    if(!p)
        p = win_getmenuh(event_statics.menuclick.window_handle);

    if(p)
        {
        event__recreation = recreate;
        event__current_menu = p->m;
        if(p->m)
            m = (wimp_menuhdr *) menu_syshandle(p->m);
        else if(!TRACE  ||  p->maker)
            {
            event__current_menu = p->maker(p->handle);
            m = (wimp_menuhdr *) menu_syshandle(event__current_menu);
            }
        else
            {
            werr(FALSE, "registered menu with no means of creation!!!\n");
            m = NULL;
            }

        /* allow icon bar recreates not to restore menu position */
        if((event_statics.menuclick.window_handle == win_ICONBAR)  &&  !recreate)
            {
            /* move icon bar menus up to standard position. */
            mi = (wimp_menuitem *) (m + 1);

            event_statics.menuclick.y = 96;

            tracef0("positioning icon bar menu.\n");
            do  {
                event_statics.menuclick.y += m->height + m->gap;
                }
            while(!((mi++)->flags & wimp_MLAST));
            }

        (void) wimpt_complain(event_create_menu((wimp_menustr *) m, event_statics.menuclick.x - 64, event_statics.menuclick.y));
        }
    else
        tracef0("no registered menu\n");
}

/*
SKS: process event: returns true if idle
*/

#define event__process event_do_process

#endif /* SKS_ACW */

extern BOOL
event__process(wimp_eventstr * e)
{
  wimp_msgstr *m = &e->data.msg;
  wimp_mousestr ms;
  char hit[20];
  int i;
  BOOL submenu_fake_hit = FALSE;

  tracef1("event__process(%s)\n", report_wimp_event(e->e, &e->data));

  /* Look for submenu requests, and if found turn them into menu hits. */
  /* People wishing to respond can pick up the original from wimpt. */
  if (e->e == wimp_ESEND && e->data.msg.hdr.action == wimp_MMENUWARN)
  {
    /* A hit on the submenu pointer may be interpreted as slightly different
     * from actually clicking on the menu entry with the arrow. The most
     * important example of this is the standard "save" menu item, where clicking
     * on "Save" causes a save immediately while following the arrow gets
     * the standard save dbox up.
    */
    tracef0("hit on submenu => pointer, fake menu hit\n");

    /* cache submenu opening info for use later on this event */
    event_statics.submenu.m = m->data.menuwarn.submenu;
    event_statics.submenu.x = m->data.menuwarn.x;
    event_statics.submenu.y = m->data.menuwarn.y;

    e->e = wimp_EMENU;
    i = 0;
    do  {
        e->data.menu[i] = m->data.menuwarn.menu[i];
    } while(e->data.menu[i++] != -1);

    submenu_fake_hit = TRUE;
  }

  /* Look for menu events */
  if (e->e == Wimp_EMouseClick)
  {
    HOST_WND window_handle = e->data.but.m.w;
    int icon_handle = e->data.but.m.i;
    mstr * p = NULL;

    if(window_handle < 0)
        window_handle = win_ICONBAR;

    /* menu on an icon can give a drag as the pointer leaves the window!
     * so Mr. Paranoid masks out all unreasonable button hits
    */
    if(e->data.but.m.bbits & (wimp_BLEFT | wimp_BMID /*SKS 29sep96 | wimp_BRIGHT*/))
    {
        if(e->data.but.m.bbits & wimp_BMID)
        {
            /* don't use Menu to get menus for registered to icons; fake work area */
            icon_handle = -1;

            if(!p)
                p = win_getmenuh(window_handle);
        }
        else if(icon_handle != -1)
            /* look for menu registered to that icon */
            p = winx_menu_get_handle(window_handle, icon_handle);
    }

    if(p)
    {
      event_statics.menuclick.window_handle = window_handle;
      event_statics.menuclick.icon_handle = icon_handle;
      event_statics.menuclick.x = e->data.but.m.x;
      event_statics.menuclick.y = e->data.but.m.y;
      event_statics.menuclick.valid = TRUE;
      event__createmenu(FALSE);
      event_statics.menuclick.valid = FALSE;
      tracef0("menu created\n");
      return(FALSE);
    }
  }


  if((e->e == wimp_EMENU)  &&  event__current_menu != 0)
  {
    /* Menu hit */
    mstr * p = NULL;

    if(event_statics.menuclick.window_handle != win_ICONBAR)
       p = winx_menu_get_handle(event_statics.menuclick.window_handle, event_statics.menuclick.icon_handle);

    if(!p)
       p = win_getmenuh(event_statics.menuclick.window_handle);

    tracef0("menu hit ");

    if(p)
    {
      if(!submenu_fake_hit)
      {
        
        event_statics.recreatepending = winx_adjustclicked();
      }
      else
      {
        /* say the submenu opening cache is valid */
        event_statics.submenu.valid = TRUE;
        event_statics.recreatepending = FALSE;
      }

      /* form array of menu hits ending in 0 */
      i = 0;
      do  {
          hit[i] = e->data.menu[i] + 1; /* convert hit on 0 to 1 etc. */
          tracef1("[%d]", hit[i]);
      } while(e->data.menu[i++] != -1);

      tracef1(", Adjust = %s\n", report_boolstring(event_statics.recreatepending));

      /* allow access to initial click cache during handler */
      event_statics.menuclick.valid = TRUE;

      if(p->event_ws_proc)
      {
        if(! (* p->event_ws_proc) (p->handle, hit, submenu_fake_hit))
        {
          if(submenu_fake_hit)
          { /* handle unprocessed submenu open events */
            int x, y;
            event_read_submenupos(&x, &y);
            (void) wimpt_complain(event_create_submenu(event_read_submenudata(), x, y));
          }
        }
      }
      else
      {
        (* p->event) (p->handle, hit);

        if(submenu_fake_hit)
        { /* handle unprocessed submenu open events */
          int x, y;
          event_read_submenupos(&x, &y);
          (void) wimpt_complain(event_create_submenu(event_read_submenudata(), x, y));
        }
      }

      /* submenu opening cache no longer valid */
      event_statics.submenu.valid = FALSE;

      if(event_statics.recreatepending)
      {
        /* Twas an ADJ-hit on a menu item.
         * The menu should be recreated.
        */
        tracef0("menu hit caused by Adjust - recreating menu\n");
        event_statics.menuclick.valid = TRUE;
        event__createmenu(TRUE);
      }
#if TRACE
      else if(submenu_fake_hit)
        tracef0("menu hit was faked\n");
      else
        tracef0("menu hit caused by Select - let tree collapse\n");
#endif

      /* initial click cache no longer valid */
      event_statics.menuclick.valid = FALSE;

      return(FALSE);
    }
    else
      tracef0("- no registered handler\n");
  }


#ifndef SKS_ACW
  if (e->e == wimp_ENULL)
  {
    int dummy_time;
    BOOL pending_alarm = alarm_next(&dummy_time);
    tracef0("Got a null\n");
    if (pending_alarm != 0 && dummy_time <= alarm_timenow())
    {
      tracef1("Calling alarm at %d\n", alarm_timenow());
      alarm_callnext();
    }

    if ((event_getmask() & wimp_EMNULL) != 0)
       return TRUE;
  }
#endif /* SKS_ACW */

  /* now process the event */
  if (win_processevent(e))
  {
    /* all is well, it was claimed */
  }
#ifndef SKS_ACW
  else if (e->e == wimp_ENULL)
  {
    /* machine idle: say so */
    return TRUE;
  }
  else if (e->e == wimp_EOPEN)
  {
    /* Assume it's a menu being moved */
    wimpt_complain(wimp_open_wind(&e->data.o));
  }
#else /* SKS_ACW */
  else
  {
    return(event__default_process(e->e, (WimpPollBlock *) &e->data));
  }
#endif /* SKS_ACW */

  return FALSE;
}

static void event__poll(wimp_emask mask, wimp_eventstr *result)
{
#ifndef SKS_ACW

  if (event__last_was_menu_hit && wimpt_last_event()->e == wimp_EMENU)
  {
    wimp_mousestr m;
    event__last_was_menu_hit = FALSE;
    wimpt_noerr(wimp_get_point_info(&m));
    if (0 != (wimp_BRIGHT & m.bbits))
    {
      /* An ADJ-hit. The menu should be recreated. */
      mstr *p = win_getmenuh(event__current_menu_window);
      if (p != 0)
      {
        wimp_menustr *m;
        event__current_menu = p->m;
        event__current_destroy_after = FALSE;
        if (p->m != 0)
        {
          m = (wimp_menustr*) menu_syshandle(p->m);
        } else if (p->maker != 0) {
          event__recreation = 1 ;
          event__current_menu = p->maker(p->handle);
          m = (wimp_menustr*) menu_syshandle(event__current_menu);
        }
        else
        {
          m = (wimp_menustr *) -1;
        }
        wimpt_complain(wimp_create_menu(m, event__menux, event__menuy));
      }
    }
  }
#endif /* SKS_ACW */

  tracef0("doing poll.\n");
  wimpt_complain(wimpt_poll(mask, result));
  tracef0("poll done.\n");
}

void event_process(void)
{
  tracef0("event_process.\n");
#ifndef SKS_ACW
  if (win_activeno() == 0) exit(0); /* stop program */
#endif
  {
    wimp_eventstr e;
    event__poll(event_getmask(), &e);
    (void) event__process(&e);
  }
}

#ifndef SKS_ACW

#ifndef UROM
BOOL event_anywindows()
{
  return(win_activeno() != 0);
}
#endif

#endif /* SKS_ACW */

void event_clear_current_menu(void) {
#ifdef SKS_ACW
  /* NB not wimpt_noerr(), and use our new function */
  (void) wimpt_complain(event_create_menu((wimp_menustr *) -1, 0, 0));
#else /* NOT SKS_ACW */
  wimpt_noerr(wimp_create_menu((wimp_menustr*) -1, 0, 0));
#endif /* SKS_ACW */
}

BOOL event_is_menu_being_recreated(void)
{
 return event__recreation;
}

/* end */
