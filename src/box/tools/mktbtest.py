#!/usr/bin/env python3
"""mktbtest.py -- !TBTest's Res file (disc/Apps/!TBTest/Res,fae).

!TBTest is a Toolbox test application for a person to try every Toolbox
object and gadget in the box's ROM (ports/toolbox): an icon on the icon
bar, a menu that opens each kind of object, and a window holding every
kind of gadget, with an event log.  Its !RunImage (BASIC) logs each
Toolbox event the objects raise.  This writes the objects (tools/mkres.py):

    python3 tools/mktbtest.py [OUT]       default disc/Apps/!TBTest/Res,fae
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkres import *  # noqa: E402,F403

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "disc", "Apps", "!TBTest", "Res,fae")

AUTO = CREATE_ON_LOAD | SHARED   # shared: one of each, whichever objects attach it
EXIT = 0x1                       # the menu's Exit entry's event


def row(y, h=44):
    """a row's top y, downwards"""
    return -y - h, -y


gadgets = []
cid = 0


def add(g):
    global cid
    gadgets.append(g)
    cid += 1


def box(x0, y, x1, h=44):
    y0, y1 = row(y, h)
    return (x0, y0, x1, y1)


# ---- the gadget window: every kind of gadget, with what to try in each
# one's help text ----
add(label(box(16, 16, 250), cid, "Action buttons"))
add(action_button(box(260, 12, 420, 52), cid, "Default", flags=1, help="A default action button"))
add(action_button(box(440, 16, 580), cid, "Cancel", flags=2, help="A cancel action button"))
add(action_button(box(600, 16, 740), cid, "Local", flags=4, help="A local action button: the window stays"))
add(action_button(box(760, 16, 940), cid, "Show Info", flags=8, click_show="ProgInfo",
                  help="Shows the program information box"))

add(option_button(box(16, 76, 250), cid, "Option button"))
add(radio_button(box(260, 76, 420), cid, 1, "Radio A", flags=1 | 4))
add(radio_button(box(440, 76, 600), cid, 1, "Radio B", flags=1))
add(radio_button(box(620, 76, 780), cid, 1, "Radio C", flags=1))

add(label(box(16, 136, 160), cid, "Writable"))
add(writable_field(box(170, 132, 500, 52), cid, "Type here", max_len=64))
add(label(box(520, 136, 620), cid, "Display"))
add(display_field(box(630, 132, 940, 52), cid, "A display field", max_len=64))

add(label(box(16, 196, 160), cid, "Number"))
add(number_range(box(170, 192, 420, 52), cid, 0, 100, 1, 42, display_length=120,
                 flags=1 | 4 | 0x10 | 0x200, help="A number range: type or use the arrows"))
add(label(box(440, 196, 540), cid, "String set"))
add(string_set(box(550, 192, 940, 52), cid, "Red,Green,Blue,Cyan,Magenta,Yellow", "Colours", "Green",
               flags=1, help="A string set: choose from its menu"))

add(label(box(16, 256, 160), cid, "Slider"))
add(slider(box(170, 256, 600, 40), cid, 0, 100, 1, 30, flags=1 | 2 | 0x10 | (11 << 12) | (1 << 16),
           help="A slider: drag its bar"))
add(adjuster(box(620, 252, 664, 48), cid, 1, help="An adjuster arrow: left, down by one"))
add(adjuster(box(666, 252, 710, 48), cid, 0, help="An adjuster arrow: right, up by one"))
add(label(box(730, 256, 840), cid, "Pop-up"))
add(popup(box(850, 252, 894, 48), cid, "PopMenu", flags=1, help="A pop-up menu"))

add(draggable(box(16, 316, 300, 48), cid, "Drag me", flags=1 | 4, help="A draggable: drag it somewhere"))
add(button(box(320, 316, 620, 48), cid, "A button gadget", 0x1700313D, help="A button gadget"))

add(labelled_box((16, -620, 960, -380), cid, "TextGadgets"))
add(text_area((32, -600, 600, -400), cid,
              "A text area (TextGadgets).\nType and select here; it wraps and scrolls.",
              flags=1 | 4, help="A text area: type in it"))
add(scroll_list((620, -600, 880, -400), cid, flags=1, help="A scrolling list: click its items"))
add(scrollbar((900, -600, 940, -400), cid, 0, 100, 0, 20, 1, 10, help="A scrollbar"))

add(label(box(16, 640, 940), cid, "Toolbox events, the latest last:"))
LOG = cid
add(text_area((16, -920, 960, -690), cid, "", flags=1 | 4, help="The events the objects raised"))

# TBTEST_GADGETS=N: only the first N gadgets (to find which one a fault is in)
if os.environ.get("TBTEST_GADGETS"):
    gadgets = gadgets[:int(os.environ["TBTEST_GADGETS"])]

gadget_window = window("Gadgets", "Toolbox test: every gadget", (160, 220, 1140, 1160), (0, -940, 980, 0),
                       gadgets, flags=2 | 4, objflags=AUTO)   # no menu: IbarMenu shows this window,
                                                             # and attached objects are made with
                                                             # their parent -- each would make the other

# ---- the icon bar menu: each kind of object ----
entries = [
    menu_entry(0, "Info", submenu_show="ProgInfo", flags=0x400),
    menu_entry(1, "Gadgets window...", click_show="Gadgets"),
    menu_entry(2, "Save as", submenu_show="SaveAs", flags=0x400),
    menu_entry(3, "File info", submenu_show="FileInfo", flags=0x400),
    menu_entry(4, "Scale", submenu_show="Scale", flags=0x400),
    menu_entry(5, "Print...", click_show="PrintDbox"),
    menu_entry(6, "Font choice...", click_show="FontDbox"),
    menu_entry(7, "Font menu", submenu_show="FontMenu", flags=0x400),
    menu_entry(8, "Colour choice...", click_show="ColourDbox"),
    menu_entry(9, "Colour menu", submenu_show="ColourMenu", flags=0x400 | 2),
    menu_entry(10, "Discard/Cancel/Save...", click_show="DCS"),
    menu_entry(11, "Quit dialogue...", click_show="Quit", flags=2),
    menu_entry(12, "Exit", click_event=EXIT),
]
ibar_menu = menu("IbarMenu", "TBTest", entries, objflags=AUTO)
pop_menu = menu("PopMenu", "Pop-up", [menu_entry(0, "First"), menu_entry(1, "Second"),
                                      menu_entry(2, "Third")], objflags=AUTO)

objects = [
    iconbar("Iconbar", "application", text="TBTest", menu="IbarMenu", select_show="Gadgets",
            help="The Toolbox test application", objflags=AUTO | SHOW_ON_CREATE),
    ibar_menu,
    pop_menu,
    gadget_window,
    proginfo("ProgInfo", "About this program", "Tries every Toolbox object and gadget", "ROSGD",
             0, "1.00 (04 Oct 2026)", objflags=AUTO),
    # SaveAs saves the log itself from where the program says it is (automatic
    # RAM transfer, a "type 1" client), told as the dialogue opens (its show event)
    saveas("SaveAs", "Save as", "TBTestLog", 0xFFF, flags=1 | 4 | 8, objflags=AUTO),
    fileinfo("FileInfo", "About this file", "ADFS::Disc.$.Example", 0xFFF, 1234, modified=1,
             objflags=AUTO),
    scale("Scale", "Scale view", 10, 400, 10, objflags=AUTO),
    printdbox("PrintDbox", page_from=1, page_to=4, copies=1, scale=100, objflags=AUTO),
    fontdbox("FontDbox", "Choose a font", "Trinity.Medium", 12, 100, "The quick brown fox", objflags=AUTO),
    fontmenu("FontMenu", "Homerton.Medium", objflags=AUTO),
    colourdbox("ColourDbox", "Choose a colour", 0x0000FF00, objflags=AUTO),
    colourmenu("ColourMenu", "Colour", 11, objflags=AUTO),
    dcs("DCS", "Unsaved changes", "This document has been changed. Save it?", objflags=AUTO),
    quit("Quit", "Quit", "Some data is unsaved. Quit anyway?", objflags=AUTO),
]

# TBTEST_ONLY=Name,Name...: only those objects (to find which one a fault is in)
only = os.environ.get("TBTEST_ONLY")
if only:
    objects = [o for o in objects if o.name in only.split(",")]

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "wb") as fh:
    fh.write(resfile(objects))
print(f"mktbtest: {OUT}, {len(objects)} objects, {len(gadgets)} gadgets; the event log is component {LOG}")
