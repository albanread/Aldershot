# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Code: the BOX's programmer's editor.  It is the shared shell
(rosapp.mojo) over an engine of its own: a rope for each document's text,
syntax colouring, and the language servers of the box.

One window holds many documents, as tabs across its top: a file dropped on
!Code's icon, or opened from it, goes into a new tab, and Ctrl-Tab moves
between them.  Files of a quarter of a megabyte and more are edited as
quickly as small ones.  Mojo, C and C++, BBC BASIC, Lua and Tcl are
coloured as they are written; a BBC BASIC program is listed to be edited and tokenised
again when it is saved.

Mojo, C and C++ files are backed by their language servers -- the box's
mojo-lsp-server and clangd -- which mark errors and warnings as you type,
and offer completion (Ctrl-Space, or after a full stop), the parameters of
a call as it is typed (Ctrl-Shift-Space to ask), information on a name (F1), renaming (F2), going to where a name is defined (F4) and where
it is used (Shift-F4), formatting (F5) and an outline of the file.  The
properties pane has the open files, Find and Change, and what the language
server knows.  Quick fix (in the Code menu and pane) makes the fix the
language server offers for the problem at the caret, such as taking out an
#include that is not used.  A file changed on disc by another program is
read again when it has no changes here; when it has, they are kept and you
are told.  Makefiles are coloured, and two tabs of the same name show their
directories.  Brackets close themselves as they are typed, a closing one
typed before its twin steps over it, and Backspace between an empty pair
takes both.  Find references and several definitions are listed at the
caret, to choose from.  For C, clangd indexes the project in the
background (in its .cache directory), so going to a definition reaches
the .c file, and references and renaming cover files that are not open;
Ctrl-F4 goes between a C file and its header; Ctrl-F8 makes the fix the
status line offers.  Save all (Tabs menu, or Ctrl-K then S) saves every
changed file, as after renaming across files; a header saved has the open
C files checked again.  Create a file lsp-log in !Code to record what is
said to the language servers and what they answer.

Tcl has no language server; !Code knows its commands itself, Tcl's own and
the box's packages' (riscos, wimp, declaritive, rosapps, workflow, agent),
for completion (Ctrl-Space) and information (F1), and marks a script's
unbalanced braces, brackets and quotes as it is written.

A single file is built and run on its own, with no project: Build and Run
(Ctrl-Shift-B and Ctrl-F5, or Build file and Run file in the Project menu)
work on the file in front when it is not one of the project's.  A C or
Mojo file with a main is compiled by *CC or *Mojo into a program beside it
(c.hello and hello/c make hello) and run; Lua, BBC BASIC, Tcl and Obey
files are run as they are.  Build on a Lua file parses it, and on a Tcl
script checks it.  An error a Lua or Tcl script stops at is a problem F8
goes to.

The box's other languages have no language server; !Code colours them,
knows their words for F1, and builds and runs a file of each: FasterBASIC
(&A75, *FasterBASIC; Build compiles it), Modula-2 (&A7B, *Modula2), BCPL
(&A78, *BCPL), Forth (&A77, *F66, the Arm box only), Python (&A73, *Python3;
Build compiles it), Lisp (.lisp, !Lisp's NCL; Build is its --check) and
Smalltalk (&A70, run in !Smalltalk).  Build on a BBC BASIC program compiles
it with *RosBas into a program beside it.  The errors each reports with a
line are problems F8 goes to.  A program that reads the keyboard is given
a line from the Project pane's Input while it runs.

The debugger stops C and Mojo programs, Python scripts (Python's own
bdb, run by *Python3 in the task window) and Lua scripts (LuaDebug, a line
hook, run by *LuaJIT -joff): breakpoints, steps, the call stack and the
variables.  FasterBASIC, Modula-2, BCPL and Forth are
compiled into memory inside the box as they are run, where the box's
debugger cannot stop them; F5 runs them, and an error's line is a problem.

A directory dropped on !Code is a project: a directory with a Makefile,
which BoxTools' *Make builds, or a program in one of the languages that
runs one file, whose first file (main.*, a Modula-2 program module, a BCPL
file with START) Build and Run work on.  The Project pane lists its files, and builds
it (F9), runs it (F10) and cleans it, in a task window whose output is the
Output tab; F8 goes to each error the build found.  A directory with no
Makefile is made a new project, from a template that builds as it stands:
C, Mojo, Modula-2, BCPL, FasterBASIC, Python, Lua, Tcl, Forth or Lisp.  *Make tells clangd how each C file is compiled
(compile_commands.json).

The keys are Visual Studio Code's wherever RISC OS leaves them free:
Ctrl-P goes to a file and Ctrl-Shift-O to a symbol (each a list that
narrows as you type), Ctrl-G to a line, Alt-Left and Alt-Right back and
forward; Alt-click or F4 goes to a definition (F12 is the command
line's, and Ctrl-click is MENU), Shift-F4 finds the references, F2 renames, F8 goes to the next
problem; Ctrl-K Ctrl-C comments lines out (RISC OS has no Ctrl-/), Alt-Up and Alt-Down move them (Shift:
copy them), Ctrl-Shift-K deletes them, Ctrl-Return opens a line below
(Shift: above), Ctrl-L selects the line, Ctrl-D the word and then the
next place it is, Ctrl-Shift-\\ goes to the matching bracket, Ctrl-C and
Ctrl-X with nothing selected take the whole line, Ctrl-Up and Ctrl-Down
move the view; Ctrl-Shift-B builds, Ctrl-F5 runs, Ctrl-Shift-F formats.

The debugger builds the project, gives the
program's breakpoints to the box's debugger, and runs the project's
"debug" target (DebugRun <program>, which the C template has) in its task
window, so the desktop goes on while the program is stopped.  A desktop
program's target is DebugRun -w <program>: it runs as a task of its own,
and while it is stopped its windows stay on the screen.  F9 puts a
breakpoint on a line (a red dot in the gutter), F5 starts the program or
lets it go on, F10 steps over a line, F11 into it, Shift-F11 out of the
function, F6 pauses it, Shift-F5 stops it.  Where it stopped is marked, and the Debug
pane shows the call stack, the variables of the frame chosen and the
registers.  C compiled with -g is debugged by its source.  MENU puts the caret where it is clicked, as in Code, and the
menu's items that make no sense there are shaded."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Code", "ROSAPPS' programmer's editor, with language servers", "0.2 (20 Oct 2026)", "!code")
        # Mojo files are its own; text and BASIC files come to it by being
        # dropped on it (Edit keeps text, BASIC keeps its programs)
        cfg.types.append(0xA6E)
        cfg.imports.append(0xFFF)
        cfg.imports.append(0xFFB)
        cfg.imports.append(0xFD1)
        cfg.imports.append(0xFE1)               # a Makefile
        cfg.imports.append(0xA6D)               # Tcl
        cfg.imports.append(0xA6C)               # a workflow, Tcl
        cfg.imports.append(0x18C)               # Lua
        cfg.imports.append(0xA7B)               # Modula-2: a program or implementation module
        cfg.imports.append(0xA7C)               # Modula-2: a definition module
        cfg.imports.append(0xA78)               # BCPL
        cfg.imports.append(0xED1)               # Scheme (EdSProg)
        cfg.imports.append(0xA75)               # FasterBASIC
        cfg.imports.append(0xA77)               # Forth (MF66)
        cfg.imports.append(0xA73)               # Python 3
        cfg.imports.append(0xA70)               # Smalltalk
        cfg.imports.append(0x1000)              # a directory: the project
        cfg.start = String("new")
        cfg.save_type = 0xFFF
        cfg.save_leaf = String("Program")
        cfg.tabs = True                     # one window, many tabs
        cfg.viewport = True                 # the engine draws only what is in view
        cfg.pastes_text = True
        cfg.dev_keys = True                 # F3 finds again, Ctrl-P is the quick pick
        cfg.menu_context = True             # MENU shades what makes no sense where it is
        cfg.props = True
        cfg.geometry = False                # the inspector says where the caret is
        cfg.view_toggles = False
        cfg.new_line = String("new")
        cfg.menu_names.append(String("Code"))
        cfg.menu_items.append(String("Go to definition,Find references,Rename...,Information,Complete,Format,"
                                     + "Outline,-Comment out,Go to line...,-Go to file...,Go to symbol...,"
                                     + "-Back,Forward,-Quick fix,Header or source"))
        cfg.menu_names.append(String("Tabs"))
        cfg.menu_items.append(String("New tab,Close tab,Next tab,Previous tab,-Save all"))
        cfg.menu_names.append(String("Project"))
        cfg.menu_items.append(String("Build,Run,Build all,Clean,Stop,-Next error,Previous error,-Compile flags,-Build file,Run file"))
        cfg.menu_names.append(String("Debug"))
        cfg.menu_items.append(String("Start debugging,Continue,Step over,Step into,Step out,Stop debugging,"
                                     + "-Toggle breakpoint,Clear breakpoints"))
        cfg.export_names.append(String("text"))
        cfg.export_types.append(0xFFF)
        cfg.export_exts.append(String("txt"))
        var desk = Desktop("Code")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Code")
