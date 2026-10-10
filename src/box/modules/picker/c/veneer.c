/* veneer.c -- the module's veneer layer, in C for the box's x32 modules
 * (written for the oslcr integration, 29 September 2026; it replaces the
 * original s/veneer.s, which the Apache drop does not carry). No OSLib was
 * read.
 *
 * The Wimp filters are entered by the dispatcher's nested x32 entry, the
 * register block as one argument (include/rosgd/capp.h, "entries into x32
 * code"): R0 the event, R1 the poll block, R12 the handle the filter was
 * registered with -- dialogue.c's dialogue_task_list. They call main.c's
 * main_pre_filter and main_post_filter, as s/veneer.s did. */
#define TRACE 1   /* trace.h declares its interface, not its SKIP macros */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel.h"

#include "dialogue.h"
#include "main.h"
#include "task.h"
#include "trace.h"
#include "wimp.h"

/* ---- the Wimp filters ---- */

/* The filters' contract is the Filter Manager's (the Wimp's): a pre-filter
 * gets R0 the poll mask and gives back the mask to poll with; a post-filter
 * gets R0 the event and gives back -1 to claim it or the event code to pass
 * it on.  main.c's main_pre_filter and main_post_filter keep that contract,
 * as s/veneer.s called them: the pre-filter widens the mask to the events
 * the picker's dialogues need and keeps the task's own, the post-filter
 * hands the event round the dialogues and swallows it if the task had
 * masked it out.  (An earlier version here returned 0 or 1 from the
 * post-filter, which the Wimp read as Null_Reason_Code or Redraw -- no
 * dialogue was ever redrawn -- and passed the mask as an event.) */
void veneer_pre_filter (int regs[17])
{  regs[0] = main_pre_filter ((bits) regs[0], (wimp_block *) regs[1], (wimp_t) regs[2],
         (dialogue_task_list) regs[12]);
}

void veneer_post_filter (int regs[17])
{  regs[0] = main_post_filter (regs[0], (wimp_block *) regs[1], (wimp_t) regs[2],
         (dialogue_task_list) regs[12]);
}

/* ---- muldiv: a*b/c, the intermediate 64-bit ---- */

int muldiv (int a, int b, int c)
{  return (int) (((long long) a*b)/c);
}

/* ---- trace: to the console, and only when the trace variable is set, as
 * the original's support library had it ---- */

static osbool trace_on = FALSE;

os_error *trace_initialise (char *var)
{  trace_on = getenv (var) != NULL;
   return NULL;
}

os_error *trace_terminate (void)
{  trace_on = FALSE;
   return NULL;
}

static void write_console (char *s)
{  for (char *p = s; *p != '\0'; p++)
   {  _kernel_swi_regs r;
      r.r[0] = (unsigned char) *p;
      _kernel_swi (0x00 /* OS_WriteC */, &r, &r);
   }
}

void trace_f (char *file, int line, char *format, ...)
{  if (!trace_on)
      return;

   char s [500];
   int n = snprintf (s, sizeof s - 2, "%s:%d: ", file, line);
   if (n < 0)
      return;
   va_list list;
   va_start (list, format);
   vsnprintf (s + n, sizeof s - 1 - n, format, list);
   va_end (list);
   write_console (s);
}

void trace_vdu (char *bytes, int count)
{  if (!trace_on)
      return;

   for (int i = 0; i < count; i++)
   {  _kernel_swi_regs r;
      r.r[0] = (unsigned char) bytes [i];
      _kernel_swi (0x00, &r, &r);
   }
}

void trace_wait (char *file, int line, int t)
{  NOT_USED (file) NOT_USED (line) NOT_USED (t)
}
