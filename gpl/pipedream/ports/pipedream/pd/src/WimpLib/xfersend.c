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

/* Title: c.xfersend
 * Purpose: generalised data transfer to a concurrent wimp program.
 * History: IDJ: 07-Feb-92: prepared for source release
 *
 */

#include "include.h" /* for SKS_ACW */

// SKS_ACW #define BOOL int
// SKS_ACW #define TRUE 1
// SKS_ACW #define FALSE 0

#define USE_DRAGASPRITE 1

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "trace.h"
#include "swis.h"
#include "os.h"
#include "bbc.h"
#include "wimp.h"
#include "wimpt.h"
#include "win.h"
#include "dbox.h"
#include "xfersend.h"
#include "cs-fileicon.h"
#include "werr.h"
#include "menu.h"
#include "event.h"
#include "msgs.h"
#include "dragasprit.h"
#include "VerIntern/messages.h"

#ifdef SKS_ACW
static struct _xfersend_statics
{
    BOOL                     ukproc;
    dbox_raw_handler_proc    oldraw_handler_proc;
    void *                   oldhandle;

    dbox                     d;
    xfersend_clickproc       clickproc;
    void *                   clickhandle;

    dbox_field               cancel_button;
}
xfersend_;
#endif /* SKS_ACW */

static int rcvbufsize;
static int xfersend__msgid = 0;           /* my_ref of last DataSave message */
static xfersend_saveproc xfersend__saveproc;
static xfersend_sendproc xfersend__sendproc;
static xfersend_printproc xfersend__printproc;
static int xfersend__filetype;
static void *xfersend__savehandle;
static int xfersend__estsize = 0;
static wimp_t xfersend__receiver;
static BOOL xfersend__fileissafe;
#ifndef SKS_ACW
static char *xfersend__filename;  /*[256]*/
#endif

#ifndef SKS_ACW
static int Unused; /*future expansion?*/
#endif

#ifdef SHARED_C_LIBRARY
static wimp_mousestr xfersend__mousestr = {0};
static wimp_msgstr xfersend__msg = {0};
#else
static wimp_mousestr xfersend__mousestr;
static wimp_msgstr xfersend__msg;
#endif

static BOOL xfersend__close = FALSE;
static wimp_w xfersend__w;

void xfersend_close_on_xfer (BOOL do_we_close, wimp_w w)
{
   xfersend__close = do_we_close;
   xfersend__w = w;
}

static void xfersend__winclose (void)
{
   wimp_eventdata e;
   e.o.w = xfersend__w;
   wimpt_noerr (wimp_sendwmessage (wimp_ECLOSE, (wimp_msgstr*) &e, e.o.w, -1));
}

#if USE_DRAGASPRITE
static BOOL xfersend__using_dragasprite (void)
{
   int r1, r2;

   r1 = 0x1c;
   os_byte (161, &r1, &r2);   /* read CMOS flag */
   return r2 & 0x02;
}
#endif

/******************************************************************************
*
* determine whether a filename is 'rooted' sufficiently
* to open not relative to cwd or path
*
******************************************************************************/

static BOOL
xfersend__file_is_rooted(
    PC_U8 filename)
{
    int res;

    res = (strpbrk(filename, ":$&%@\\") != NULL);

    return(res);
}

/* nd 23-07-1996 added veneer for DragASprite and Wimp_StartDrag */
static void nd_dragstop(void);

static BOOL xfersend__wimp_EBUT(dbox d, wimp_eventstr *e);

static BOOL xfersend__wimpevents(dbox d, void *ev, void *handle);

static BOOL xfersend__unknowns (wimp_eventstr *e, void *handle)
{
  tracef1 ("xfersend__unknowns got event %s\n", report_wimp_event(e->e, &e->data));
  return(xfersend__wimpevents((dbox) handle, (void *) e, NULL));
}

static BOOL xfersend__wimpevents(dbox d, void *ev, void *handle)
{
  wimp_eventstr eventcopy = * (wimp_eventstr *) ev; /* copy event */
  wimp_eventstr * e = &eventcopy;

  tracef1 ("xfersend__wimpevents got event %s\n", report_wimp_event(e->e, &e->data));
  handle = handle;

  switch (e->e)
  {
    case wimp_EUSERDRAG:
    { /* finish my drag */
      tracef0 ("drag event received.\n");
      #if USE_DRAGASPRITE
#ifndef SKS_ACW
        if (xfersend__using_dragasprite)
          dragasprite_stop ();
#else /* SKS_ACW */
      nd_dragstop();
#endif /* SKS_ACW */
      #endif
      wimp_get_point_info (&xfersend__mousestr);
      if (xfersend__mousestr.w == dbox_syshandle(xfersend_.d))
      {
        tracef0 ("drag to same window: has no effect in PipeDream.\n");
        return TRUE;
      }
      if (xfersend__mousestr.w != -1)
      {
        wimp_msgstr msg;

        tracef1 ("drag to window %i: offer data.\n", xfersend__mousestr.w);
        msg.hdr.size = sizeof (wimp_msghdr) + sizeof (wimp_msgdatasave);
        msg.hdr.task = xfersend__mousestr.w;
        msg.hdr.your_ref = 0;
        msg.hdr.action = wimp_MDATASAVE;
        msg.data.datasave.w = xfersend__mousestr.w;
        msg.data.datasave.i = xfersend__mousestr.i;
        msg.data.datasave.x = xfersend__mousestr.x;
        msg.data.datasave.y = xfersend__mousestr.y;
        msg.data.datasave.type = xfersend__filetype;
        msg.data.datasave.estsize = xfersend__estsize;

        {
          int i, tail, namelen;
          char name[256];

          dbox_getfield(d, (dbox_field) xfersend_FName, name, sizeof(name));

          tail = strlen (name); /* point at the zero */
          while (tail > 0 && name[tail-1] != '.' && name[tail-1] != ':')
               tail--;

          namelen = strlen(name);
          for (i = 0; tail <= namelen && i < 211; i++)
            msg.data.datasave.leaf[i] = name[tail++];

          msg.data.datasave.leaf[i+1] = '\0';     /* force termination */
          tracef1 ("suggest leaf '%s'.\n", (int) &msg.data.datasave.leaf[0]);
        }

        wimpt_noerr (wimp_sendwmessage (wimp_ESEND, &msg, xfersend__mousestr.w,
            xfersend__mousestr.i));
        xfersend__msgid = msg.hdr.my_ref; /* filled in by wimp. */
        /* We still get unknown events, so we'll get the reply sometime. */
      }
      else
        /* do nothing */
        tracef0 ("drag to no window: has no effect.\n");

      return TRUE;
    }
    break;

    case wimp_ESEND:
    case wimp_ESENDWANTACK:
      tracef3 ("xfersend msg %s received: your_ref %i, msgid %i\n",
          report_wimp_message(e, FALSE), e->data.msg.hdr.your_ref,xfersend__msgid);

      if (e->data.msg.hdr.your_ref == xfersend__msgid)
        switch (e->data.msg.hdr.action)
        {
          case wimp_MRAMFETCH:
            if (xfersend__sendproc != 0)
            {
              xfersend__fileissafe = FALSE;

              /*He wants to do an in-core transfer, and we can do this. Note
                that this can't be other than the first response, as others
                are grabbed by sendbuf*/
              tracef0 ("ram transfer starting.\n");

              /* Prepare the reply record. */
              xfersend__msg = e->data.msg;
              xfersend__msg.hdr.your_ref = xfersend__msg.hdr.my_ref;
              xfersend__msg.hdr.action = wimp_MRAMTRANSMIT;
              xfersend__msg.data.ramtransmit.addr = e->data.msg.data.ramfetch.addr;
              xfersend__msg.data.ramtransmit.nbyteswritten = 0; /* so far. */
              rcvbufsize = e->data.msg.data.ramfetch.nbytes;

              xfersend__receiver = e->data.msg.hdr.task;
              /*the copy in xfersend__msg.hdr.task is overwritten by the Wimp
                message sending */

              if (xfersend__sendproc (xfersend__savehandle, &rcvbufsize))
              {
                /* get dbox closed */
                winx_send_close_window_request(dbox_window_handle(d));

                /* See sendbuf for all the real work for this case... */

                tracef0 ("The send succeeded; send final RAMTRANSMIT.\n");

                /*We may have transferred some data but not yet told the
                  other end about it. xfersend__msg contains a final RAMTRANSMIT,
                  which does not quite fill his buffer (or we'd have sent it
                  already) thus signalling to him that the transfer is over.*/

                wimpt_noerr (wimp_sendmessage (wimp_ESEND, &xfersend__msg,
                    xfersend__receiver));
              }
              else
                tracef0 ("the send failed.\n");

              if (xfersend__close) xfersend__winclose ();
              return TRUE;
            }
          break;

          case wimp_MPrintFile: /* was dropped on a printer application*/
            /*From RISC O S 3.00, we always bounce this message. Printers will
              respond with a Message_DataSaveAck (if it wants a copy of the
              file), or Message_PrintTypeOdd (if it wants the file to be printed
              now).*/
            return TRUE;

          case wimp_MPrintTypeOdd: /*was dropped on a printer application with
              queue empty - print immediately*/
             if (xfersend__printproc != NULL)
            {
              wimp_t xfersend__printer;
              int res;

              tracef0 ("immediate print request acceptable\n");
              xfersend__fileissafe = FALSE;

              xfersend__printer = e->data.msg.hdr.task;
              xfersend__msg = e->data.msg;
              xfersend__msg.hdr.your_ref = e->data.msg.hdr.my_ref;

              /* get dbox closed */
              winx_send_close_window_request(dbox_window_handle(d));

              res = (*xfersend__printproc) (e->data.msg.data.print.name,
                  xfersend__savehandle);
                  /*reports errors, if any*/

              xfersend__msg.hdr.action = (res >= 0) ? wimp_MDATALOAD : wimp_MWillPrint;

              xfersend__msg.data.print.type = res;
              /* in case it's been saved */

              wimpt_noerr (wimp_sendmessage (wimp_ESEND, &xfersend__msg,
                  xfersend__printer));
              if (xfersend__close) xfersend__winclose ();
            }
            /*If we weren't given a printproc, Printers' message bounces and
              it tries to copy the data away for printing via Message_
              DataSaveAck.*/

            /*In either case, the message has been handled successfully */
            return TRUE;
          break;

#ifdef UNUSED_AS_YET
          case wimp_MPrintError:
            if (e->data.msg.hdr.size > 24)
              werr(FALSE, &e->data.msg.data.chars[4]);
            win_remove_unknown_event_processor (&xfersend__unknowns, 0);
            if (xfersend__close) xfersend__winclose ();
            return TRUE;
          break;
#endif

          case wimp_MDATASAVEOK:
            tracef4 ("datasaveok %i %i %i %i.\n",
                e->data.msg.hdr.size,
                e->data.msg.hdr.task,
                e->data.msg.hdr.your_ref,
                e->data.msg.hdr.my_ref);
            tracef4 ("datasaveok %x %x %x %x.\n",
                e->data.msg.data.words[0],
                e->data.msg.data.words[1],
                e->data.msg.data.words[2],
                e->data.msg.data.words[3]);
            tracef1 ("it's the datasaveok, to file '%s'.\n",
                (int) &e->data.msg.data.datasaveok.name[0]);

            win_remove_unknown_event_processor (xfersend__unknowns, d);
            xfersend_.ukproc = FALSE;

            tracef1 ("save to filename '%s'.\n",
                (int) &e->data.msg.data.datasaveok.name[0]);

            xfersend__fileissafe = e->data.msg.data.datasaveok.estsize > 0;

            xfersend__msg = e->data.msg;
                  /* sets hdr.size, data.w,i,x,y, size, name */
            xfersend__receiver = e->data.msg.hdr.task;
            xfersend__msg.hdr.your_ref = e->data.msg.hdr.my_ref;
            xfersend__msg.hdr.action = wimp_MDATALOAD;

            if (xfersend__saveproc != NULL &&
                xfersend__saveproc (&e->data.msg.data.datasaveok.name[0],
                xfersend__savehandle))
            { tracef0 ("the save succeeded: send dataload\n");

              /* get dbox closed */
              winx_send_close_window_request(dbox_window_handle(d));

              xfersend__msg.data.dataload.type = xfersend__filetype;
              wimpt_noerr (wimp_sendmessage (wimp_ESENDWANTACK, &xfersend__msg,
                  xfersend__receiver));
            }
            else
              /* he has already reported the error: nothing more to do. */
              tracef0 ("save was not successful.\n");

            if (xfersend__close) xfersend__winclose ();
            return xfersend__saveproc != NULL;
          break;
        }
    break;

    case wimp_EACK:
      /*If this is our Message_PrintSave being bounced, then we have to print the
         file now (according to the revised protocol 3 Apr '92). */
      if (e->data.msg.hdr.action == wimp_MPrintSave &&
          e->data.msg.hdr.my_ref == xfersend__msgid)
      {
        tracef0 ("no printer manager - printing direct\n");

#ifdef UNUSED_AS_YET
        if (xfersend__printproc != NULL)
          (void) (*xfersend__printproc) (e->data.msg.data.print.name,
              xfersend__savehandle);
            /*This reports errors itself.*/
        else
          tracef0 ("no printing function supplied\n");

        return TRUE;
#endif
      }

      if (e->data.msg.hdr.action == wimp_MDATALOAD &&
          e->data.msg.hdr.my_ref == xfersend__msgid)
      {
        /* It looks as if he hasn't acknowledged my DATALOAD acknowledge:
           thus it may be a loose scrap file, and must be deleted. */
        _kernel_swi_regs rs;
        tracef0 ("he hasn't ack'd our data load of temp file, so delete the file.\n");
        rs.r[0] = OSFile_Delete; /* doesn't complain if not there */
        rs.r[1] = (int) xfersend__msg.data.dataload.name;
        (void) _kernel_swi(OS_File, &rs, &rs);
        werr(FALSE, msgs_lookup("xfersend1" /*":Bad data transfer, receiver dead"*/));
        return(TRUE);
      }
      break;

    case wimp_EBUT:
      return xfersend__wimp_EBUT(d, e);
  }

  if(xfersend_.oldraw_handler_proc)
    return((* xfersend_.oldraw_handler_proc) (d, ev, xfersend_.oldhandle));

  return FALSE;
}

static int sendbuf__state;

static BOOL sendbuf__unknowns (wimp_eventstr *e, void *h)
{
 h = h;

 tracef4 ("sendbuf__unknowns %d %d %d %d\n",
          e->data.msg.hdr.my_ref, e->data.msg.hdr.your_ref,
          xfersend__msg.hdr.your_ref, xfersend__msg.hdr.my_ref);

 if ( (e->e == wimp_ESENDWANTACK || e->e == wimp_ESEND) &&
     e->data.msg.hdr.your_ref == xfersend__msg.hdr.my_ref &&
     e->data.msg.hdr.action == wimp_MRAMFETCH)
 {
  /* Prepare xfersend__msg as the next RAMTRANSMIT. Most of
  the fields are already set up. */

  xfersend__msg.data.ramtransmit.addr = e->data.msg.data.ramfetch.addr;
  xfersend__msg.data.ramtransmit.nbyteswritten = 0;
  xfersend__msg.hdr.your_ref = e->data.msg.hdr.my_ref;
  rcvbufsize = e->data.msg.data.ramfetch.nbytes;

  tracef2 ("RAMFETCH received: continue with buffer at %x, size %d\n",
          (int) xfersend__msg.data.ramtransmit.addr, rcvbufsize);

  sendbuf__state = 1;
  return TRUE;      /* We've had another RAMFETCH: off we go again */
 }

 if (e->e == wimp_EACK &&
    e->data.msg.hdr.my_ref == xfersend__msg.hdr.my_ref)
 {
  sendbuf__state = 2;
  tracef0 ("xfersend RAMTRANSMIT bounced; set failed state\n");
  return TRUE;/* our message bounced back; give up */
 }

 return FALSE;    /* we don't want it */
}


BOOL xfersend_sendbuf (char *buffer, int size)
{

/* Called by his sendproc when sending things in memory. The
reply record is in xfersend__msg. */

 tracef2 ("xfersend_sendbuf %i %i\n", (int) buffer, size);

 /* Make the data transfer */
 tracef3 ("transfer block of %d from %x to %x\n", size, (int) buffer,
          (int) (xfersend__msg.data.ramtransmit.addr +
            xfersend__msg.data.ramtransmit.nbyteswritten));

 wimpt_noerr (wimp_transferblock (wimpt_task (), buffer, xfersend__receiver,
      xfersend__msg.data.ramtransmit.addr +
      xfersend__msg.data.ramtransmit.nbyteswritten,
      size));

 /* record bytes to be sent to the other end */
 xfersend__msg.data.ramtransmit.nbyteswritten += size;
 rcvbufsize -= size;

 /* if size != 0, there are still bytes to send. */

  if (rcvbufsize > 0) return TRUE;

 tracef1 ("xfersend message has put %d into buffer\n",size);
 /* Tell him that you've done it */
 wimpt_noerr (wimp_sendmessage (wimp_ESENDWANTACK,
      &xfersend__msg,
      xfersend__receiver));

  /* Get his reply. Poll and despatch events until get nack or message */

 sendbuf__state = 0;

 win_add_unknown_event_processor (sendbuf__unknowns, 0);
 do event_process (); while (sendbuf__state == 0);
 win_remove_unknown_event_processor (sendbuf__unknowns, 0);

  /* This exit happens in the cases where the buffers at each end
      are of identical size. So, return for another call to sendbuf, or
      so that the sendbuf procedure can return. */

 return sendbuf__state != 2;  /* OK unless state = broken */
}

/* make and set a file icon of the appropriate type */

static void xfersend__fileicon(dbox d)
{
  wimp_w w = dbox_syshandle(d);
  fileicon(w, xfersend_FIcon, xfersend__filetype);
  (void) wimpt_complain(wimp_set_icon_state(w, xfersend_FIcon, (wimp_iconflags) 0, (wimp_iconflags) 0));
}

BOOL xfersend_x(
  int                filetype,
  char *             name,
  int                estsize,
  xfersend_saveproc  saveproc,
  xfersend_sendproc  sendproc,
  xfersend_printproc printproc,
  void *             savehandle,
  dbox               d,
  xfersend_clickproc clickproc,
  void *             clickhandle)
{
  dbox_field f;
  char filename[216];

  xfersend__filetype      = filetype;
  xfersend__estsize       = estsize;
  xfersend__saveproc      = saveproc;
  xfersend__savehandle    = savehandle;
  xfersend__sendproc      = sendproc;
  xfersend__printproc     = printproc;
  xfersend_.d             = d;
  xfersend_.clickproc     = clickproc;
  xfersend_.clickhandle   = clickhandle;

  /* set up icons in dbox */
  dbox_setfield(d, (dbox_field) xfersend_FName, name);
  xfersend__fileicon(d);

  dbox_raw_eventhandlers(d, &xfersend_.oldraw_handler_proc, &xfersend_.oldhandle);

  dbox_raw_eventhandler(d, xfersend__wimpevents, NULL);

  while ((f = dbox_fillin(d)) != dbox_CLOSE)
  {
    if (f == dbox_OK)
    {
      dbox_getfield(d, (dbox_field) xfersend_FName, filename, sizeof(filename));

      /* complain if not rooted */
      if (!xfersend__file_is_rooted(filename))
      {
        werr(FALSE, msgs_lookup("saveas1" /*":To save, drag the icon to a directory viewer"*/));
        continue;
      }

      xfersend__fileissafe = TRUE;        /* bog standard save to real file */

      if (xfersend__saveproc (filename, xfersend__savehandle)  &&
          !dbox_persist())
      {
        tracef0 ("file saved ok but not persisting - terminate xfersend loop\n");
        break;
      }
    }
    else if (f == xfersend_.cancel_button)
    {
      tracef0 ("click on cancel button terminates xfersend loop\n");
      break;
    }
    else if (xfersend_.clickproc)
    {
      filetype = xfersend__filetype;
      xfersend_.clickproc (d, f, &xfersend__filetype, xfersend_.clickhandle);
      if(filetype != xfersend__filetype)
        xfersend__fileicon(d);
    }
    else
    {
      tracef0 ("strange click terminates xfersend loop\n");
      break;
    }
  }

  tracef0 ("xfersend loop done\n");

  xfersend_.cancel_button = dbox_OK; /* reset to default in any case */

  /* Might have escaped the loop above still owning unknown events. */
  if (xfersend_.ukproc != NULL)
  {
    xfersend_.ukproc = FALSE;
    win_remove_unknown_event_processor(xfersend__unknowns, d);
  }

  return (f == dbox_OK);
}

#ifndef SKS_ACW

BOOL xfersend (int filetype, char *filename, int estsize,
   xfersend_saveproc saver, xfersend_sendproc sender, xfersend_printproc printer,
   wimp_eventstr *e, void *handle)

{
   wimp_wstate wstate;
   wimp_icon icon;
   wimp_w w = e->data.but.m.w;
   wimp_mousestr mouse_str;
   int
      x_limit = bbc_vduvar (bbc_XWindLimit) << bbc_vduvar (bbc_XEigFactor),
      y_limit = bbc_vduvar (bbc_YWindLimit) << bbc_vduvar (bbc_YEigFactor),
      x0, y0, x1, y1;
   int screen_x0;
   int screen_y0;
#if USE_DRAGASPRITE
   char *name;
   sprite_area *area;
#endif

   xfersend__saveproc = saver;
   xfersend__sendproc = sender;
   xfersend__printproc = printer;
   xfersend__filetype = filetype;
   xfersend__estsize = estsize;
   xfersend__savehandle = handle;
   if (xfersend__filename == 0) xfersend__filename = malloc (256);
   if (filename == 0)
      strcpy (xfersend__filename, msgs_lookup (MSGS_xfersend2));
   else
      strncpy (xfersend__filename,filename,256);

   tracef0 ("Initiate a drag.\n");

   /*Find screen origin*/
   wimp_get_wind_state (w, &wstate);
   screen_x0 = wstate.o.box.x0 - wstate.o.x;
   screen_y0 = wstate.o.box.y1 - wstate.o.y;

   /*Get initial icon position*/
   wimp_get_icon_info (w, e->data.but.m.i, &icon);
   x0 = icon.box.x0 += screen_x0;
   y0 = icon.box.y0 += screen_y0;
   x1 = icon.box.x1 += screen_x0;
   y1 = icon.box.y1 += screen_y0;

#if USE_DRAGASPRITE
   if (xfersend__using_dragasprite ())
   {
      /* Find the sprite name and area */
      if (icon.flags & wimp_ITEXT)
      {  area = wimp_spritearea;
         name = strstr ( icon.data.indirecttext.validstring, ";s" ) + 2;
      }
      else
      {  area = icon.data.indirectsprite.spritearea;
         name = icon.data.indirectsprite.name;
      }

      wimpt_complain (dragasprite_start (dragasprite_HJUSTIFY_CENTRE |
          dragasprite_VJUSTIFY_CENTRE | dragasprite_BOUNDTO_SCREEN |
          dragasprite_BOUND_POINTER | dragasprite_DROPSHADOW_PRESENT,
          area, name, &icon.box, NULL));
   }
   else   /* not using dragasprite */
   {
      /*Get pointer position to allow icon to be dragged partially off-screen */
      int mouse_x,mouse_y;
      wimp_dragstr dr;
      wimp_get_point_info (&mouse_str);
      mouse_x = mouse_str.x;
      mouse_y = mouse_str.y;

      /*Set up drag*/
      dr.window    = w; /*not relevant*/
      dr.type      = wimp_USER_FIXED;
      dr.box.x0    = x0;
      dr.box.y0    = y0;
      dr.box.x1    = x1;
      dr.box.y1    = y1;
      dr.parent.x0 = x0 - mouse_x; /*Expanded parent by box overlap*/
      dr.parent.y0 = y0 - mouse_y;
      dr.parent.x1 = x1 - mouse_x + x_limit;
      dr.parent.y1 = y1 - mouse_y + y_limit;
      wimp_drag_box (&dr);
   }
#else
   /*Get pointer position to allow icon to be dragged partially off-screen. */
   wimp_get_point_info (&mouse_str);
   mouse_x = mouse_str.x;
   mouse_y = mouse_str.y;

   /*Set up drag*/
   dr.window    = w; /*not relevant*/
   dr.type      = wimp_USER_FIXED;
   dr.box.x0    = x0;
   dr.box.y0    = y0;
   dr.box.x1    = x1;
   dr.box.y1    = y1;
   dr.parent.x0 = x0 - mouse_x; /*Expanded parent by box overlap*/
   dr.parent.y0 = y0 - mouse_y;
   dr.parent.x1 = x1 - mouse_x + x_limit;
   dr.parent.y1 = y1 - mouse_y + y_limit;
   wimp_drag_box (&dr);
#endif

   win_add_unknown_event_processor (&xfersend__unknowns, NULL);
   return TRUE;
}

BOOL xfersend_print (int type, char *filename, int estsize,
   xfersend_saveproc saver, xfersend_sendproc sender, xfersend_printproc printer,
   void *handle)

{
   xfersend__saveproc = saver;
   xfersend__sendproc = sender;
   xfersend__printproc = printer;
   xfersend__filetype = type;
   xfersend__estsize = estsize;
   xfersend__savehandle = handle;
   if (xfersend__filename == NULL &&
        (xfersend__filename = malloc (256)) == NULL)
      return FALSE;

   sprintf (xfersend__filename, "%.256s", filename != 0? filename:
       msgs_lookup (MSGS_xfersend2));

   /*Broadcast a PrintSave message. The printer will reply with PrintFile
     (which we ignore in RISC O S 3.00)), then DataSaveAck which is caught by
      xfersend__unknowns ().*/
   xfersend__msg.hdr.size = sizeof (wimp_msghdr) +
         sizeof (wimp_msgdatasave);
   xfersend__msg.hdr.your_ref = 0;
   xfersend__msg.hdr.action = wimp_MPrintSave;
   xfersend__msg.data.datasave.estsize = estsize; /*this is PrintSave not
         DataSave, but it matches (apart from w, i, x, y not used)*/
   xfersend__msg.data.datasave.type = type;
   sprintf (xfersend__msg.data.datasave.leaf, "%.*s",
         sizeof xfersend__msg.data.datasave.leaf - 1, xfersend__filename);

   tracef1 ("sending Message_PrintSave \"%s\"\n",
         xfersend__msg.data.datasave.leaf);
   wimpt_noerr (wimp_sendmessage (wimp_ESENDWANTACK, &xfersend__msg, NULL));

   tracef1 ("message id is %d\n", xfersend__msg.hdr.my_ref);
   xfersend__msgid = xfersend__msg.hdr.my_ref; /*Filled in by WIMP.*/

   tracef0 ("adding unknown event processor\n");
   win_add_unknown_event_processor (&xfersend__unknowns, NULL);

   return TRUE;
}

#ifndef UROM
BOOL xfersend_pipe(int filetype, char *filename, int estsize,
                     xfersend_saveproc saver,
                     xfersend_sendproc sender,
                     xfersend_printproc printer,
                     void *handle, wimp_t task)
{
      xfersend__saveproc = saver;
      xfersend__sendproc = sender;
      xfersend__printproc = printer;
      xfersend__filetype = filetype;
      xfersend__estsize = estsize;
      xfersend__savehandle = handle;
      if (xfersend__filename == 0) xfersend__filename = malloc (256);
      if (filename == 0)
        strcpy (xfersend__filename, msgs_lookup (MSGS_xfersend2));
      else
        strncpy (xfersend__filename,filename,256);

        {
        wimp_msgstr msg;

        msg.hdr.size = sizeof (wimp_msghdr) + sizeof (wimp_msgdatasave);
        msg.hdr.task = task;
        msg.hdr.your_ref = 0;
        msg.hdr.action = wimp_MDATASAVE;
        msg.data.datasave.w = 0; /* kludge, it's not being sent to a window!! */
        msg.data.datasave.i = 0;
        msg.data.datasave.x = 0;
        msg.data.datasave.y = 0;
        msg.data.datasave.type = xfersend__filetype;
        msg.data.datasave.estsize = xfersend__estsize;
        {
          int i, tail, namelen;
          char name[256];
          if (xfersend__filename == 0) xfersend__filename = malloc (256);
          strncpy (name,xfersend__filename,  256);
          tail = strlen (name); /* point at the zero */
          while (tail > 0 && name[tail-1] != '.' && name[tail-1] != ':')
               tail--;

          namelen = strlen(name);
          for (i = 0; /*i <= 10*/ tail <= namelen && i < 211; i++)
            msg.data.datasave.leaf[i] = name[tail++];
          msg.data.datasave.leaf[i+1] = '\0';     /* force termination */
          tracef1 ("suggest leaf '%s'.\n", (int) &msg.data.datasave.leaf[0]);
        };
        wimpt_noerr (wimp_sendmessage (wimp_ESEND, &msg, task));
        xfersend__msgid = msg.hdr.my_ref; /* filled in by wimp. */
      }

      win_add_unknown_event_processor (&xfersend__unknowns, 0);
      return TRUE;
}
#endif

#endif /* SKS_ACW */

BOOL xfersend_file_is_safe(void)
{
  return xfersend__fileissafe;
}

void xfersend_set_fileissafe(BOOL value)
{
  xfersend__fileissafe = value;
}

void xfersend_clear_unknowns(void)
{
   win_remove_unknown_event_processor (sendbuf__unknowns, 0);
   win_remove_unknown_event_processor (&xfersend__unknowns, 0);
}

int xfersend_read_last_ref(void)
{
  return xfersend__msgid;        /* my_ref of last DataSave message */
}

extern void xfersend_set_cancel_button(dbox_field f)
{
  xfersend_.cancel_button = f; 
}

/* Starts a drag using DragASprite */

static int dragging_using_dragasprite = FALSE;

static os_error *nd_dragstart(wimp_dragstr dr, int filetype)
{
    os_regset r;
    os_error * e;
    char icon_name[32];

    fileicon_name(icon_name, filetype);

    r.r[0]=0x85;
    r.r[1]=1;
    r.r[2]=(int) icon_name;
    r.r[3]=(int) &(dr.box);
    r.r[4]=0;
    e=os_swix(DragASprite_Start, &r);

    if(!e)
        dragging_using_dragasprite = TRUE;

    return(e);
}

/* Finishes a DragASprite call */

static void nd_dragstop(void)
{
    if(dragging_using_dragasprite)
    {
        dragging_using_dragasprite = FALSE;

#ifdef SKS_ACW
#if defined(NORCROFT_INLINE_SWIX_NOT_YET) /* this _swix not yet optimised */
        (void) _swix(DragASprite_Stop, 0);
#else
        os_swix0(DragASprite_Stop);
#endif
#else
        os_swi0(DragASprite_Stop);
#endif
    }
}

static BOOL xfersend__wimp_EBUT(dbox d, wimp_eventstr *e)
{
  tracef1 ("xfersend EBUT &%x\n", e->data.but.m.bbits);

  if( (e->data.but.m.bbits == wimp_BDRAGLEFT) &&
      (e->data.but.m.i     == xfersend_FIcon) )
  {
    wimp_dragstr dr;
    wimp_wstate wstate;
    wimp_icon icon;
    wimp_w w;

    win_add_unknown_event_processor(xfersend__unknowns, d);

    w = dbox_syshandle(d);

    tracef0 ("Initiate a drag.\n");

    xfersend_.ukproc = TRUE;

    if(wimpt_complain(wimp_get_wind_state(w, &wstate))) return(FALSE);
    if(wimpt_complain(wimp_get_icon_info(w, xfersend_FIcon, &icon))) return(FALSE);
    dr.window = w; /* not relevant */
    dr.type   = wimp_USER_FIXED;
    dr.box.x0 = wstate.o.box.x0 - wstate.o.x + icon.box.x0;
    dr.box.y0 = wstate.o.box.y1 - wstate.o.y + icon.box.y0;
    dr.box.x1 = wstate.o.box.x0 - wstate.o.x + icon.box.x1;
    dr.box.y1 = wstate.o.box.y1 - wstate.o.y + icon.box.y1;
    dr.parent.x0 = 0;
    dr.parent.y0 = 0;
    dr.parent.x1 = bbc_vduvar(bbc_XWindLimit) << bbc_vduvar(bbc_XEigFactor);
    dr.parent.y1 = bbc_vduvar(bbc_YWindLimit) << bbc_vduvar(bbc_YEigFactor);

    /* nd 19-07-1996 Added in support for DragASprite */
    if(!xfersend__using_dragasprite() || (NULL != nd_dragstart(dr, xfersend__filetype)))
        (void) wimpt_complain(wimp_drag_box(&dr));

    return TRUE;
  }
  else
    tracef0 ("not a drag on the file icon - ignored\n");

  return FALSE;
}

/* end xfersend.c */
