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

/* Title: > c.dbox
 * Purpose: System-independent dialog boxes.
 * History: IDJ: 05-Feb-92: prepared for source release
 *
 */

#include "include.h" /* for SKS_ACW */

#define BOOL int
#define TRUE 1
#define FALSE 0

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>

#include "trace.h"
#include "werr.h"
#include "os.h"
#include "akbd.h"
#include "wimp.h"
#include "wimpt.h"
#include "win.h"
#include "menu.h"
#include "event.h"
#include "dbox.h"
#include "res.h"
#include "sprite.h"
#include "resspr.h"
#include "template.h"
#include "alarm.h"
#include "msgs.h"
#include "VerIntern/messages.h"

/*   07-Mar-89 SKS: hacked format so I can read it!
 *   22-Mar-89 SKS: added a hook into the window definition
 *   05-Apr-89 SKS: made PDACTION changes for PipeDream
 * SKS 08 Jan 1997 No longer gets in BubbleHelp's way by fronting dialogs
 * SKS 14 Jan 1998 Use correct wimp_IESGMASK definition
*/

#define wimp_IESG               0x00010000

typedef struct dbox__str {
  struct dbox__str *next;  /* if user wants to link dboxes into a list */
  wimp_w w;                /* only used in live dialog boxes */
  int posatcaret;          /* Every time it is shown, it appears "near" the
                            * caret.
                            */
  int showing;
#ifndef SKS_ACW
  wimp_caretstr caretstr;   /* save between fillin's. */
#else
  BOOL caret_set;
#endif /* SKS_ACW */
  dbox_handler_proc eventproc;
  void *eventprochandle;
  dbox_raw_handler_proc raweventproc;
  void *raweventprochandle;

  dbox_field field;     /* button last pressed */
  int fieldwaiting;     /* a button waiting to be picked up */
  int eventdepth;       /* for delaying disposal */
  int disposepending;

#ifndef SKS_ACW
  char name[12];
  char *workspace;
  int workspacesize;
  wimp_wind window;
  /* any icons follow directly after this. */
#else /* SKS_ACW */
    struct _dbox_str_bits
        {
        int motion_updates : 1;
        int sent_close     : 1; /* have we returned dbox_CLOSE before now? */
        }
    bits;

  wimp_wind * p_window;
#endif /* SKS_ACW */
} dbox__str;
/* Abstraction: a dbox is really a dbox__str*. */

#ifdef SKS_ACW

static BOOL note_position = FALSE;

static struct _noted_position
{
    BOOL noted;
    int x;
    int y;
}
noted_position, saved_position;

#endif /* SKS_ACW */

/* -------- Miscellaneous. -------- */

static dbox dbox__fromtemplate(template *from)
{
  dbox to;
#ifndef SKS_ACW
  int j;
  int size = sizeof(dbox__str) + from->window.nicons * sizeof(wimp_icon);

  to = malloc(size);
  if (to == 0) return 0;

  /* --- copy relevant stuff from template --- */
  strncpy (to->name, from->name, 12);
  to->workspacesize = from->workspacesize;
  (void) memcpy(&to->window, &from->window, sizeof(wimp_wind) + from->window.nicons * sizeof(wimp_icon));

  /* --- allocate and copy workspace --- */
  if (to->workspacesize != 0)
  {
    to->workspace = malloc(to->workspacesize);
    if (to->workspace == 0)
    {
      free(to);
      return 0;
    }
    (void) memcpy(to->workspace, from->workspace, to->workspacesize);

    /* -- fix up indirect icon pointers -- */
    for (j=0; j<to->window.nicons; j++)
    {
      wimp_icon *i = ((wimp_icon *)(&to->window + 1)) + j;
      if ((i->flags & wimp_INDIRECT) != 0)
      {
        i->data.indirecttext.buffer += to->workspace - from->workspace;
        if ((i->flags & wimp_ITEXT) != 0 &&
            (int) (i->data.indirecttext.validstring) > 0)
          i->data.indirecttext.validstring += to->workspace - from->workspace;
      }
    }

    /* -- fix up indirect title pointer -- */
    if ((to->window.titleflags & wimp_INDIRECT) != 0)
      to->window.title.indirecttext.buffer += to->workspace - from->workspace;

  }
#else /* SKS_ACW */
  /* SKS for p_window - quite a different implementation used by PipeDream */
  size_t size = sizeof(dbox__str);
  wimp_wind * w;
  wimp_wflags clearbits;

  tracef1("[dbox__fromtemplate, size = %d]\n", size);
  to = calloc(size, 1);
  if (to == 0) return 0;

  /* Make a copy of the given dbox template and its workspace */
  if(NULL == (w = (wimp_wind *) template_copy_new(from)))
  {
    free(to);
    return 0;
  }

  to->p_window = w;

  to->posatcaret = (0 != (w->flags & wimp_WTRESPASS));

  clearbits = wimp_WTRESPASS;

  /* knock out back bits on certain dboxes */
  if(w->colours[wimp_WCSCROLLOUTER] == 12)
      clearbits = (wimp_wflags) (clearbits | wimp_WBACK);

  w->flags = (wimp_wflags) (w->flags & ~clearbits);
#endif /* SKS_ACW */

  return(to);

}


static void dbox__dispose(dbox d)
{
#ifndef SKS_ACW
  if (d->workspacesize != 0) {
    free(d->workspace);
  }
#else /* SKS_ACW */
  /* SKS for p_window */
  if (d->p_window) {
    free(d->p_window);
  }
#endif /* SKS_ACW */
  free(d);
}

/* This is more logically connected with dbox_dispose below, the
 * ordering is dictated by importation of process_wimp_event in dbox_new.
*/
/* The menu is removed just in case any client had registered one */
static void dbox__dodispose(dbox d)
{
  win_register_event_handler(d->w, 0, 0);
  event_attachmenu(d->w, 0, 0, 0);
#ifndef SKS_ACW
  if (d->showing) {
    win_activedec();
  }
  wimpt_noerr(wimp_delete_wind(d->w));
#else /* SKS_ACW */
  {
  HOST_WND window_handle = d->w;
  (void) wimpt_complain(winx_delete_window(&window_handle));
  } /* block */
#endif /* SKS_ACW */
  dbox__dispose(d);
}


/* -------- Finding Icons. -------- */

/* useful icon flag masks, for searching for specific icon types */
#define BUTTON_IFLAGS (15 * wimp_IBTYPE)
#define WRITABLE_IFLAGS (wimp_BWRITABLE * wimp_IBTYPE)
#define CLICK_IFLAGS (wimp_BCLICKDEBOUNCE * wimp_IBTYPE)
#define AUTO_IFLAGS (wimp_BCLICKAUTO * wimp_IBTYPE)
#define RELEASE_IFLAGS (wimp_BSELREL * wimp_IBTYPE)
#define ONOFF_IFLAGS (wimp_BSELDOUBLE * wimp_IBTYPE)
#define ONOFF2_IFLAGS (wimp_BCLICKSEL * wimp_IBTYPE)
#define MENU_IFLAGS (wimp_BSELNOTIFY * wimp_IBTYPE)

static int dbox__findicon(dbox d, wimp_iconflags mask, wimp_iconflags settings, wimp_i *j)
/* Rather like SWI WhichIcon, but only finds the first. Returns 0 if not
found. */
{
#ifndef SKS_ACW
  for (; (*j)<d->window.nicons; (*j)++) {
    wimp_icon *i = ((wimp_icon*) (&d->window + 1)) + *j;
#else /* SKS_ACW */
  /* SKS for p_window */
  const wimp_wind *w = d->p_window;
  for (; (*j)<w->nicons; (*j)++) {
    const wimp_icon *i = ((const wimp_icon *) (w + 1)) + *j;
#endif /* SKS_ACW */
    if ((i->flags & mask) == settings) {
      tracef1("Found icon %i.\n", *j);
      return(1);
    }
  }
  return(0);
}

#ifndef SKS_ACW
static int dbox__findiconbefore(dbox d,
  wimp_iconflags mask, wimp_iconflags settings, wimp_i *j)
/* Does not look at the current icon. */
{
#ifndef SKS_ACW
  while ((*j) != 0) {
    wimp_icon *i = ((wimp_icon*) (&d->window + 1)) + (--(*j));
#else /* SKS_ACW */
  /* SKS for p_window */
  const wimp_wind * w = d->p_window;
  while ((*j) != 0) {
    const wimp_icon *i = ((const wimp_icon *) (w + 1)) + (--(*j));
#endif /* SKS_ACW */
    if ((i->flags & mask) == settings) {
      tracef1("Found icon %i.\n", *j);
      return(1);
    }
  }
  return(0);
}
#endif /* SKS_ACW */

/* -------- Icons and Fields. -------- */

#ifndef SKS_ACW

#ifndef UROM
static dbox_field dbox__icontofield(wimp_i i)
{
  return(i);
}
#endif

#endif /* SKS_ACW */

static wimp_i dbox__fieldtoicon(dbox_field f)
{
  return(f);
}

static wimp_icon *dbox__iconhandletoptr(dbox d, wimp_i i)
{
#ifndef SKS_ACW
  return(((wimp_icon*) (&d->window + 1)) + i);
#else /* SKS_ACW */
  /* SKS for p_window */
  return(((wimp_icon*) (d->p_window + 1)) + i);
#endif /* SKS_ACW */
}

static wimp_icon *dbox__fieldtoiconptr(dbox d, dbox_field f)
{
  return(dbox__iconhandletoptr(d, dbox__fieldtoicon(f)));
}

static wimp_iconflags dbox__ibutflags(wimp_icon *i)
{
#ifndef SKS_ACW
  return(i->flags & BUTTON_IFLAGS);
#else /* SKS_ACW */
  return((wimp_iconflags) (i->flags & BUTTON_IFLAGS)); /* SKS cast for modern warnings */
#endif /* SKS_ACW */
}

static dbox_fieldtype dbox__iconfieldtype(wimp_icon *i)
{
  switch (dbox__ibutflags(i)) {
  case AUTO_IFLAGS:
  case RELEASE_IFLAGS:
  case CLICK_IFLAGS:
  case MENU_IFLAGS:
    return(dbox_FACTION);
  case ONOFF_IFLAGS:
  case ONOFF2_IFLAGS:
    return(dbox_FONOFF);
  case WRITABLE_IFLAGS:
    return(dbox_FINPUT);
  default:
    return(dbox_FOUTPUT);
  }
}

#ifndef SKS_ACW

static BOOL dbox__has_action_button(dbox d)
{
   wimp_i j;

   for (j = 0; j < d->window.nicons; j++)
   {
     wimp_icon *i = ((wimp_icon*) (&d->window + 1)) + j;
     dbox_fieldtype t = dbox__iconfieldtype(i);

     if (t == dbox_FACTION || t == dbox_FONOFF || t == dbox_FINPUT)
        return TRUE;
   }

   return FALSE;
}


static int dbox__min(int a, int b) {if (a<b) {return(a);} else {return(b);}}

#endif /* SKS_ACW */


void dbox_fadefield (dbox d, dbox_field f)
{

  /* set shaded bit in iconflags */
#ifndef SKS_ACW
  wimpt_noerr(wimp_set_icon_state (d->w, dbox__fieldtoicon(f),
                                   wimp_INOSELECT, wimp_INOSELECT));
#else /* SKS_ACW */
  winf_fadefield(dbox_window_handle(d), dbox_field_to_icon_handle(ed, f));
#endif /* SKS_ACW */
}

void  dbox_unfadefield (dbox d, dbox_field f)
{

  /* unset shaded bit in iconflags */
#ifndef SKS_ACW
  wimpt_noerr(wimp_set_icon_state(d->w, dbox__fieldtoicon(f),
                                  0, wimp_INOSELECT));
#else /* SKS_ACW */
  winf_unfadefield(dbox_window_handle(d), dbox_field_to_icon_handle(ed, f));
#endif /* SKS_ACW */
}


void dbox_setfield(dbox d, dbox_field f, char *value)
{
#ifndef SKS_ACW
  wimp_icon *i = dbox__fieldtoiconptr(d, f);
  if ((i->flags & wimp_ITEXT) == 0)
  {
    tracef0("SetField of non-text.");
    /* Allowed, has no effect */
  }
  else
  {
    wimp_caretstr caret ;
    if ((i->flags & wimp_INDIRECT) != 0) {
      (void) memcpy(i->data.indirecttext.buffer, value,
        dbox__min(i->data.indirecttext.bufflen - 1,
                  strlen(value) + 1));
      i->data.indirecttext.buffer[i->data.indirecttext.bufflen-1] = 0;
    }
    else
    {
      (void) memcpy(&i->data.text[0], value, 12);
      i->data.text[11] = 0;
    }

    /* ensure that the caret moves correctly if it's in this icon */

    wimpt_noerr(wimp_get_caret_pos(&caret)) ;

    if (caret.w == d->w && caret.i == dbox__fieldtoicon(f))
    {
     int l = strlen((i->flags & wimp_INDIRECT) != 0 ?
                       i->data.indirecttext.buffer : i->data.text) ;

     if (caret.index > l) caret.index = l ;
     caret.height = -1;   /* calc from index */
         /*Fix MED-4747: was 'caret.height = caret.x = caret.y -1;' J R C
            28th Feb 1995*/
     wimpt_noerr(wimp_set_caret_pos(&caret)) ;
    }

    /* prod it, to cause redraw */
    wimpt_noerr(wimp_set_icon_state(d->w, dbox__fieldtoicon(f), 0, 0));
  }
#else /* SKS_ACW */
  winf_setfield(d->w, dbox_field_to_icon_handle(d, f), value);
#endif /* SKS_ACW */
}

void dbox_getfield(dbox d, dbox_field f, char *buffer, int size)
{
#ifndef SKS_ACW
  wimp_icon *i = dbox__fieldtoiconptr(d, f);
  int j = 0;
  char *from;
  if ((i->flags & wimp_ITEXT) == 0) {
    tracef0("GetField of non-text.");
    /* Allowed, returns "". */
  } else {
    if ((i->flags & wimp_INDIRECT) != 0) {
      while (i->data.indirecttext.buffer[j] >= 32) {j++;}
      from = i->data.indirecttext.buffer;
    } else {
      while (i->data.text[j] >= 32 && j < 11) {j++;}
      from = &i->data.text[0];
    }
    if (j > size) {j = size;}
    tracef3("GetField copies %i from %i to %i.\n",
      j, (int) from, (int) buffer);
    (void) memcpy(buffer, from, j);
  }
  buffer[j] = 0;
#else /* SKS_ACW */
  winf_getfield(d->w, dbox_field_to_icon_handle(d, f), buffer, size);
#endif /* SKS_ACW */
  tracef1("GetField returns %s.\n", (int) buffer);
}

static int dbox__fieldlength(dbox d, dbox_field f)
{
  char a[255];
  dbox_getfield((dbox) d, f, a, 255);
  tracef1("got field %i in FieldLength.\n", f);
  return(strlen(a));
}

void dbox_setnumeric(dbox d, dbox_field f, int n)
{
#ifndef SKS_ACW
  char a[20];
  wimp_icon *i = dbox__fieldtoiconptr(d, f);
  dbox_fieldtype ftype = dbox__iconfieldtype(i);

  switch (ftype) {
  case dbox_FONOFF:
  case dbox_FACTION:
      if (n)
        wimpt_noerr(wimp_set_icon_state(d->w, dbox__fieldtoicon(f),
          wimp_ISELECTED, wimp_ISELECTED));
      else
        wimpt_noerr(wimp_set_icon_state(d->w, dbox__fieldtoicon(f),
          0, wimp_ISELECTED));
      break;
  default:
      sprintf(a, "%i", n);
      dbox_setfield((dbox) d, f, a);
  }
#else /* SKS_ACW */
  wimp_icon *i = dbox__fieldtoiconptr(d, f);
  dbox_fieldtype ftype = dbox__iconfieldtype(i);

  switch(ftype) {
  case dbox_FONOFF:
  case dbox_FACTION:
      winf_setonoff(d->w, dbox_field_to_icon_handle(d, f), (n != 0));
      break;
  default:
      winf_setint(d->w, dbox_field_to_icon_handle(d, f), n);
      break;
  }
#endif /* SKS_ACW */
}

int dbox_getnumeric(dbox d, dbox_field f)
{
#ifndef SKS_ACW
  char a[20];
  int n;
  int i;
  int neg;
  int fail;
  wimp_icon *iptr = dbox__fieldtoiconptr(d, f);
  wimp_icon icon;

  if (dbox__iconfieldtype(iptr) == dbox_FONOFF) {
    wimpt_noerr(wimp_get_icon_info(d->w, dbox__fieldtoicon(f), &icon));
    if ((icon.flags & wimp_ISELECTED) != 0) {
      n = 1;
    } else {
      n = 0;
    }
  } else {
    dbox_getfield((dbox) d, f, a, 20);
    tracef1("dbox_getnumeric on '%s'\n",(int) a) ;
    n = 0;
    i = 0;
    neg = 0;
    fail = 0;
    while (1) {
      if (fail || a[i] == 0) {break;}
      if (a[i] == '-') {
        if (neg || (n!=0)) {fail = 1;} else {neg = 1;}
      } else if ((a[i] >= '0') && (a[i] <= '9')) {
        n = n * 10 + a[i] - '0';
      } else {
        tracef1("dbox_getnumeric fails with %d\n",a[i]) ;
        fail = 1;
      }
      i++;
    }
    if (neg) {n = -n;}
    if (fail) {n = 0;}
  }
#else /* SKS_ACW */
  int n;
  wimp_icon *iptr = dbox__fieldtoiconptr(d, f);

  switch(dbox__iconfieldtype(iptr)) {
  case dbox_FONOFF:
  case dbox_FACTION:
    n = winf_getonoff(d->w, dbox_field_to_icon_handle(d, f));
    break;
  default:
    n = winf_getint(d->w, dbox_field_to_icon_handle(d, f), 0);
    break;
  }
#endif /* SKS_ACW */
  return(n);
}

/* -------- Arrival of events from DBoxes. -------- */

dbox_field dbox_get(dbox d)
{
  d->fieldwaiting = 0;
  return(d->field);
}

#ifndef UROM
dbox_field dbox_read(dbox d)
{
  return(d->field);
}
#endif

void dbox_eventhandler(dbox d, dbox_handler_proc handler, void* handle)
{
  d->eventproc = handler;
  d->eventprochandle = handle;
}

void dbox_raw_eventhandler(dbox d, dbox_raw_handler_proc handler, void *handle)
{
  d->raweventproc = handler;
  d->raweventprochandle = handle;
}

/* -------- Processing Wimp Events. -------- */

static void dbox__buttonclick(dbox d, dbox_field f)
{
  tracef1("Button click icon %i.\n", f);
  d->field = f;
  d->fieldwaiting = 1;
  if (d->eventproc != 0) {
    tracef0("obeying user event proc.\n");
    d->eventdepth++;
    d->eventproc((dbox) d, d->eventprochandle);
    d->eventdepth--;
    if (d->disposepending && d->eventdepth == 0) {
      tracef0("delayed dispose of DBox.\n");
      dbox__dodispose(d);
    }
  }
}

#ifdef SKS_ACW

static void dbox__hitfield(dbox d, dbox_field hit_j, BOOL adjustclicked, BOOL notwimphit)
{
    wimp_w w;
    wimp_icon *i;
    wimp_i j, nicons;
    wimp_i k, icon_to_select;
    dbox_fieldtype f;
    wimp_icon icon;
    int esg;
    wimp_i esg_first, esg_selected;
    int woggle_count;
    BOOL allow_invert = TRUE;
    wimp_eventstr * e;

    tracef2("[dbox_hitfield(&%p, %d)]\n", d, hit_j);

    w      = d->w;
    nicons = d->p_window->nicons;

    i = dbox__iconhandletoptr(d, hit_j);
    f = dbox__iconfieldtype(i);

    /* lots of pratting about required here to cope with the inadequacies
     * of el Window Manager. If a member of an non-menu type esg, then if right click, step
     * selection, deselect all others anyway.
    */
    esg = i->flags & wimp_IESGMASK;
    tracef2("[dbox_hitfield icon %d esg %8X]\n", hit_j, esg);
    if(dbox__ibutflags(i) == MENU_IFLAGS)
        {
        /* give it a woggle now, restore state after */
        icon_to_select = (wimp_i) hit_j;
        for(woggle_count = 0; woggle_count < 5; ++woggle_count)
            winf_invertfield(w, icon_to_select);
        }
    else if(esg)
        {
        /* first turn all selected items off, select just one at end */
        esg_first = esg_selected = icon_to_select = (wimp_i) -1;
        j = 0;
        do  {
            i = dbox__iconhandletoptr(d, j);
            k = (wimp_i) j;
            tracef2("[considering icon %d, esg %8X]\n", j, i->flags & wimp_IESGMASK);
            if(esg == (i->flags & wimp_IESGMASK))
                {
                if(esg_first == (wimp_i) -1)
                    esg_first = k; /* found the first member of this ESG */

                (void) wimpt_complain(wimp_get_icon_info(w, k, &icon));
                if(icon.flags & wimp_ISELECTED)
                    {
                    esg_selected = k;
                    if(adjustclicked  ||  (k != (wimp_i) hit_j))
                        {
                        /* if found a selected one turn it off if adjustclicked or not the right one */
                        (void) wimpt_complain(wimp_set_icon_state(w, esg_selected,
                                                                  /* EOR */ wimp_ISELECTED,
                                                                  /* BIC */ (wimp_iconflags) 0));
                        }
                    else
                        {
                        /* Select clicked on selected icon - keep selected */
                        icon_to_select = (wimp_i) hit_j;
                        allow_invert = FALSE;
                        }
                    }
                else
                    {
                    if(adjustclicked)
                        {
                        if((esg_selected != (wimp_i) -1)  &&  (icon_to_select == (wimp_i) -1))
                            /* this icon is the next one along from the one that was selected */
                            icon_to_select = k;
                        }
                    else
                        {
                        if(k == (wimp_i) hit_j)
                            /* Select clicked on unselected icon - will need inversion to select */
                            icon_to_select = (wimp_i) hit_j;
                        }
                    }
                }
            }
        while(++j < nicons);

        tracef3("[esg_first %p, esg_selected %d, icon_to_select %p]\n", esg_first, esg_selected, icon_to_select);

        if(icon_to_select == (wimp_i) -1)
            {
            icon_to_select = esg_first;

            if(esg_selected == esg_first)
                /* one member ESG, already deselected */
                allow_invert = FALSE;
            }

        /* now select this one, all currently cleared */
        }
    else
        {
        /* toggle this one unless autorepeat type */
        if(dbox__ibutflags(i) == AUTO_IFLAGS)
            allow_invert = FALSE;

        /* if hit by Neil, he will have selected it already */
        if(!notwimphit)
            allow_invert = FALSE; /* why oh why oh why */

        icon_to_select = (wimp_i) hit_j;

        }

    if(allow_invert)
        winf_invertfield(w, icon_to_select);

    /* ensure hit looked like it came from a left button click */
    e = wimpt_last_event();
    if(e->e != wimp_EBUT)
        {
        e->e = wimp_EBUT;
        e->data.but.m.bbits = wimp_BLEFT;
        e->data.but.m.w     = w;
        e->data.but.m.i     = icon_to_select;
        }

    /* finally call the responsible client with the field finally selected */
    dbox__buttonclick(d, (dbox_field) icon_to_select);
}

#endif /* SKS_ACW */

static BOOL dbox__hitbutton(dbox d, int button)
/* A button is an action button or an on/off switch. "button" counts only
such interesting buttons, button==0 -> the first one in the DBox. Find the
right icon. If an action, do it. If on/off, flip it. If button is too big, do
nothing. */
{
  wimp_icon *i;
  int j = 0; /* counts icons */
  dbox_fieldtype f;
#ifndef SKS_ACW
  wimp_icon icon;
  BOOL icon_found = FALSE;

  for (j=0; j<d->window.nicons; j++) {
    i = dbox__iconhandletoptr(d, j);
    f = dbox__iconfieldtype(i);
    if (f == dbox_FACTION || f == dbox_FONOFF) {
      if (button == 0) {
        /* this is the right one */
        if (f == dbox_FACTION) {
          tracef1("buttonclick %i.\n", j);
          dbox__buttonclick(d, j);
        } else {
          /* on/off button */
          tracef1("Flip on/off %i.\n", j);
          (void) wimp_get_icon_info(d->w, j, &icon);
          if ((icon.flags & wimp_ISELECTED) != 0) {
            wimpt_noerr(wimp_set_icon_state(d->w, j, wimp_ISELECTED, 0));
          } else {
            wimpt_noerr(wimp_set_icon_state(d->w, j, wimp_ISELECTED, wimp_ISELECTED));
          }
          /* inverted the select bit. */
        }
        icon_found = TRUE;
        break;
      } else {
        /* right sort, but not this one. keep going. */
        button--;
      }
    } else {
      /* not the right sort of icon: keep going. */
    }
  }
#else /* SKS_ACW */
  int esg, esg_seen_mask = 0;
  BOOL icon_found = FALSE;

  /* loop over icons until correct button found */
  for (j=0; j<d->p_window->nicons; j++) {
    i = dbox__iconhandletoptr(d, j);
    f = dbox__iconfieldtype(i);
    switch (f)
    {
    case dbox_FONOFF:
    case dbox_FACTION:
      /* on/off or action button */
      esg = i->flags & wimp_IESGMASK;
      if(esg)
      {
        /* only consider the first member of an ESG, ignore others for count */
            esg = esg / wimp_IESG; /* number in 1..15 */
        esg = 1 << esg;
        if(esg_seen_mask & esg)
        {
          tracef2("[icon %d is a member of a noted ESG, mask %8X --- ignored]\n", j, esg);
          continue; /* get another icon before considering button hit */
        }
        tracef2("[icon %d is the first member of an ESG, mask %8X --- counted]\n", j, esg);
        esg_seen_mask |= esg;
      }
      else
      {
        tracef1("[icon %d is not a member of any ESG --- counted]\n", j);
      }

      if(button-- == 0)
      {
        dbox__hitfield(d, j, TRUE, TRUE);
        return FALSE;
      }

      break;

    default:
      tracef1("[icon %d is not an action or an on/off button --- ignored]\n", j);
      break;
    }
  }
#endif /* SKS_ACW */

  return icon_found;
}


#define DBOX_SHIFTED_FN_START 10

static BOOL dbox__wimp_event_handler(wimp_eventstr *e, void *handle)
{
  dbox d = (dbox) handle;
  wimp_caretstr c;
  wimp_icon *i;
  wimp_i j;
  char target;
  BOOL setcaretpos = FALSE;
  BOOL done;
  int ch;

  tracef2("[dbox__wimp_event_handler got event %s for dbox &%p]\n", report_wimp_event(e->e, &e->data), d);

  if (d->raweventproc != 0) {
    tracef2("calling client-installed raw event handler(%s, &%p)\n",
            report_procedure_name(report_proc_cast(d->raweventproc)),
            d->raweventprochandle);
    d->eventdepth++;
    done = (d->raweventproc)(d, (void*) e, d->raweventprochandle);
    d->eventdepth--;
    if (d->disposepending && d->eventdepth == 0) {
      tracef0("delayed dispose of DBox.\n");
      dbox__dodispose(d);
      return(done);
    } else if (done) { /* this event has been processed. */
      return(done);
    }
  }

  /* process some events */
  done = TRUE;

  switch (e->e) {
  case  wimp_ECLOSE:
      dbox__buttonclick(d, dbox_CLOSE); /* special button code */
      break;

#ifndef SKS_ACW
  case wimp_EOPEN:
      wimpt_noerr(wimp_open_wind(&e->data.o));
      break;
#endif /* SKS_ACW */

  case wimp_EBUT:
      if(((wimp_BMID | (wimp_BMID << 4) | (wimp_BMID << 8)) & e->data.but.m.bbits) != 0) {
        /* ignore it */
        /* It will already have been intercepted (by Events) if there's
        a menu, otherwise we're not interested anyway. */
      } else if (e->data.but.m.i != (wimp_i) -1) {
        /* ignore clicks not on icons. */
        i = dbox__iconhandletoptr(d, e->data.but.m.i);
        switch(dbox__iconfieldtype(i)) {
        case dbox_FONOFF:
        case dbox_FACTION:
            dbox__hitfield(d, (dbox_field) e->data.but.m.i, e->data.but.m.bbits & wimp_BRIGHT, FALSE);
            break;
        default:
            break;
        }
      }
      break;

  case wimp_EKEY:
      wimpt_noerr(wimp_get_caret_pos(&c));
      ch = e->data.key.chcode;
      switch(e->data.key.chcode) {
      case akbd_Fn+1:
      case akbd_Fn+2:
      case akbd_Fn+3:
      case akbd_Fn+4:
      case akbd_Fn+5:
      case akbd_Fn+6:
      case akbd_Fn+7:
      case akbd_Fn+8:
      case akbd_Fn+9:
          dbox__hitbutton(d, ch - (akbd_Fn+1));
          break;

      case akbd_Fn10:
  /*  case akbd_Fn11:     keep off F11 too!!!     *** */
  /*  case akbd_Fn12:     keep off F12 !!!        *** */
          dbox__hitbutton(d, ch - (akbd_Fn10) + 9);
          break;

#if defined(DBOX_SHIFTED_FN_START)
      case akbd_Sh+akbd_Fn+1:
      case akbd_Sh+akbd_Fn+2:
      case akbd_Sh+akbd_Fn+3:
      case akbd_Sh+akbd_Fn+4:
      case akbd_Sh+akbd_Fn+5:
      case akbd_Sh+akbd_Fn+6:
      case akbd_Sh+akbd_Fn+7:
      case akbd_Sh+akbd_Fn+8:
      case akbd_Sh+akbd_Fn+9:
          dbox__hitbutton(d, ch - (akbd_Sh+akbd_Fn+1) + DBOX_SHIFTED_FN_START);
          break;

      case akbd_Sh+akbd_Fn10:
  /*  case akbd_Sh+akbd_Fn11:     keep off F11 too!!!     *** */
  /*  case akbd_Sh+akbd_Fn12:     keep off F12 !!!        *** */
          dbox__hitbutton(d, ch - (akbd_Sh+akbd_Fn10) + 9 + DBOX_SHIFTED_FN_START);
          break;
#endif

      case 13: /* return key */
          tracef1("Caret is in icon %i\n", c.i);

#if defined(PDACTION)
          /* should be first action button! */
          dbox__hitbutton(d, 0);
#else /* PDACTION */
          if(c.i != (wimp_i) -1)
          {
            c.i = (wimp_i) ((wimp__i) c.i + 1);
            if( ((wimp__i) c.i >= nicons)  ||
                !dbox__findicon(d,
                                WRITABLE_IFLAGS | wimp_INOSELECT,
                                WRITABLE_IFLAGS | 0, /* desired state of just these flags */
                                &c.i)
                /* find a writeable button */
              )
            {
              /* should be first action button! */
              dbox__hitbutton(d, 0);
            }
            else
            {
              c.index = dbox__fieldlength(d, c.i);
              setcaretpos = TRUE;
            }
          }
          else
          {
            /* should be first action button! */
            dbox_buttonclick(d, 0);
          }
#endif /* PDACTION */
          break;

      case 27: /* ESC key */
          dbox__buttonclick(d, dbox_CLOSE);
          break;

#ifndef SKS_ACW /* leave to Window Manager Kat validation processing now */
#if defined(PDACTION)
      case akbd_TabK:
#endif /* PDACTION */
      case akbd_DownK:
          tracef1("Caret is in icon %i\n", c.i);

#ifdef DBOX_MOTION_UPDATES
          if(d->bits.motion_updates)
              dbox__buttonclick(d, c.i);
#endif
          if(c.i == (wimp_i) -1) {
            /* do nothing */
          } else {
            c.i++;
            if( (c.i >= d->p_window->nicons)  ||
                !dbox__findicon(d,
                                (wimp_iconflags) (WRITABLE_IFLAGS | wimp_INOSELECT),
                                (wimp_iconflags) (WRITABLE_IFLAGS),
                                &c.i))
            {
              c.i = 0;
              (void) dbox__findicon(d,
                                    (wimp_iconflags) (WRITABLE_IFLAGS | wimp_INOSELECT),
                                    (wimp_iconflags) (WRITABLE_IFLAGS),
                                    &c.i);
              /* bound to find at least the one you started on.*/
            }

            c.index = dbox__fieldlength(d, c.i);
            setcaretpos = TRUE;
          }
          break;

#if defined(PDACTION)
      case akbd_TabK + akbd_Sh:
#endif /* PDACTION */
      case akbd_UpK:
          tracef1("Caret is in icon %i\n", c.i);

#ifdef DBOX_MOTION_UPDATES
          if(d->bits.motion_updates)
              dbox__buttonclick(d, c.i);
#endif
          if (c.i == (wimp_i) -1) {
            /* do nothing */
          } else {
            if(!dbox__findiconbefore(d,
                                     (wimp_iconflags) (WRITABLE_IFLAGS | wimp_INOSELECT),
                                     (wimp_iconflags) (WRITABLE_IFLAGS),
                                     &c.i))
            {
              c.i = d->p_window->nicons;
              (void) dbox__findiconbefore(d,
                                          (wimp_iconflags) (WRITABLE_IFLAGS | wimp_INOSELECT),
                                          (wimp_iconflags) (WRITABLE_IFLAGS),
                                          &c.i);
              /* bound to find at least the one you started on */
            }

            c.index = dbox__fieldlength(d, c.i);
            setcaretpos = TRUE;
          }
          break;
#endif /* SKS_ACW */

      default:
          /* If not to a field and this is a letter, try matching it
          with the first chars of action buttons in this DBox. */
          if(!(ch & ~0xFF)  &&  isalpha(ch))
          {
            ch = toupper(ch); /* now buggered */
            for(j = 0; j < d->p_window->nicons; ++j) {
              tracef1("trying icon %i\n", j);
              i = dbox__iconhandletoptr(d, j);
              if ((i->flags & wimp_ITEXT) != 0
              && dbox__iconfieldtype(i) == dbox_FACTION) {
                const char *targetptr;
                BOOL found = FALSE;

                if ((i->flags & wimp_INDIRECT) != 0) {
                  targetptr = &i->data.indirecttext.buffer[0];
                } else {
                  targetptr = &i->data.text[0];
                }

                while (1) {
                  target = *targetptr++;
                  if (target == 0) break;
                  if (target == ch) { /* NB ch is uppercased */
                    tracef2("clicking on %i, %i.\n", j, target);
                    dbox__hitfield(d, j, FALSE, TRUE);
                    found = TRUE;
                    break;
                  }
                  /* end if we didn't match the capital letter */
                  if (isupper(target)) break;
                }
                if (found) break;
                tracef2("no: target=%i,code=%i.\n",
                  target, e->data.key.chcode);
              }
            }
          } else {
            tracef1("Key code %i ignored.\n", e->data.key.chcode);
            wimp_processkey(e->data.key.chcode);
          }
          break;
      }

      /* end of all EKEY type events */
      if(setcaretpos)
      {
        c.height = -1; /* calc x,y,h from icon/index */
        tracef2("Setting caret in icon %i, index = %i\n", c.i, c.index);
        (void) wimpt_complain(wimp_set_caret_pos(&c));
      }

      break;

  default:
      /* do nothing */
      tracef1("[dbox__wimp_event_handler: ignored event %s]\n", report_wimp_event(e->e, &e->data));
      done = FALSE;
      break;
  }

  return(done);
}

/* -------- New and Dispose. -------- */

#ifndef SKS_ACW

dbox dbox_new(char *name)
{
  dbox d = dbox__fromtemplate(template_find(name));
  wimp_i j;
  if (d == 0) {
    werr(FALSE, msgs_lookup(MSGS_dbox1));
    return 0;
  }
  d->next = 0;
  d->posatcaret = (wimp_WTRESPASS & d->window.flags) != 0;
  d->window.flags &= ~wimp_WTRESPASS;
  { os_error *er;
    er = wimp_create_wind(&d->window, &d->w);
    if (er != 0) {
      werr(FALSE, &er->errmess[0]);
      dbox__dispose(d);
      return 0;
    }
  }
  d->eventproc = 0;
  d->raweventproc = 0;
  d->disposepending = 0;
  d->eventdepth = 0;
  d->fieldwaiting = 0;
  d->field = 0;
  d->showing = 0;
  win_register_event_handler(d->w, dbox__wimp_event_handler, d);
  tracef0("Template created.\n");
  j = 0;
  if (dbox__findicon(d, WRITABLE_IFLAGS, WRITABLE_IFLAGS, &j)) {
    /* there is a writable icon to be found. */
    tracef1("Set caret in icon %i.\n", j);
    /* Default setting, used in FillIn */
    d->caretstr.w = d->w;
    d->caretstr.i = j;
    d->caretstr.x = 0;
    d->caretstr.y = 0;
    d->caretstr.height = -1;
    d->caretstr.index = INT_MAX;
  }
  return d;
}

#else /* SKS_ACW */

/* SKS Now ONLY for binary compatibility */

dbox dbox_new(char * name)
{
  char * errorp;
  return(dbox_new_new(name, &errorp));
}

static template *dbox__findtemplate(const char * name)
{
  template * templateHandle = template_find_new(name);
  if(TRACE  &&  !templateHandle)
    werr(FALSE, "Template '%s' not found", name);
  return(templateHandle);
}

dbox dbox_new_new(const char * name, char ** errorp /*out*/)
{
  dbox d;

  tracef1("dbox_new_new(%s)\n", name);

  d = dbox__fromtemplate(dbox__findtemplate(name));

  if (d == 0) {
    *errorp = NULL;
  }
  else
  { os_error *er;
    er = winx_create_window((WimpWindowWithBitset *) d->p_window, (HOST_WND *) &d->w, dbox__wimp_event_handler, d);
    if (er != 0) {
      *errorp = er->errmess;
      dbox__dispose(d);
      return 0;
    }
  }

  tracef1("dbox_new_new yields &%p\n", d);
  return d;
}

#endif /* SKS_ACW */

static void dbox__doshow(dbox d, BOOL isstatic)
/* This is complicated by the following case: if the show is as a result
of a submenu message (e.g. that was the last message received) then we
open the dbox as a submenu rather than as a standalone window. */
{
  wimp_mousestr m;
  wimp_caretstr c;
  wimp_openstr o;
  wimp_wstate ws;
  wimp_eventstr *e;
  int submenu;
  int xsize, ysize;

  d->bits.sent_close = FALSE; /* clear this every time we show up */

  if (d->showing) return;
  d->showing = TRUE;
#ifndef SKS_ACW
  win_activeinc();
#endif

  e = wimpt_last_event();

  submenu = ((e->e == wimp_ESEND)  &&  (e->data.msg.hdr.action == wimp_MMENUWARN));

  if(submenu && !isstatic)
  {
    event_read_submenupos(&m.x, &m.y);
    dbox_noted_position_trash();
  }
  else if(noted_position.noted)
  {
    m.x = noted_position.x;
    m.y = noted_position.y;
    noted_position.noted = FALSE;
    /* but don't clear note_position! */
  }
  else
  {
    tracef0("Move dbox to near pointer\n");
    (void) wimpt_complain(wimp_get_point_info(&m));
    m.x -= 64 /*32*/; /* try to be a bit into it */
    m.y += 64 /*32*/; /* SKS 23oct96 make distance consistent with Fireworkz dialogue boxes */

    if (d->posatcaret) {
      /* move to near the caret. */
      if ((e->e == wimp_EKEY)) {
        /* move to near the caret if it's in a window */
        (void) wimpt_complain(wimp_get_caret_pos(&c));
        if(c.w != (wimp_w) -1) {
          tracef0("Move dbox to near caret.\n");
          (void) wimpt_complain(wimp_get_wind_state(c.w, &ws));
          c.x = c.x + (ws.o.box.x0 - ws.o.x);
          c.y = c.y + (ws.o.box.y1 - ws.o.y);

          m.x = c.x + 100; /* a little to the right */
          m.y = c.y - 120; /* a little down */
        }
      }
    }
  }

  if(submenu && !isstatic)
  {
    /* this is a dbox that is actually part of the menu tree */
    tracef0("opening dbox as top-level menu (submenu)");
    (void) wimpt_complain(winx_create_submenu(d->w, m.x, m.y));
  }
  else
  {
    o.w = d->w;
    o.box = d->p_window->box;
    o.x = d->p_window->scx;
    o.y = d->p_window->scy;
    o.behind = -1;

    xsize = o.box.x1 - o.box.x0;
    ysize = o.box.y1 - o.box.y0;

    tracef2("put box at (%i,%i).\n", m.x, m.y);
    o.box.x0 = m.x;
    o.box.y0 = m.y - ysize;
    o.box.x1 = m.x + xsize;
    o.box.y1 = m.y;

    /* try not to overlap icon bar much (SKS 23oct96 from Fireworkz) */
    if(96 > o.box.y0)
    {
      int d_y = 96 - o.box.y0;
      o.box.y0 += d_y;
      o.box.y1 += d_y;
    }

    if(isstatic) {
      (void) wimpt_complain(winx_open_window((WimpOpenWindowBlock *) &o));
    } else {
      /*dbox__submenu = d->w;*/ /* there's only ever one. */
      tracef0("opening dbox as top-level menu (not submenu)");
      (void) wimpt_complain(winx_create_menu(d->w, o.box.x0, o.box.y1));
    }

    tracef0("Dialog box shown\n");
  }
}

void dbox_show(dbox d) {
  dbox__doshow(d, FALSE);
}

void dbox_showstatic(dbox d) {
  dbox__doshow(d, TRUE);
}

static void
dbox__dohide(dbox d)
{
  tracef1("dbox_hide(&%p): ", d);
  if (! d->showing) {
    tracef0("dbox_hide, not showing\n");
  } else {
    d->showing = FALSE;
#ifndef SKS_ACW
    win_activedec();
    if (d->w == dbox__submenu) {
      wimp_wstate ws;
      tracef0("hiding submenu dbox.\n");
      wimpt_noerr(wimp_get_wind_state(d->w, &ws));
      dbox__submenu = 0;
      if ((ws.flags & wimp_WOPEN) == 0) {
        /* The dbox has been closed: presumably by the wimp. */
        /* Thus, there is nothing more to do. */
      } else {
        /* The dbox was closed without the menu tree knowing about it. */
        event_clear_current_menu();
        /* That will cause the menu system to close the dbox. */
      }
    } else {
      tracef0("hiding non-submenu dbox.\n");
      wimpt_noerr(wimp_close_wind(d->w));
    }
#else /* SKS_ACW */
      if(note_position) { /* stash the window position */
        wimp_wstate ws;
        (void) wimpt_complain(wimp_get_wind_state(d->w, &ws));
        noted_position.x = (ws.o.box.x0 - ws.o.x);
        noted_position.y = (ws.o.box.y1 - ws.o.y);
        noted_position.noted = TRUE;
      }
      tracef0("hiding non-submenu dbox.\n");
      (void) wimpt_complain(winx_close_window(d->w));
#endif /* SKS_ACW */
  }

  note_position = FALSE; /* can only be a one-shot operation */
}

extern void
dbox_hide(dbox d)
{
  dbox__dohide(d);
}

void dbox_dispose(dbox *dd)
{
  dbox d = *dd;
  tracef2("dbox_dispose(&%p -> &%p): ", dd, d);
  if (d == 0) return;
  if (d->eventdepth != 0) {
    /* don't kill me yet */
    tracef0("setting pending dispose as in event processor\n");
    d->disposepending = 1;
  } else {
    dbox_hide(d);
    dbox__dodispose(d);
  }
  *dd = NULL;
}

/* -------- Event processing. -------- */

/* We cheerfully allow the caret to go elsewhere, but we intercept any
keystroke events and divert them to the dbox. This allows e.g. find commands
to see where in the text they've got to so far. dboxes with no fill-in fields
do not even try to get the caret. */

dbox_field dbox_fillin_loop(dbox d)
{
    wimp_eventstr e;
    int harmless;
    dbox_field result;
    wimp_wstate ws;
    wimp_caretstr caret;
    wimp_w w = d->w;
    wimp_i j = 0;

    tracef3("dbox_fillin(&%p (%d)): sp ~= &%p\n", d, w, &ws);

    if(d->bits.sent_close)
        return(dbox_CLOSE); /* keep returning this until the client listens */

    while (1) {
        /* keep going round until he presses a button (one may already be queued on entry) */
        if(d->fieldwaiting)
            {
            result = dbox_get(d);
            break;
            }

        if(wimpt_complain(wimp_get_caret_pos(&caret))) { result = dbox_CLOSE; break; }

        if(d->caret_set)
            { /*EMPTY*/ /* SKS 20130523 don't repeat */
            }
        else if(caret.w != w) /* SKS: only set caret pos if not in here */
            {
            if(dbox__findicon(d,
                              (wimp_iconflags) (WRITABLE_IFLAGS | wimp_INOSELECT),
                              (wimp_iconflags) (WRITABLE_IFLAGS), /* desired state of these flags */
                              &j))
                {
                tracef1("setting caret in icon %i\n", j);

                caret.w      = w;
                caret.i      = j;
                caret.x      = 0;
                caret.y      = 0;
                caret.height = -1;      /* calc x,y,h from icon/index */
                caret.index  = dbox__fieldlength(d, j);

                (void) wimpt_complain(wimp_set_caret_pos(&caret));

                d->caret_set = TRUE;
                }
            else
                tracef0("no writeable icons in dbox\n");
            }
        else
            tracef0("caret already in dbox\n");

#if TRACE
        {
        int i;
        tracef3("[dbox_fillin(&%p (%d)) (sp ~= &%p) doing wimpt_poll]\n", d, w, &i);
        }
#endif

        wimpt_poll(event_getmask(), &e);

        tracef1("[dbox_fillin got event %s]\n",
                report_wimp_event(e.e, &e.data));

        if(winx_submenu_query_is_submenu(w))
            {
            /* Check to see if the window has been closed.
             * If it has then we are in a menu tree, and the wimp
             * is doing things behind our backs.
            */
            if(winx_submenu_query_closed())
                {
                tracef0("--- menu dbox has been closed for us!\n");

                wimpt_fake_event(&e); /* stuff event back in the queue */

                /* On a redraw event you may not perform operations such
                 * as the deletion of windows before actually servicing the
                 * redraw. This caused the mysterious bug in ArcEdit such
                 * that certain dboxes of the menu tree caused a repaint
                 * of the entire window.
                */
                if(e.e == wimp_EREDRAW)
                    event_process();

                (void) wimpt_complain(winx_close_window(w));

                result = dbox_CLOSE;
                break;
                }
            else
                {
                tracef0("menu dbox is still open\n");

                (void) wimpt_complain(wimp_get_wind_state(w, &ws));

                if(0 == (ws.flags & wimp_WTOP))
                    {
                    trace_0(TRACE_OUT | TRACE_ANY, "menu dbox not at front!!!\n");
                    /*SKS 08jan97 winx_send_front_window_request(w, FALSE);*/
                    }
                }
            }

        harmless = TRUE;

        switch(e.e)
            {
            case wimp_EKEY:
                /* Pass key events to submenu dbox */
                if(winx_submenu_query_is_submenu(w) &&  (e.data.key.c.w != w))
                    {
                    e.data.key.c.w = w;
                    e.data.key.c.i = (wimp_i) -1;
                    }
                break;

            case wimp_ESEND:
            case wimp_ESENDWANTACK:
                /* prequit message must be marked as harmful! */
                harmless = (e.data.msg.hdr.action != wimp_MPREQUIT);
                /* >>>> Hum: potentially not true, but this is what the
                 * menu mechanism does...
                */
                break;

            case wimp_ENULL:
            case wimp_EOPEN:
            case wimp_ECLOSE:
            case wimp_EREDRAW:
            case wimp_EPTRENTER:
            case wimp_EPTRLEAVE:
            case wimp_EBUT:
            case wimp_ESCROLL:
            case wimp_EMENU:
            case wimp_ELOSECARET:
            case wimp_EGAINCARET:
            case wimp_EUSERDRAG:
            default:
                break;
            }

        wimpt_fake_event(&e); /* stuff it back in the queue in any case */

        if(!harmless)
            {
            tracef0("harmful event causes dbox_fillin to give up\n");
            result = dbox_CLOSE;
            break;
            }

        tracef0("process harmless event\n");
        event_process();
        /* and loop until we get a result */
        }

    if(result == dbox_CLOSE)
        d->bits.sent_close = 1;

    return(result);
}

#ifndef SKS_ACW

dbox_field dbox_fillin(dbox d)
{
  wimp_i j = 0;

  if (dbox__findicon(d, WRITABLE_IFLAGS, WRITABLE_IFLAGS, &j)) {
    tracef1("Set caret in icon %i.\n", j);
    d->caretstr.i = j;
    d->caretstr.x = 0;
    d->caretstr.y = 0;
    d->caretstr.height = -1;
    d->caretstr.index = dbox__min(d->caretstr.index, dbox__fieldlength(d, j));
    /* w, i already set up. */
    d->caretstr.index = dbox__fieldlength(d, j);
    wimpt_noerr(wimp_set_caret_pos(&d->caretstr));
  }

  return dbox_fillin_loop(d);
}

dbox_field dbox_fillin_fixedcaret(dbox d)
{
   wimp_i j = 0;
   wimp_caretstr caret;

   wimpt_noerr(wimp_get_caret_pos(&caret));
   if (dbox__findicon(d, WRITABLE_IFLAGS, WRITABLE_IFLAGS, &j))
   {
      if (caret.w != dbox_syshandle(d))
      {
         d->caretstr.i = j;
         d->caretstr.x = 0;
         d->caretstr.y = 0;
         d->caretstr.height = -1;
         d->caretstr.index = dbox__fieldlength(d, j);
         wimpt_noerr(wimp_set_caret_pos(&d->caretstr));
      }
   }
   return dbox_fillin_loop(d);
}

#else /* SKS_ACW */

dbox_field dbox_fillin(dbox d)
{
  return dbox_fillin_loop(d);
}

#endif /* SKS_ACW */

#ifndef SKS_ACW

#ifndef UROM
dbox_field dbox_popup(char *name, char *message)
{
  dbox_field result;
  dbox d = dbox_new(name);

  if (!d) return 0;
  dbox_setfield(d, 1, message);
  dbox_show(d);
  result = dbox_fillin(d);
  dbox_dispose(&d);
  return(result);
}
#endif

#endif /* SKS_ACW */

BOOL dbox_persist(void) {
  wimp_mousestr m;
  wimpt_noerr(wimp_get_point_info(&m));
  return (m.bbits & wimp_BRIGHT) != 0;
}


/* -------- System Hook. -------- */

int dbox_syshandle(dbox d)
{
  if (d != NULL)
    return d->w;
  return (int)NULL;
}

/* -------- Initialisation. -------- */

void dbox_init(void)
{

#ifndef SKS_ACW
  if (template_loaded() == FALSE)
      werr(0, msgs_lookup(MSGS_dbox2));
#endif /* SKS_ACW */
}

#ifdef SKS_ACW

/*
SKS added for PipeDream
*/

extern BOOL
dbox_adjusthit(dbox_field * fp, dbox_field a, dbox_field b, BOOL adjustclicked)
{
    BOOL res;
    dbox_field f;

    f = *fp;

    res = ((f == a)  ||  (f == b));

    if(res  &&  adjustclicked)
        *fp = f ^ a ^ b;

    return(res);
}

extern void
dbox_motion_updates(dbox d)
{
    d->bits.motion_updates = 1;
}

extern void
dbox_note_position_on_completion(BOOL f)
{
    note_position = f;
}

extern void
dbox_noted_position_restore(void)
{
    noted_position = saved_position;
}

extern void
dbox_noted_position_save(void)
{
    saved_position = noted_position;
}

extern void
dbox_noted_position_trash(void)
{
    note_position = FALSE;

    noted_position.noted = FALSE;
}

extern void
dbox_raw_eventhandlers(dbox d, dbox_raw_handler_proc * handlerp, void ** handlep)
{
    if(handlerp)
        *handlerp = d->raweventproc;

    if(handlep)
        *handlep  = d->raweventprochandle;
}

#endif /* SKS_ACW */

/* end */
