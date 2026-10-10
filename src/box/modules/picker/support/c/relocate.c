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
/*relocate.c - change module context to allow inter-module procedure calls*/

/* ROSGD's version (the original is support/c/relocate, beside it).
 *
 * On RISC OS the shared C library finds a module's statics through two
 * relocation words at the base of the SVC stack (sp rounded down to a
 * megabyte), and a call from one C module into another's code -- a
 * colour model's entries, or a model calling back into the picker --
 * must swap them for the callee's and back: that is what the original
 * does.  In the box there are no such words (the SVC stack's base is a
 * guard page, and reading it aborted ColourPicker_OpenDialogue, #63): an
 * x32 module's library statics are found through its static base, %gs
 * (x18 in the Apple Silicon box), which only the runtime writes -- on
 * every entry into a module's code, its own range's base (rosgd/include/
 * rosgd/capp.h).  The picker's models (c/rgb.c, c/hsv.c, c/cmyk.c) are in
 * the picker, so the base in force is already theirs, and there is
 * nothing to swap: begin and end leave the frame empty.  A model in
 * another x32 module would run with the picker's base; none exists. */

#include "relocate.h"
#include "trace.h"

/*------------------------------------------------------------------------*/
void relocate_begin (void *workspace, relocate_frame *frame)

{  tracef ("relocate_begin\n");

   (void) workspace;
   frame->client_offset = 0;
   frame->lib_offset = 0;
}
/*------------------------------------------------------------------------*/
void relocate_end (relocate_frame *frame)

{
   (void) frame;

   tracef ("relocate_end\n");
}
