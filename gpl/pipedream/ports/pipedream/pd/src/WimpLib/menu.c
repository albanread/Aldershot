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

/* Title: c.menu
 * Purpose: menu manipulation.
 * History: IDJ: 06-Feb-92: prepared for source release
 */

/*  SKS big hack 15-May-89 to not explode with fatal errors */

#include "include.h" /* for SKS_ACW */

#define BOOL int
#define TRUE 1
#define FALSE 0

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "os.h"
#include "wimp.h"
#include "werr.h"
#include "menu.h"

#include "sprite.h"
#include "resspr.h"
#include "msgs.h"
#include "Verintern/messages.h"

typedef struct menu__str {
  wimp_menuhdr *m;         /* the wimp-level menu that we've built. */
  int nitems;
  void *entryspace;        /* for sub-menus, and entries with >12 chars */
  int nbytes;
  int maxentrywidth;
} menu__str;
/* concrete representation of abstract menu object */

/* The menu__str structure points to a RISCOS-style menu, and to a separate
buffer of workspace for sub-menu pointers and for string fields that
are >12 characters. The format of the entryspace is:
  each sub-Menu pointer
  a Menu(NIL) word
  each entry buffer
*/

static wimp_menuitem *menu__itemptr(menu m, int n)
/* Pointer to the nth item in the menu (starting at 0). */
{
  return(((wimp_menuitem*)(m->m + 1)) + n);
}

/* -------- Building RISC OS Menus. -------- */

/* The menu is assembled entry by entry in temporary workspace, then copied
to more precisely allocated store. The copying of menu structures is split
into the allocation of store and then the copying of data, so that the copy
into the larger buffer can share the latter half of the operation. */

static void menu__disposespace(menu m)
{
  if (m->m != 0) {
    free(m->m);
    m->m = 0;
  }
  if (m->entryspace != 0) { /* can only happen with very new menu__strs. */
    free(m->entryspace);
    m->entryspace = 0;
  }
}

static BOOL menu__preparecopy(menu from, menu to)
{
  /* Allocate space in the destination to suit the copy. */
  to->m = malloc(sizeof(wimp_menuhdr) + from->nitems * sizeof(wimp_menuitem));
  if (to->m == 0) {
#ifdef SKS_ACW
    return(FALSE); /*SKS no longer fatal*/
#endif /* SKS_ACW */
  }
  if (from->nbytes != 0) {
    to->entryspace = malloc(from->nbytes);
    if (to->entryspace == 0) {
      free(to->m);
      to->m = NULL;
#ifdef SKS_ACW
      return(FALSE); /*SKS no longer fatal*/
#endif /* SKS_ACW */
    }
  } else {
    to->entryspace = 0;
  }
  return(TRUE);
}

static void menu__copydata(menu from, menu to)
/* Copy the data and lengths. Relocate indirection pointers. */
{
  int moveoffset;
  int i;

  to->maxentrywidth = from->maxentrywidth;
  to->nitems = from->nitems;
  (void) memmove(to->m, from->m,
    sizeof(wimp_menuhdr) + from->nitems * sizeof(wimp_menuitem));
  to->nbytes = from->nbytes;
  moveoffset = ((char*)to->entryspace) - ((char*)from->entryspace);
  (void) memmove(to->entryspace, from->entryspace, from->nbytes);
  for (i=0;i<to->nitems;i++) {
    wimp_menuitem *ptr = menu__itemptr(to, i);
    if ((ptr->iconflags & wimp_INDIRECT) != 0) {
      ptr->data.indirecttext.buffer += moveoffset;
    }
  }
}

/* The work area is allocated on the stack, with the following limits: */

#ifdef SKS_ACW /* was originally inadequate */
#define WORKAREASIZE  10*1024
#define AVGINDSIZE    32         /* estimated average size of indirected entry */
#define MAXITEMS      ((WORKAREASIZE - sizeof(menu__str) - sizeof(wimp_menuhdr)) / \
                       (sizeof(wimp_menuitem) + AVGINDSIZE))
                                 /* max size of a menu: ca. 192 - surely OK. */
#define MAXENTRYSPACE (MAXITEMS * AVGINDSIZE)
                                 /* space for building entries > 12 chars */
#else /* SKS_ACW */
#define MAXITEMS 64       /* max size of a menu: surely OK. */
#define MAXENTRYSPACE 1024 /* space for building entries > 12 chars */
#endif /* SKS_ACW */

typedef struct {
  menu__str m;
  wimp_menuhdr menuhdr;
  wimp_menuitem menuitems[MAXITEMS];
  char entryspace[MAXENTRYSPACE];
} menu__workarea;

static void menu__initworkarea(menu__workarea *w)
{
  tracef3("menu__initworkarea(&%p): MAXITEMS %d, sizeof(menu__workarea) = %d\n", w, MAXITEMS, sizeof(menu__workarea));
  w->m.m = &w->menuhdr;
  w->m.nitems = 0;
  w->m.entryspace = &w->entryspace[0];
  w->m.maxentrywidth = 0;
  /* insert a NIL in the entrySpace to distinguish sub-Menu pointers
  from text space. */
  w->m.nbytes = 4;
  *((int*)w->entryspace) = 0;
}

static void menu__copytoworkarea(menu m /*in*/, menu__workarea *w /*out*/)
{
  menu__initworkarea(w);
  menu__copydata(m, &w->m);
  menu__itemptr(&w->m, w->m.nitems-1)->flags &= ~wimp_MLAST;
}

static BOOL menu__copyworkarea(menu__workarea *w /*in*/, menu m /*out*/)
{
  if (w->m.nitems > 0) {
    menu__itemptr(&w->m, w->m.nitems-1)->flags |= wimp_MLAST;
  }
  menu__disposespace(m);
  if(!menu__preparecopy(&w->m, m))
#ifdef SKS_ACW
     return FALSE; /* SKS no longer fatal */
#endif /* SKS_ACW */
  menu__copydata(&w->m, m);
  return TRUE;
}

/* -------- Creating menu descriptions. -------- */

static void menu__initmenu(char *name, menu m /*out*/)
{
  int i;
  for (i=0; i<12; i++) {
    m->m->title[i] = name[i];
    if (name[i]==0) {break;}
  }
  m->m->tit_fcol = 7; /* title fore: black */
  m->m->tit_bcol = 2;  /* title back: grey */
  m->m->work_fcol = 7; /* entries fore */
  m->m->work_bcol = 0; /* entries back */
  m->m->width = i*16;  /* minimum value */
  m->m->height = 44;   /* per entry */
  m->m->gap = 0;       /* gap between entries, in OS units */
}

static int menu__max(int a, int b)
  { if (a < b) {return(b);} else {return(a);} }

static wimp_menuitem *menu__additem(
  menu__workarea *w /*out*/, char *name, int length)
/* The returned pointer can be used to set flags, etc. */
{
  wimp_menuitem *ptr;
  if (w->m.nitems == MAXITEMS) {return(menu__itemptr(&w->m, MAXITEMS-1));}
  ptr = menu__itemptr(&w->m, w->m.nitems++);
  ptr->flags = 0;
  ptr->submenu = (wimp_menustr*) -1;
  ptr->iconflags = wimp_ITEXT + wimp_IFILLED + wimp_IVCENTRE + (7*wimp_IFORECOL);
  if (length > w->m.maxentrywidth) {
    w->m.maxentrywidth = length;
    w->m.m->width = menu__max(w->m.m->width, 16 + length * 16);
      /* in OS units, 16 per char. */
  }
  if (length <= 12) {
    int i;
    for (i=0; i<length; i++) {ptr->data.text[i] = name[i];}
    if (length < 12) {ptr->data.text[length] = 0;}
  } else if (length+1+w->m.nbytes >= MAXENTRYSPACE) {
    /* no room for the text: unlikely */
    ptr = menu__itemptr(&w->m, w->m.nitems-1); /* fudge */
  } else {
    ptr->iconflags += wimp_INDIRECT;
    ptr->data.indirecttext.buffer = ((char*)w->m.entryspace) + w->m.nbytes;
    ptr->data.indirecttext.validstring = (char*) -1;
    ptr->data.indirecttext.bufflen = 100;
    (void) memmove(((char*)w->m.entryspace) + w->m.nbytes, name, length);
    w->m.nbytes += length + 1;
    ((char*)w->m.entryspace)[w->m.nbytes-1] = 0; /* terminate the string. */
  }
  return(ptr);
}

/* -------- Parsing Description Strings. -------- */

static void menu__syntax(void)
{
  /* General policy: be lenient on syntax errors, so do nothing */
}

typedef enum {
  TICK = 1,
  FADED = 2,
  DBOX = 4,
  NUM  = 8
} opt;

typedef enum {OPT, SEP, NAME, END} toktype;

typedef struct {
  char *s;
  toktype t;
  char ch;        /* last separator char encountered */
  opt opts;       /* last opts encountered */
  char *start;
  char *end;      /* last name encountered */
} parser;

static void menu__initparser(parser *p, char *s)
{
  p->s = s;
  p->ch = ',';
}

static void menu__getopt(parser *p)
{
  p->opts = 0;
  while (p->ch=='!' || p->ch=='~' || p->ch=='>' || p->ch=='#' || p->ch==' ') {
    if (p->ch=='!') {
      p->opts |= TICK;
    } else if (p->ch=='~') {
      p->opts |= FADED;
    } else if (p->ch=='#') {
      p->opts |= NUM;
    } else if (p->ch=='>') {
      p->opts |= DBOX;
    }
    p->ch=*p->s++;
  }
  p->s--;
}

static void menu__getname (parser *p)

{ /*Skip leading spaces*/
  while (p->ch == ' ')
    p->ch = *p->s++;

  p->start = p->s - 1;

  if (p->ch == '"')
  { /*Quoted string*/
    p->ch = *p->s++;

    p->start = p->s - 1;

    while (p->ch != 0 && p->ch != '"')
      p->ch = *p->s++;

    p->end = p->s - 1;

    if (p->ch == '"')
    { p->ch = *p->s++;

      /*Skip trailing spaces*/
      while (p->ch == ' ')
        p->ch = *p->s++;
    }

    if (p->ch != 0 && p->ch != ',' && p->ch != '|')
      p->ch = *p->s++;

    p->s--;
  }
  else
  { /*Non-quoted string*/
    p->start = p->s - 1;

    while (p->ch != 0 && p->ch != ',' && p->ch != '|')
      p->ch = *p->s++;

    p->end = --p->s;
  }
}

static toktype menu__gettoken(parser *p)
{
  p->ch = ' ';
  while (p->ch == ' ') p->ch = *p->s++;
  switch (p->ch) {
  case 0:
    p->t = END;
    break;
  case '!':
  case '~':
  case '>':
  case '#':
    p->t = OPT;
    menu__getopt(p);
    break;
  case ',':
  case '|':
    p->t = SEP;
    break;
  default:
    p->t = NAME;
    menu__getname(p);
    break;
  }
  return(p->t);
}

/* -------- Parsing and Extension. -------- */

static void menu__doextend(menu__workarea *w, char *descr)
{
  parser p;
  toktype tok;
  wimp_menuitem *ptr;

  menu__initparser(&p, descr);
  tok = menu__gettoken(&p);
  if (tok==END) {
    /* do nothing */
  } else {
    if (tok==SEP) {
      if (w->m.nitems == 0) {
        menu__syntax();
      } else {
        if (p.ch == '|') {
          ptr = menu__itemptr(&w->m, w->m.nitems-1);
          ptr->flags |= wimp_MSEPARATE;
        }
        tok = menu__gettoken(&p);
      }
    }
    while (1) {
      if (tok == OPT) {
        tok = menu__gettoken(&p); /* must be NAME, check below */
      } else {
        p.opts = 0;
      }
      if (p.t != NAME) {
        menu__syntax();
      } else {
        ptr = menu__additem(w, p.start, p.end - p.start);
        if ((TICK & p.opts) != 0) {
          ptr->flags |= wimp_MTICK;
        }
        if ((FADED & p.opts) != 0) {
          ptr->iconflags |= wimp_INOSELECT;
        }
        if ((NUM & p.opts) != 0) {
          ptr->iconflags |= (1<<20);
        }
        if ((DBOX & p.opts) != 0) {
          ptr->flags |= wimp_MSUBLINKMSG;
          ptr->submenu = (wimp_menustr*) 1;
        }
        tok = menu__gettoken(&p);
        if (tok == END) break;
        if (tok != SEP) {
          menu__syntax();
        } else {
          if (p.ch == '|') ptr->flags |= wimp_MSEPARATE;
        }
      }
      tok = menu__gettoken(&p);
    }
  }
}

/* -------- Entrypoints. -------- */

menu menu_new(char *name, char *descr)
{
  menu m;
  menu__workarea menu__w;
  wimp_menuitem *ptr;

  menu__initworkarea(&menu__w);
  menu__initmenu(name, &(menu__w.m));
  menu__doextend(&menu__w, descr);
  m = malloc(sizeof(menu__str));
  if (m == 0) {
#ifndef SKS_ACW
    werr(TRUE, msgs_lookup(MSGS_menu1));
#else /* SKS_ACW */
    return 0; /* SKS no longer fatal */
#endif /* SKS_ACW */
  }
  m->m = 0;
  m->entryspace = 0;
  if(!menu__copyworkarea(&menu__w, m))
    wlalloc_dispose((void **) &m);
#ifndef SKS_ACW
  if (strlen(name) > 12) {
      *(char **)m->m->title = name;
      ptr = menu__itemptr(m, 0);
      ptr->flags |= wimp_MINDIRECTED;
  }
#endif /* SKS_ACW */
  return m;
}

void menu_dispose(menu *m, int recursive)
{
#ifdef SKS_ACW /* SKS never recursive and extra arg checks */
  if ((m == NULL) || (*m == NULL)) return;
  if (recursive != 0) {
  /*EMPTY*/
  }
#else /* SKS_ACW */
  if (recursive != 0) {
    menu *a = (menu*) ((*m)->entryspace);
    while (1) {
      menu subm = *(a++);
      if (subm == 0) {break;}
      menu_dispose(&subm, 1);
    }
  }
#endif /* SKS_ACW */
  menu__disposespace(*m);
  free(*m);
}

void menu_extend(menu m, char *descr)
{
  menu__workarea menu__w;
  menu__copytoworkarea(m, &menu__w);
  menu__doextend(&menu__w, descr);
  menu__copyworkarea(&menu__w, m);
}

void menu_setflags(menu m, int entry, int tick, int fade)
{
  wimp_menuitem *p;
  if (entry == 0) {return;}
  if (entry > m->nitems) {return;}
  p = menu__itemptr(m, entry-1);
  if (tick != 0) {
    p->flags |= wimp_MTICK;
  } else {
    p->flags &= ~wimp_MTICK;
  }
  if (fade != 0) {
    p->iconflags |= wimp_INOSELECT;
  } else {
    p->iconflags &= ~wimp_INOSELECT;
  }
}

#ifndef SKS_ACW

void menu_make_writeable(menu m, int entry, char *buffer, int bufferlength,
                         char *validstring)
{
  wimp_menuitem *p;
  if (entry == 0) {return;}
  if (entry > m->nitems) {return;}
  p = menu__itemptr(m, entry-1);
  p->flags |= wimp_MWRITABLE ;
  p->iconflags |= wimp_BWRITABLE * wimp_IBTYPE + wimp_INDIRECT +
                  wimp_IHCENTRE + wimp_IVCENTRE + wimp_ITEXT ;
  p->data.indirecttext.buffer = buffer ;
  p->data.indirecttext.bufflen = bufferlength ;
  p->data.indirecttext.validstring = validstring ;
}


void menu_make_sprite(menu m, int entry, char *spritename)
{
  wimp_menuitem *p;
  if (entry == 0) {return;}
  if (entry > m->nitems) {return;}
  p = menu__itemptr(m, entry-1);


  p->iconflags &= ~wimp_ITEXT;
  p->iconflags |= wimp_INDIRECT+wimp_IVCENTRE+wimp_ISPRITE;
  p->data.indirectsprite.name = spritename;
  p->data.indirectsprite.spritearea = resspr_area();
  p->data.indirectsprite.nameisname = 1;
}

#endif /* SKS_ACW */

void menu_submenu(menu m, int place, menu submenu)
{
#ifndef SKS_ACW
  int i;
  wimp_menuitem *p = menu__itemptr(m, place-1);
  menu__workarea menu__w;

  p->submenu = (wimp_menustr*) (submenu?submenu->m:NULL);
  menu__copytoworkarea(m, &menu__w);
  (void) memmove(
    /* to */ ((menu*) menu__w.m.entryspace) + 1,
    /* from */ ((menu*) menu__w.m.entryspace),
    menu__w.m.nbytes);
  menu__w.m.nbytes += sizeof(menu*);
  *((menu__str**)(menu__w.m.entryspace)) = submenu;

  for (i=0; i<menu__w.m.nitems; i++) {
    p = menu__itemptr(&menu__w.m, i);
    if (((p->iconflags)&wimp_INDIRECT) != 0) {
      p->data.indirecttext.buffer += 4;
    }
  }
  menu__copyworkarea(&menu__w, m);
#else /* SKS_ACW */
  /* SKS wildy different */
  wimp_menuitem *p = menu__itemptr(m, place-1);
  p->submenu = submenu ? (wimp_menustr *) submenu->m : (wimp_menustr *) submenu;
#endif /* SKS_ACW */
}

void *menu_syshandle(menu m)
{
  if (m != NULL)
    return (void *)m->m;
  return (void *)-1;
}

#ifdef SKS_ACW

/*
SKS added for PipeDream
*/

extern menu
menu_new_c(const char *name, const char *descr)
{
    return(menu_new(de_const_cast(char *, name), de_const_cast(char *, descr)));
}

extern menu
menu_new_unparsed(const char *name, const char *descr)
{
    menu m;
    menu__workarea menu__w;

    tracef2("menu_new_unparsed(%s, %s)\n", name, descr);

    menu__initworkarea(&menu__w);
    menu__initmenu(de_const_cast(char *, name), &menu__w.m);

    menu__additem(&menu__w, de_const_cast(char *, descr), strlen(descr));
    /* Not menu__doextend, cos we don't want ',' or '|' treated as a separator */

    m = malloc(sizeof(menu__str));
    if(m)
    {
        m->m            = NULL;
        m->entryspace   = NULL;

        if(!menu__copyworkarea(&menu__w, m))
            wlalloc_dispose((void **) &m);
    }

    tracef1("menu_new_unparsed() returns &%p\n", m);

    return(m);
}

extern BOOL
menu_extend_unparsed(menu * mm, const char *descr)
{
    menu m = *mm;
    menu__workarea menu__w;
    BOOL res;

    tracef2("menu_extend_unparsed(&%p, %s)\n", m, descr);

    if(m)
    {
        menu__copytoworkarea(m, &menu__w);

        menu__additem(&menu__w, de_const_cast(char *, descr), strlen(descr));
       /* Not menu__doextend, cos we don't want ',' or '|' treated as a separator */

        res = menu__copyworkarea(&menu__w, m);

        if(!res)
            m = NULL;
    }
    else
        res = FALSE;

    *mm = m;

    return(res);
}

extern void
menu_settitle(menu m, const char * title)
{
    int i;

    tracef2("menu_settitle(&%p, %s)\n", m, title);

    if(!m)
        return;

    i = 0;
    do  {
        /* could use strncpy but for width calculation */
        m->m->title[i] = title[i];
        if (title[i] == CH_NULL) break;
    }
    while(++i < 12);

    m->m->width = max(m->m->width, i * 16);
}

extern void
menu_entry_changetext(menu m, int entry, const char *text)
{
    int i, length;
    wimp_menuitem *p;

    tracef3("menu_entry_settext(&%p, %d, %s)\n",
                m, entry, text);

    if(!m)
        return;

#if TRACE
    if((entry == 0)  ||  (entry > m->nitems))
    {
        werr(FALSE, "duff entry %d in menu_settext (%d items) - ignored", entry, m->nitems);
        return;
    }
#endif

    p = menu__itemptr(m, entry-1);

    /* if menu entry is non-indirected text, copy upto 12 chars from supplied string */
    if((p->iconflags & wimp_ITEXT) && !(p->iconflags & wimp_INDIRECT))
    {
        length = min(12, strlen(text));

        for(i = 0; i < length; i++)
            p->data.text[i] = text[i];
        if(length < 12)
            p->data.text[length] = CH_NULL;
    }
}

#endif /* SKS_ACW */

/* end */
