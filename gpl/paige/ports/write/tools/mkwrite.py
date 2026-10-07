#!/usr/bin/env python3
"""mkwrite.py OUT -- !Write's Res: its Toolbox objects (tools/mkres.py).

    python3 ports/write/tools/mkwrite.py OUT/Res,fae

The application's events (src/main.c's EV_ numbers) are given to the
menus' entries here; the window is the document's, made once for each
(not on load) and an ancestor, so its menus' and dialogues' events name it.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from mkres import *  # noqa: E402,F403

SHARED_AUTO = CREATE_ON_LOAD | SHARED
SUB = 0x400                     # the entry raises its submenu's object
FILETYPE_WRITE = 0x0A0

EV_NEW, EV_QUIT = 0x100, 0x101
EV_UNDO, EV_SELALL, EV_CUT, EV_COPY, EV_PASTE = 0x110, 0x111, 0x112, 0x113, 0x114
EV_PLAIN, EV_BOLD, EV_ITALIC, EV_UNDERLINE = 0x120, 0x121, 0x122, 0x123
EV_SIZE = 0x130
EV_SAVE = 0x140
EV_SPELL_NEXT = 0x150
EV_SPELL_MARK = 0x151
EV_SPELL_LEARN = 0x152
SIZES = [8, 10, 12, 14, 18, 24, 36]

# the document's toolbar, along the top of its window inside it: bold,
# italic, underline; left, centre, right, justified; the paragraph's
# style.  Its buttons' clicks come to main.c as the Wimp's, by component
# (TB_ numbers); its style's choice as StringSet_ValueChanged.
TOOL_H = 64                     # main.c's TOOLBAR_H
TB_BOLD, TB_ITALIC, TB_UNDER = 0, 1, 2
TB_LEFT, TB_CENTRE, TB_RIGHT, TB_FULL = 3, 4, 5, 6
TB_STYLE = 7
# sprite and text, bordered (an R5 slab, in when selected), centred,
# filled, indirected, click; black on grey.  The sprite's second name is
# its selected look (mksprites.py's, on blue), not the Wimp's shading.
TOOL_ICON = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | 3 << 12 | 7 << 24 | 1 << 28


def tool(cid, x, sprite, help):
    return button((x, -58, x + 52, -6), cid, "", TOOL_ICON, validation="R5;S%s,%sp" % (sprite, sprite), max_value=1,
                  help=help)


TOOLS = [
    tool(TB_BOLD, 8, "tb_bold", "Makes the selection bold, or not bold."),
    tool(TB_ITALIC, 60, "tb_italic", "Makes the selection italic, or not italic."),
    tool(TB_UNDER, 112, "tb_under", "Underlines the selection, or not."),
    tool(TB_LEFT, 184, "tb_left", "Sets the paragraph flush left."),
    tool(TB_CENTRE, 236, "tb_centre", "Centres the paragraph."),
    tool(TB_RIGHT, 288, "tb_right", "Sets the paragraph flush right."),
    tool(TB_FULL, 340, "tb_full", "Justifies the paragraph to both margins."),
    string_set((412, -56, 700, -8), TB_STYLE, "Normal,Heading 1,Heading 2,Heading 3", "Style", "Normal",
               max_len=12, flags=1, help="The paragraph's style: Normal text, or a heading."),
]

objects = [
    iconbar("Iconbar", "!write", menu="IbarMenu", select_event=EV_NEW, flags=0x20,
            help="This is Write, the word processor.|MClick SELECT for a new document.",
            objflags=SHARED_AUTO | SHOW_ON_CREATE),
    menu("IbarMenu", "Write", [
        menu_entry(0, "Info", submenu_show="ProgInfo", flags=SUB),
        menu_entry(1, "Quit", click_event=EV_QUIT),
    ], objflags=SHARED_AUTO),
    proginfo("ProgInfo", "About this program", "Word processor", "ROSGD, over Paige (LGPL)",
             0, "0.10 (05 Oct 2026)", objflags=SHARED_AUTO),

    # the document's window: a white work area, its button type 10 (click,
    # drag, double-click); made for each document
    window("Doc", "<Untitled>", (200, 300, 1513, 1100), (0, -1600, 1313, 0), [],
           flags=2, menu="DocMenu", window_flags=0xFF000002, objflags=ANCESTOR,
           work_flags=0x0000A000, title_max=256, colours=(7, 2, 7, 0), toolbars=(None, "Tools", None, None)),
    # the toolbar: a pane (flag 0x10, a toolbar), its sprites the application's
    window("Tools", "", (0, 0, 1400, TOOL_H), (0, -TOOL_H, 1400, 0), TOOLS, flags=0x10,
           window_flags=0x80000030, work_flags=0, colours=(7, 2, 7, 1), own_sprites=True),
    menu("DocMenu", "Write", [
        menu_entry(0, "File", submenu_show="FileMenu", flags=SUB),
        menu_entry(1, "Edit", submenu_show="EditMenu", flags=SUB),
        menu_entry(2, "Style", submenu_show="StyleMenu", flags=SUB),
        menu_entry(3, "Size", submenu_show="SizeMenu", flags=SUB),
        menu_entry(4, "Font", submenu_show="FontMenu", flags=SUB),
        menu_entry(5, "Spell", submenu_show="SpellMenu", flags=SUB),
    ], objflags=SHARED_AUTO),
    # the word at the caret: its verdict (component 1), then the
    # suggestions main.c adds as it is shown (its show event, flag 1)
    menu("SpellMenu", "Spell", [
        menu_entry(0, "Next misspelling  ^N", click_event=EV_SPELL_NEXT),
        menu_entry(2, "Mark as you type", click_event=EV_SPELL_MARK, flags=1 | 2),  # ticked
        menu_entry(1, "The word at the caret", flags=0x100, text_max=80),
        menu_entry(3, "Learn word", click_event=EV_SPELL_LEARN, flags=0x100, text_max=80),
    ], flags=1, objflags=SHARED_AUTO),
    menu("FileMenu", "File", [
        menu_entry(0, "Save      ^S", click_event=EV_SAVE),
        menu_entry(1, "Save as", submenu_show="SaveAs", flags=SUB),
        menu_entry(3, "Export RTF", submenu_show="SaveRTF", flags=SUB),
        menu_entry(4, "Export HTML", submenu_show="SaveHTML", flags=SUB),
        menu_entry(2, "Export text", submenu_show="SaveText", flags=SUB | 2),
        menu_entry(5, "Print...   ^P", click_show="PrintDbox"),
    ], objflags=SHARED_AUTO),
    menu("EditMenu", "Edit", [
        menu_entry(0, "Undo      ^Z", click_event=EV_UNDO, flags=2),
        menu_entry(1, "Cut       ^X", click_event=EV_CUT),
        menu_entry(2, "Copy      ^C", click_event=EV_COPY),
        menu_entry(3, "Paste     ^V", click_event=EV_PASTE, flags=2),
        menu_entry(4, "Select all ^A", click_event=EV_SELALL),
    ], objflags=SHARED_AUTO),
    menu("StyleMenu", "Style", [
        menu_entry(0, "Plain", click_event=EV_PLAIN, flags=2),
        menu_entry(1, "Bold      ^B", click_event=EV_BOLD),
        menu_entry(2, "Italic", click_event=EV_ITALIC),
        menu_entry(3, "Underline ^U", click_event=EV_UNDERLINE),
    ], objflags=SHARED_AUTO),
    menu("SizeMenu", "Size", [menu_entry(i, "%d pt" % s, click_event=EV_SIZE + i)
                              for i, s in enumerate(SIZES)], objflags=SHARED_AUTO),
    fontmenu("FontMenu", objflags=SHARED_AUTO),
    saveas("SaveAs", "Save as", "Document", FILETYPE_WRITE, flags=1 | 4, objflags=SHARED_AUTO),
    saveas("SaveRTF", "Export RTF", "Document/rtf", 0xC32, flags=1 | 4, objflags=SHARED_AUTO),
    saveas("SaveHTML", "Export HTML", "Page/html", 0xFAF, flags=1 | 4, objflags=SHARED_AUTO),
    saveas("SaveText", "Export text", "Text", 0xFFF, flags=1 | 4, objflags=SHARED_AUTO),
    # the print dialogue: all, or from and to; copies (its show event sets
    # the document's page range)
    printdbox("PrintDbox", page_from=1, page_to=1, copies=1, flags=1 | 8 | 0x10, objflags=SHARED_AUTO),
    dcs("DCS", "Write", "This document has been changed. Save it?", objflags=SHARED_AUTO),
    quit("Quit", "Write", "Some documents are unsaved. Quit anyway?", objflags=SHARED_AUTO),
]

out = sys.argv[1]
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with open(out, "wb") as fh:
    fh.write(resfile(objects))
print("mkwrite: %s, %d objects" % (out, len(objects)))
