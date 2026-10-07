# !Write: a word processor for the box

`!Write` is a Toolbox application over the Paige text engine
([ports/paige](../paige/)).  Paige keeps the document -- its text, styles,
selection and undo -- and lays it out; Write is the desktop round it.

    make write                  # from rosgd/: the Intel box (x32)
    make ARCH=aarch64 write     # the Apple Silicon box (A64X32)

builds `build/ports/write/!Write` (or `build/aarch64/...`), which
`make usershare` puts in Apps.  `ports/write/build.sh` builds the engine
first (`ports/paige/build.sh`), then the application, linked by roscc
against the box's C library (`--clib`).

Plain `make` builds it too, after the ROM, when Write's sources, the
machine layer's or the engine's have changed; `tools/build-box.sh` checks
that the share has it.  The engine's source is HERMES-Paige at `cddd954`
with `ports/paige/patches/engine-32bit-and-platform.patch` applied, which
ports/paige/build.sh makes in `.cache/paige` (cloned from
`third_party/Paige` when that is here) and makes again when the patch
changes.  `PAIGE=dir` builds another tree as it stands.

`disc/Apps/!Write` is committed: the x32 build, which the Intel box's share
and the PC image take.  `make write-disc` makes it again from the sources;
run it and commit the result when Write changes.  On the Apple Silicon
box `make usershare` puts the A64X32 build in its place, or leaves Write
out when there is none (an x32 image cannot run there).

## What it does

- Select on the icon bar icon: a new document.  A Write document or a
  text file dropped on the icon opens in a window of its own; a Write
  document double-clicked opens likewise.
- In a document: typing at the caret; Select places the caret, a drag
  selects, a double-click selects a word, Adjust extends.  Backspace,
  Delete (forwards), Return, Tab, the arrows (Shift-Left and Shift-Right
  select), Home and Copy (the start and the end).
- Keys: ^Z undo (and again, redo), ^X ^C ^V cut, copy and paste, ^A
  select all, ^B bold, ^U underline, ^S save.
- The clipboard is the desktop's (RISC OS's global clipboard protocol):
  Cut and Copy claim it (Message_ClaimEntity), and another application's
  Paste gets the selection (Message_DataRequest answered with DataSave,
  then the data through the file its DataSaveAck names) -- as Write's own
  format when it asks for that, else as text.  Write's Paste uses its own
  copy while the clipboard is Write's, and otherwise asks for it with
  Message_DataRequest (Write's format, then text) and puts in what comes
  at the caret.  When another application claims the clipboard Write
  drops its copy.  A file dropped into a document (Write's own or text)
  goes in at the caret; dropped on the icon, it opens.
- The toolbar, along the top of the window inside it: bold, italic and
  underline (as the Style menu: on, or off where they are on throughout
  the selection); flush left, centred, flush right and justified, for the
  paragraphs the selection touches; and the paragraph's style, Normal
  (12 point) or Heading 1, 2 or 3 (24, 18 and 14 point bold), from its
  pop-up menu.  Its buttons show the selection as it is: a style's button
  is in where the style is on throughout, the paragraph's alignment's is
  in, and the style's name is shown, or nothing for any other size.  Each
  is undone by ^Z.  It is a Toolbox toolbar (the Doc window's internal
  top-left, the Tools window); the Window module keeps it in place.  Its
  B, I and U are painted as Write starts, in the Font Manager's
  anti-aliased Trinity (Bold, Italic, Medium with a rule), into the
  sprites the Toolbox loaded from Sprites; tools/mksprites.py's pixel
  letters are what shows if the fonts cannot be found.
- Menu: File (Save, Save as, Export RTF, Export HTML, Export text), Edit, Style (plain, bold,
  italic, underline), Size (8 to 36 point), Font (the Toolbox's font
  menu: the family; Style chooses the face) and Spell.
- Spell: the word at the caret, checked against the dictionary, and up
  to eight suggestions, best first, to replace it with (a click on one).
  Next misspelling (^N) selects the next word from the caret the
  dictionary has not.  Mark as you type (on to begin with) draws a red
  wavy line under every word on the screen the dictionary has not, but
  the one the caret is at the end of: a word is marked once it is done.
  Learn 'word' adds the word at the caret to the user's own words, and
  it is right from then on; on one of those, Forget 'word' takes it away.
- Write's own documents are Paige's file format, file type &0A0 (not yet
  allocated: provisional), keeping styles.  RTF (&C32) is read and
  written by Paige's RTF codecs (ports/paige): fonts, sizes, bold, italic,
  underline, colours, paragraphs and their spacing; an RTF document opened
  in Write is saved back as RTF.  Export RTF and Export text write the
  document as either; RTF is also a clipboard format.
- Print (File > Print..., ^P): the Toolbox's print dialogue (all pages or
  from and to, copies), then a job through PDriver to printer: -- the
  box's driver makes a PDF and sends it where *PrintTo says.  The document
  is cut into pages at lines' tops to fit the paper PDriver_PageSize gives,
  inside the window's 3/4-inch margins; each page is drawn as the screen
  is, less the selection and the misspelling marks.  The box's Font
  Manager does not yet hand Font_Paint to the printer driver
  (PDriver_FontSWI), so text comes out misplaced and small until #160 is
  done; Write needs nothing more then.
- HTML (&FAF) likewise, through Paige's HTML codecs: headings, bold,
  italic, underline, font colours and sizes, lists, paragraphs.  A page
  dropped on Write opens (double-clicking one is still the browser's);
  dropped in a document it goes in at the caret; Export HTML writes one;
  HTML is a clipboard format after RTF.  Closing a changed document asks first (DCS);
  so does Quit, and a desktop shutdown.

## Spelling

The dictionary is ports/rossqlite's: SymSpell's English frequency list
(82,769 words) prepared as two SQLite tables, `disc/Resources/
Dictionaries/spelldict` on the box's disc, read through SQLite over the
RISC OS VFS, linked into Write (`src/spell.c`).  A check is one query
(about 1.3 ms); suggestions are the words filed under the word and under
each of its one-letter deletes, kept within Damerau-Levenshtein 1 --
SymSpell's distance-1 lookup -- and ranked by frequency, the word's capital
kept.  The dictionary is in lower case without apostrophes: a word with
one is right when its stem is and its ending is 's 't 'll 're 've 'd or 'm.

Marking checks every word in each redraw rectangle; each word's verdict
is kept (spell_check_cached), so the dictionary is asked once a word.
SQLite's page cache is held to 1 MB: its default grows past the
application's slot, an allocation fails, and the lookup with it (SQLite
reports SQLITE_INTERRUPT) -- a failed lookup counts as right, never as a
misspelling.

The user's own words are a text file, one word to a line, in lower case:
`Choices:Write.Words` (the box disc's !Boot sets Choices$Write to its
Choices directory; Write makes Choices.Write when it first writes), or
!Write.Words when there is no Choices.  Write reads it as it starts and
writes it whole after each Learn or Forget; a word in it is right, and so
is one of its forms with an apostrophe ending.

Write finds the dictionary through `Write$Dictionary` (its !Boot sets it,
a macro over the box disc's `BoxDisc$Dir`), else the disc's
Resources.Dictionaries beside the application (Apps.!Write, or !Write at
the root), else `!Write.Dictionary`.

## Layout

    src/main.c        the Toolbox front end: objects, events, the Wimp's
                      redraw, clicks, keys and messages
    src/doc.c, doc.h  the document: every Paige call is here
    src/spell.c, .h   the spelling: the prepared dictionary by SQL
    tools/mkwrite.py  the Res file's objects (tools/mkres.py)
    tools/mksprites.py  !Sprites: the application's and the file type's;
                      Sprites: the toolbar's buttons
    res/!Write/       !Boot, !Run, !Help, Messages
    test/engine.c     the document layer without the desktop
    test/engine.py    runs it in a headless box: typing, styles, undo,
                      the word at the caret, save, export, load, the
                      lines' breaks, spelling checks and suggestions,
                      the next misspelling replaced, headings and
                      alignment

## Coordinates

A document measures in points, y down.  The window's work area is the
document at 5/2 OS units the point, inset by 48 OS units (and the
toolbar's 64 more at the top, which it covers), y negated.  A
redraw sets the machine layer's target to the window's origin on the
screen and asks Paige to display what lies in the rectangle; the
selection is then inverted over it (EOR with white).  The Wimp's caret
goes where pgCaretPosition says.

## Not done yet

Rulers, tabs, indents and spacing in the desktop; styles beyond the
toolbar's four;
pictures.

## Licence

Write's own code is this repository's.  Paige is LGPL-2.1: the engine's
changes stay in `third_party/Paige` and publish with Write.
