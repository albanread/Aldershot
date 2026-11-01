# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""rosapp: the shell shared by every ROSAPPS application.

Each application is this shell with its own name, engine, tools and
document types, set in an AppConfig.  For example, !Shell runs it over a
stub engine and !Scribe runs it over WordCraft.  Because the shell owns
everything the applications have in common, they all look and behave the
same way.  The shell provides:

  - the document window and its toolbar;
  - the rulers, marked in millimetres;
  - the tool palette and the inspector, which sit outside the window's
    frame (and move in over the page when the window is pushed against
    the edge of the screen);
  - the menu entries every application has;
  - the icon bar menu and the Info box;
  - Choices, interactive help, and loading documents dropped on it.

The engine is a Linux program in the application's directory, one for
each processor (Engine-aarch64 and Engine-x86_64).  The ROSApps module
starts it and binds its page to the window, and the Wimp redraws the
window straight from that page.  So the shell itself draws nothing but the
rulers.  The engine tells the shell how big its picture is ("doc"), what
the document is called ("title") and what went wrong ("error"), and the
shell binds a page of that size at the current zoom.

build.sh copies this file into each application, next to the
application's own file.
"""

from riscos.desktop import (Desktop, WimpApp, Window, Click, Menu, WindowState, Redraw,
                            iconbar_icon, report_error, nest_links, set_colour, WINDOW_DOCUMENT,
                            BUTTON_DOUBLE_CLICK_DRAG, WF_MOVEABLE, WF_TITLE_BAR, WF_NEW_FORMAT,
                            WF_AUTO_REDRAW, WINDOW_DIALOGUE, WINDOW_PANE, WF_PANE, LINK_LOW,
                            LINK_HIGH, PANE_LEFT, PANE_BELOW, PANE_RIGHT, start_task)
from riscos.swi import Regs, Buffer, swi, swi_x, get_string, peek, peek_byte
from riscos import system, vdu, files

comptime ROSApps_Open = 0xC0200
comptime ROSApps_Bind = 0xC0201
comptime ROSApps_Send = 0xC0202
comptime ROSApps_Poll = 0xC0203
comptime ROSApps_Close = 0xC0204
comptime Wimp_GetPointerInfo = 0x400CF
comptime Wimp_GetWindowState = 0x400CB
comptime Wimp_GetCaretPosition = 0x400D3

# The engine's picture has 10 mm of empty "desk" around its pages.  At 100%
# a page is shown at its real size on a 90 dpi screen, which is 3.54 pixels
# to the millimetre (stored as 354 hundredths).
comptime DESK_MM = 10
comptime ACTUAL = 354
comptime PAGE_BYTES = 60 << 20     # the largest page the engine's buffer may hold

comptime ROSApps_LinuxPath = 0xC0206
comptime ROSApps_Tiles = 0xC020C
comptime ROSApps_Tower = 0xC020D
comptime ROSApps_Scene3d = 0xC020E
comptime OS_ReadMonotonicTime = 0x42
comptime Wimp_SendMessage = 0x400E7
comptime MSG_TW_INPUT = 0x808C0         # TaskWindow's messages (PRM 3-277)
comptime MSG_TW_OUTPUT = 0x808C1
comptime MSG_TW_EGO = 0x808C2
comptime MSG_TW_MORIO = 0x808C3
comptime MSG_TW_MORITE = 0x808C4
comptime MSG_CLAIM_ENTITY = 15
comptime MSG_DATA_REQUEST = 16
comptime TYPE_TEXT = 0xFFF
comptime PDriver_SelectJob = 0x80145
comptime PDriver_EndJob = 0x80148
comptime PDriver_AbortJob = 0x80149
comptime PDriver_GiveRectangle = 0x8014B
comptime PDriver_DrawPage = 0x8014C
comptime PDriver_GetRectangle = 0x8014D
comptime OS_SpriteOp = 0x2E
comptime ColourPicker_OpenDialogue = 0x47702
comptime MSG_COLOUR_CHOICE = 0x47700
comptime PRINT_DPI = 180                # one pixel per OS unit, so printing needs no scaling



def button_width(text: String) -> Int:
    """An action button's width for its text: the text as the desktop font
    draws it (Wimp_TextOp 1), and the border and margins either side."""
    var r = Regs()
    r[0] = 1
    r.set_string(1, text)
    r[2] = 0
    if swi_x(0x400F9, r) != 0 or r[0] <= 0:
        return 36 + 16 * text.byte_length()
    return r[0] + 40

def prop_split(f: String) -> Tuple[String, String, Int]:
    """A properties row (show_props) as its shape, its value and its kind.
    The shape is the row without its value: two lines of the same shapes
    are the same rows.  Kinds: -1 nothing to change (a heading, a label),
    0 a field, 1 a choice, 2 a switch, 3 a button, 5 a value shown,
    6 a colour, 7 a picture that is a button."""
    if f.startswith("*"):
        var pressed = f.startswith("*=")
        var name = String(f[byte=2:]) if pressed else String(f[byte=1:])
        return (String("*") + name, String("1") if pressed else String("0"), 3)
    if f.startswith("^"):
        var pressed = f.startswith("^=")
        var name = String(f[byte=2:]) if pressed else String(f[byte=1:])
        return (String("^") + name, String("1") if pressed else String("0"), 7)
    if f.startswith("#"):
        return (f.copy(), String(""), -1)
    var cut = f.byte_length()
    for c in [f.find("="), f.find(":"), f.find("?"), f.find("!"), f.find("~")]:
        if c > 0 and c < cut:
            cut = c
    if cut >= f.byte_length():
        return (f.copy(), String(""), -1)
    var sep = String(f[byte=cut:cut + 1])
    var rest = String(f[byte=cut + 1:])
    var head = String(f[byte=0:cut + 1])
    if sep == "?":
        var q2 = rest.find("?")
        if q2 < 0:
            return (head, rest^, 1)
        return (head + String(rest[byte=q2 + 1:]), String(rest[byte=0:q2]), 1)
    if sep == "!":
        return (head, rest^, 2)
    if sep == "~":
        return (head, rest^, 6)
    if sep == "=":
        return (head, rest^, 0)
    return (head, rest^, 5)


def caret_in(window: Int, icon: Int) -> Bool:
    """Whether the caret is in this icon (Wimp_GetCaretPosition)"""
    var b = Buffer(24)
    var r = Regs(0, b.addr())
    _ = swi_x(Wimp_GetCaretPosition, r)
    return b.word(0) == window and b.word(4) == icon


struct AppConfig(Copyable, Movable):
    """The settings that make the shell one application rather than another."""

    var name: String                # used for the task, <name>$Dir and Choices:<name>
    var purpose: String
    var version: String
    var icon: String                # its sprite, on the icon bar
    var types: List[Int]            # the file types it opens
    var imports: List[Int]          # types it accepts when dropped on it, but does not open
    var imports_any: Bool           # any type dropped on a document is placed in it (with places):
                                    # !Workflows' inputs; its own types still open
    var tools: List[String]         # its tool palette's icons
    var start: String               # the first command for the engine, or ""
    var save_type: Int              # the file type it saves as, or 0 if it does not save
    var save_leaf: String           # a new document's name in the save box
    var pastes_text: Bool           # text pasted or dropped in goes in at the caret
    var dev_keys: Bool              # a programmer's keys: F3 and Ctrl-P go to the engine (find
                                    # again, the quick pick) rather than the save box and print
    var menu_context: Bool          # MENU on the document asks the engine first ("menuat X Y"),
                                    # which answers "menustate M.I ..." with the items to shade
    var new_line: String            # the engine command for an empty document
    var fit: Bool                   # a document bigger than the screen opens zoomed to fit
    var zoom_keys: Bool             # Ctrl with keypad + and -, - and 0 zooms
    var colours: Bool               # the inspector has a colour to paint with (ColourPicker)
    var props: Bool                 # the properties pane is shown until Choices say otherwise
    var tool_help: List[String]     # the program's own help for its tools, by place (or "")
    var viewport: Bool              # the engine draws only what is in view (as a spreadsheet
                                    # does): its picture stays fixed to the window, and the
                                    # engine is told when the window scrolls
    var tiles: Bool                 # the engine's picture is a tile map that the GPU presenter
                                    # draws from the tiles themselves (ROSApps_Tiles), where there
                                    # is one: zooming and scrolling cost the engine nothing.
                                    # Where there is none, a page (or a viewport) as ever
    var tower: Bool                 # with tiles: the picture is a tower (ROSApps_Tower,
                                    # include/rosgd/tower.h) rather than a tile map
    var scene3d: Bool               # with tiles: the picture is a 3D scene (ROSApps_Scene3d,
                                    # include/rosgd/scene3d.h), the icon its viewport; the
                                    # engine's session is opened with the scene's buffer
    var formula_bar: Bool           # a bar under the toolbar for the current cell's contents
    var geometry: Bool              # the inspector shows X, Y, W and H; if False it shows a
                                    # line of text from the engine instead
    var view_toggles: Bool          # the toolbar has the rulers, guides and grid buttons
    var view_rulers: Bool           # a viewport whose engine draws the rulers in its picture, and is
                                    # told "rulers on" or "rulers off" as they are switched
    var guides_on: Bool             # the guides are shown until Choices say otherwise (a page
                                    # layout shows its margins and frames from the start)
    var thumbs: Bool                # the engine makes pictures of the document's pages (a sprite
                                    # file in the scrap directory, "thumbs N" when it is written)
                                    # for rows of the properties pane that start with ^
    var tabs: Bool                  # one window holds many documents, as tabs (!Code): a file
                                    # opened goes to the window as a new tab, and the engine says
                                    # which tab is in front ("tab TITLE|PATH|TYPE")
    var rulers: Bool                # the window has rulers (paper millimetres); False for a
                                    # program whose page is drawn to a scale (!FloorPlan)
    var export_names: List[String]  # the File menu's Export entries ("PNG picture"), with
    var export_types: List[Int]     # each one's file type and the engine's name for the format
    var export_exts: List[String]
    var places: Bool                # a file dropped on a document is placed in it, as Draw
                                    # does; double-clicked or dropped on the icon, it opens
    var menu_names: List[String]    # its own menus, after File, Edit and View
    var menu_items: List[String]    # their entries, as Menu takes them ("-" before a gap)
    var view_items: String          # its own entries at the foot of the View menu, after a
                                    # gap ("Classic,Enhanced,-Night"), or ""; chosen, the
                                    # engine is told "menu 2 N", N from 0
    var replacing_menus: List[Int]  # those of its menus (3 up) whose entries replace the
                                    # document (a new city, a scenario): the shell asks first
                                    # whether to discard unsaved changes, as New does
    var big_label: String           # a wide button at the end of the toolbar ("Show"), or ""
    var big_sprite: String          # that button's icon
    var big_line: String            # the command it sends the engine
    var big_help: String            # its interactive help text

    def __init__(out self, name: String, purpose: String, version: String, icon: String):
        self.name = name
        self.purpose = purpose
        self.version = version
        self.icon = icon
        self.types = List[Int]()
        self.imports = List[Int]()
        self.imports_any = False
        self.tools = List[String]()
        self.start = String("")
        self.save_type = 0
        self.save_leaf = String("Document")
        self.pastes_text = False
        self.dev_keys = False
        self.menu_context = False
        self.props = False
        self.tool_help = List[String]()
        self.new_line = String("new")
        self.fit = False
        self.zoom_keys = False
        self.colours = False
        self.viewport = False
        self.tiles = False
        self.tower = False
        self.scene3d = False
        self.formula_bar = False
        self.geometry = True
        self.view_toggles = True
        self.guides_on = False
        self.view_rulers = False
        self.thumbs = False
        self.rulers = True
        self.tabs = False
        self.places = False
        self.export_names = List[String]()
        self.export_types = List[Int]()
        self.export_exts = List[String]()
        self.menu_names = List[String]()
        self.menu_items = List[String]()
        self.view_items = String("")
        self.replacing_menus = List[Int]()
        self.big_label = String("")
        self.big_sprite = String("")
        self.big_line = String("")
        self.big_help = String("")

# The sizes of the window's furniture, in OS units.  They are the same in
# every ROSAPPS application.  The toolbar is inside the window, under the
# title bar; the tool palette and the inspector are panes outside it.
comptime TOOLBAR_H = 68
comptime VIEW_OWN = 7                   # the View menu's first entry of the application's own
comptime PALETTE_W = 72
comptime INSPECTOR_H = 100
comptime RULER_H = 44
comptime RULER_W = 56
comptime FORMULA_H = 56
comptime PROPS_W = 500                 # the properties pane, on the right
comptime PROPS_LABEL = 220             # where its values start, after their names
# The panes along a window's edge are made as wide as the widest screen
# (an 8K screen at two OS units a pixel), so that they reach right across
# a window however wide it is made.
comptime PANE_MAX = 16384


def toolbar_names() -> List[String]:
    """The toolbar's buttons, the same in every application: file, edit,
    undo, view and zoom, in that order, and last the properties pane's
    switch.  "|" marks a gap between groups."""
    var n = List[String]()
    for s in ["rs_info", "rs_new", "rs_save", "rs_print", "|", "rs_cut", "rs_copy", "rs_paste", "|",
              "rs_undo", "rs_redo", "|", "rs_rulers", "rs_guides", "rs_grid", "|",
              "rs_zoomout", "rs_zoom100", "rs_zoomin", "|", "rs_props"]:
        n.append(String(s))
    return n^


def file_type_of(path: String) -> Int:
    """A file's type (OS_File 23), or -1 if it is not a file"""
    var r = Regs()
    r[0] = 23
    r.set_string(1, path)
    if swi_x(0x08, r) != 0 or r[0] != 1:
        return -1
    return r[6]


def swi2(number: Int, r0: Int, r1: Int = 0) -> Int:
    """Calls a SWI with one or two arguments.  An error is returned, not raised."""
    var r = Regs(r0, r1)
    return swi_x(number, r)


def confirm(name: String, text: String) -> Bool:
    """Asks a question in the Wimp's error box, with OK and Cancel buttons.
    Returns True if the user chose OK."""
    var b = Buffer(260)
    b.set_word(0, 0)
    b.set_string(4, String(text[byte=0:min(text.byte_length(), 250)]))
    var r = Regs()
    r[0] = b.addr()
    r[1] = 3 | 16                           # OK, Cancel; "Message from"
    r.set_string(2, name)
    _ = swi_x(0x400DF, r)
    return r[1] == 1


def discard_or_save(name: String, text: String) -> Int:
    """Asks whether to discard unsaved changes, as the Style Guide has it:
    Discard, Cancel and Save.  Returns 1 for Discard, 2 for Save and 0 for
    Cancel (or Escape)."""
    var b = Buffer(260)
    b.set_word(0, 0)
    b.set_string(4, String(text[byte=0:min(text.byte_length(), 250)]))
    var r = Regs()
    r[0] = b.addr()
    r[1] = 16 | 0x100 | (4 << 9)             # "Message from"; a question
    r.set_string(2, name)
    r[3] = 0                                # the application's sprite
    r[4] = 1                                # from the Wimp's pool
    # Discard, Cancel and Save from the left: the first is the default
    # (Return) and the second Escape's, and they are returned as 3, 4, 5
    r.set_string(5, String("Save,Cancel,Discard"))
    _ = swi_x(0x400DF, r)
    if r[1] == 3:
        return 2
    if r[1] == 5:
        return 1
    return 0


def modifiers() -> Int:
    """Which of Shift (1), Ctrl (2) and Alt (4) are held down now.  This is
    needed because RISC OS gives the same key code for Return and
    Ctrl-Return, and for Home with or without them."""
    var m = 0
    for k in range(3):
        var r = Regs()
        r[0] = 121
        r[1] = k ^ 0x80
        _ = swi_x(0x06, r)
        if r[1] == 0xFF:
            m |= 1 << k
    return m


def hex2(n: Int) -> String:
    var d = String("0123456789ABCDEF")
    return String(d[byte=(n >> 4) & 15:((n >> 4) & 15) + 1]) + String(d[byte=n & 15:(n & 15) + 1])


def field_size(text: String) -> Int:
    """A properties field's buffer: room to type, and for a paragraph's
    words being edited"""
    return min(4096, max(64, text.byte_length() + 256))


def hexval(c: Int) -> Int:
    if c >= 48 and c <= 57:
        return c - 48
    if c >= 65 and c <= 70:
        return c - 55
    if c >= 97 and c <= 102:
        return c - 87
    return 0


def unescape_into(t: String, mut out: List[Int]):
    """Turns the engine's escaped text into Latin-1 bytes.  \\n is a newline
    (LF, as in RISC OS text files), \\t is a tab and \\xHH is a byte."""
    var b = t.as_bytes()
    var i = 0
    while i < len(b):
        var c = Int(b[i])
        if c == 92 and i + 1 < len(b):
            var e = Int(b[i + 1])
            if e == 110:
                out.append(10)
                i += 2
            elif e == 116:
                out.append(9)
                i += 2
            elif e == 120 and i + 3 < len(b):
                out.append(hexval(Int(b[i + 2])) * 16 + hexval(Int(b[i + 3])))
                i += 4
            else:
                out.append(e)
                i += 2
        else:
            out.append(c)
            i += 1


def hex8(n: Int) -> String:
    return hex2((n >> 24) & 255) + hex2((n >> 16) & 255) + hex2((n >> 8) & 255) + hex2(n & 255)


def hex3(n: Int) -> String:
    """Three lower-case hex digits, as in a file type's sprite name"""
    var d = String("0123456789abcdef")
    var s = String("")
    for shift in [8, 4, 0]:
        var k = (n >> shift) & 15
        s += String(d[byte=k:k + 1])
    return s


def leaf_of(path: String) -> String:
    """The last part of a RISC OS file name, after the last '.' or ':'"""
    var at = 0
    for i in range(path.byte_length()):
        var ch = path[byte=i:i+1]
        if ch == "." or ch == ":":
            at = i + 1
    return String(path[byte=at:])


def help_for(name: String) -> String:
    if name == "rs_info":
        return ("This button shows the program's name, purpose, author and version."
                + "|MClick SELECT to see them.")
    if name == "rs_marquee": return "The selection box: drag to select a rectangle; click to select nothing."
    if name == "rs_lasso": return "The lasso: drag round what to select."
    if name == "rs_wand": return "The wand: click to select what touches the place and is near its colour."
    if name == "rs_brush": return "The brush: paint in the colour chosen in the inspector.  [ and ] change its size."
    if name == "rs_pencil": return "The pencil: draw freehand; the line is smoothed into curves."
    if name == "rs_rrect": return "The rounded rectangle: drag out a rectangle with rounded corners."
    if name == "rs_polygon": return "The polygon: drag out a many-sided shape from its centre."
    if name == "rs_triangle": return "The triangle: drag out a triangle."
    if name == "rs_arrow": return "The arrow: drag out a block arrow."
    if name == "rs_star": return "The star: drag out a star from its centre."
    if name == "rs_spray":
        return ("The spray: fine dots of the colour, thicker where it rests.  "
                + "Its size and flow are in the inspector.")
    if name == "rs_smudge":
        return ("The smudge: drag the colours under the brush along.  "
                + "Its size and strength are in the inspector.")
    if name == "rs_eraser": return "The eraser: rub out, to the background colour on the background."
    if name == "rs_fill": return "The fill: click to fill what touches the place and is near its colour."
    if name == "rs_gradient": return "The gradient: drag from where the colour starts to where it ends."
    if name == "rs_eyedrop": return "The eyedropper: click to take the colour to paint with from the picture."
    if name == "rs_redeye": return "Red eyes: click on the red of a pupil to make it dark."
    if name == "rs_crop": return "The crop: drag a box round what to keep."
    if name == "rs_note": return "The note: click on the page to put a note there."
    if name == "rs_highlight": return "The highlighter: drag over text to highlight it."
    if name == "rs_ink": return "Ink: draw on the page."
    if name == "rs_new": return "This button starts a new document.|MClick SELECT to start one."
    if name == "rs_direct": return "The direct selection tool: click on anchor points and segments to select and move them."
    if name == "rs_pen": return "The pen: click to place anchor points, drag to curve them."
    if name == "rs_rect": return "The rectangle: drag to draw one."
    if name == "rs_ellipse": return "The ellipse: drag to draw one."
    if name == "rs_line": return "The line: drag to draw a straight line."
    if name == "rs_text": return "The type tool: click to type text."
    if name == "rs_save": return "This button saves the document.|MClick SELECT to save it."
    if name == "rs_print": return "This button prints the document.|MClick SELECT to print it."
    if name == "rs_cut": return "This button moves the selection to the clipboard."
    if name == "rs_copy": return "This button copies the selection to the clipboard."
    if name == "rs_paste": return "This button puts the clipboard's contents into the document."
    if name == "rs_undo": return "This button undoes the last change."
    if name == "rs_redo": return "This button does again what was undone."
    if name == "rs_rulers": return "This button shows or hides the rulers, in millimetres."
    if name == "rs_guides": return "This button shows or hides the guides."
    if name == "rs_grid": return "This button shows or hides the grid."
    if name == "rs_zoomout": return "This button makes the page smaller on the screen."
    if name == "rs_zoom100": return "This button shows the page at its actual size."
    if name == "rs_zoomin": return "This button makes the page larger on the screen."
    if name == "rs_props": return "This button shows or hides the properties pane, on the window's right."
    if name == "rs_select": return "The selection tool: click on things to select them, and drag them to move them."
    if name == "rs_pan": return "The hand: drag the page to move it in the window."
    if name == "rs_move": return "The move tool: drag what is selected to move it."
    if name == "rs_zoomtool": return "The magnifier: click to look closer, with ADJUST to look further away."
    if name == "rs_measure": return "The rule: drag to measure a distance on the page, in millimetres."
    return ""


struct Doc(Copyable, Movable):
    var cfg: AppConfig
    var doc_w100: Int                   # the engine's picture, in 1/100 mm; 0 if not known yet
    var doc_h100: Int
    var pg_w_mm: Int                    # a page's size, mm
    var pg_h_mm: Int
    var pg_h100: Int                    # a page's height, 1/100 mm
    var pages: Int
    var win: Window
    var toolbar: Window
    var palette: Window
    var inspector: Window
    var hruler: Window
    var vruler: Window
    var infobox: Window
    var savebox: Window
    var save_kind: Int                  # what the save box saves: -1 the document, or an export
    var save_icon: Int                  # the save box's file icon, name field and OK button
    var save_name: Int
    var save_ok: Int
    var doc_path: String                # the document's RISC OS file name; "" if never saved
    var tab_type: Int                   # tabs: the front tab's file type (0: cfg.save_type)
    var holds_file: Bool                # a file was opened in it (perhaps not of the type it
                                        # saves, so with no doc_path): it is not an empty window
    var menu_answered: Bool             # the engine has said which menu items to shade
    var menu_shaded: List[Int]          # the items it shaded, each menu * 256 + item
    var tw_ids: List[Int]               # task windows the engine started ("task"): its numbers,
    var tw_txt: List[Int]               # the handle each was started with (-txt),
    var tw_child: List[Int]             # and each one's task, once its Ego says (0 until then)
    var doc_title: String
    var modified: Bool
    var page_label: Int                 # the inspector's icons
    var sel_label: Int
    var x_field: Int
    var y_field: Int
    var w_field: Int
    var colour_button: Int              # the inspector's colour button, for programs that paint
    var colour_label: Int
    var colour: Int                     # &RRGGBB
    var h_field: Int
    var selecting: Bool                 # a SELECT drag across the text
    var owns_clip: Bool                 # the clipboard is ours (Message_ClaimEntity)
    var clip_out: Bool                  # a DataSave of the clipboard is under way
    var sel_x: Int
    var press_mods: Int
    var starting: Bool                  # True in the constructor, before the first Wimp_Poll
    var play: String                    # a command for the engine once the window is open
    var thumb_ro: String                # the sprite file of the pages' pictures, a RISC OS name
    var panning: Bool                   # a drag with the hand tool, and where it started
    var pan_x: Int
    var pan_y: Int
    var sel_y: Int
    var info_engine: Int                # the Info box's line for the engine's version
    var bar_menu: Menu
    var main_menu: Menu
    var file_menu: Menu
    var edit_menu: Menu
    var view_menu: Menu
    var docs_menu: Menu
    var own_menus: List[Menu]           # the application's own menus
    var askbox: Window                  # a box that asks the user for a line of text
    var ask_id: String                  # the question's name, sent back with the answer
    var ask_prompt: Int
    var ask_field: Int
    var ask_ok: Int
    var ask_cancel: Int
    var fbar: Window                    # the formula bar, and its icons
    var fbar_ref: Int
    var fbar_input: Int
    var toolbar_w: Int                  # the toolbar's buttons' width, OS units
    var view_w: Int                     # a viewport's picture, in pixels; 0 if not bound
    var tiles_icon: Int                 # the tile map's icon, the whole map at the zoom; -1 if none
    var tiles_off: Bool                 # no presenter draws a tile map here: pages instead
    var view_h: Int
    var tb_names: List[String]          # the toolbar's icon names, by icon number
    var tb_icons: List[Int]
    var tools: List[String]             # the palette's, by icon number
    var tool: Int
    var session: Int
    var engine: String
    var engine_version: String
    var rulers: Bool
    var guides: Bool
    var grid: Bool
    var zoom: Int                       # per cent
    var zoom_kept: Int                  # the user's zoom, while the engine has set another
    var engine_zoomed: Bool             # the engine set the zoom (a view that needs room)
    var ppmm100: Int                    # screen pixels per millimetre of the page, x 100
    var page_w: Int                     # pixels
    var page_h: Int
    var mleft: Int                      # the page's margins in the work area, OS units
    var mtop: Int
    var ospp: Int                       # OS units per screen pixel
    var scroll_x: Int                   # the window's scroll when the rulers were last drawn
    var scroll_y: Int
    var last_x: Int                     # the pointer, in the page's pixels; -1 if off the page
    var hover_tool: Int                 # the palette's tool the pointer is over, or -1
    var last_y: Int
    var fresh: Bool                     # a new document, to be opened at its top left
    var caret_x: Int                    # the engine's caret, in page pixels; no caret if caret_h is 0
    var caret_y: Int
    var caret_h: Int
    var pollword: Int                   # the engine's poll word, for Wimp_Poll
    var in_use: Bool                    # open, with an engine; if not, kept for the next document
    var want_new: Bool                  # New was chosen: the application opens another window
    var want_close: Bool                # saved as the discard question asked: the application closes it
    var close_after_save: Bool          # Save chosen in the discard question, the save box open
    var want_paste: Bool                # Paste was chosen, and the clipboard is not ours
    var opt_icons: List[Int]            # every icon of the tool's options in the inspector
    var opt_names: List[String]         # each option's name
    var opt_fields: List[Int]           # its number field, or -1 if it is read only
    var opt_minus: List[Int]            # its - and + buttons, or -1
    var opt_plus: List[Int]
    var showwin: Window                 # a window covering the whole screen, for a slide show
    var showing: Bool
    var choosebox: Window               # a box offering a choice (such as a spelling suggestion)
    var choose_id: String
    var choose_prompt: Int
    var choose_field: Int
    var choose_icons: List[Int]         # the buttons in it, made again each time it opens
    var choose_sugg: List[Int]
    var choose_sugg_text: List[String]
    var choose_acts: List[Int]
    var choose_act_text: List[String]
    var qbox: Window                    # the game's questions: text and action buttons only
    var q_id: String
    var q_lines: List[Int]
    var q_acts: List[Int]
    var q_act_text: List[String]
    var cascade: Int                    # how far down and right of the first window, OS units
    var props: Window                   # the properties pane, docked on the right
    var props_on: Bool                  # shown (View menu, remembered in Choices)
    var props_room: Int                 # its height on the screen, as the engine was told
    var props_icons: List[Int]          # every icon in it, made again for each props line
    var props_names: List[String]       # each property's name, by row
    var props_kinds: List[Int]          # 0 a field, 1 a choice, 2 a switch, 3 a button,
                                        # 4 a colour's button
    var props_values: List[String]      # each one's value as shown
    var props_choices: List[String]     # a choice's items, separated by commas
    var props_hit: List[Int]            # the icon of each row that takes clicks or Return
    var props_menu: Menu                # a choice's menu, while it is open
    var props_menu_row: Int             # the row whose menu is open
    var props_line: String              # the engine's last props line
    var props_colour: String            # the colour row whose ColourPicker is open, or ""
    var props_more: String              # rows sent ahead in "propsmore" lines
    var props_shape: String             # the rows as last made, values left out: a props
                                        # line of the same shape is shown in place
    var props_rows: List[Int]           # each row's value icon, or -1
    var props_rkind: List[Int]          # each row's kind (prop_split's), and its place in
    var props_rhit: List[Int]           # props_hit, or -1
    var props_sent: Int                 # the field whose value Return last sent, or -1:
                                        # the engine's answer is shown in it, caret or not
    var opt_shape: String               # the options as last made, values left out
    var opt_vals: List[Int]             # each option's value icon

    def __init__(out self, mut desk: Desktop, cfg: AppConfig, start: String, cascade: Int) raises:
        """Makes a document window, its panes and its engine.  start is the
        engine's first command (to show a sample or an empty document), or
        "" when a file is to be loaded into it.  cascade moves the window
        down and to the right of the one before."""
        self.cfg = cfg.copy()
        self.pollword = 0
        self.in_use = True
        self.want_new = False
        self.want_close = False
        self.close_after_save = False
        self.want_paste = False
        self.opt_icons = List[Int]()
        self.opt_names = List[String]()
        self.opt_fields = List[Int]()
        self.opt_minus = List[Int]()
        self.opt_plus = List[Int]()
        self.showwin = Window.create("", 16, 16, WF_NEW_FORMAT, 7)
        self.showing = False
        self.choosebox = Window.create("Choose", 900, 300,
                                       WF_MOVEABLE | WF_TITLE_BAR | WF_NEW_FORMAT | WF_AUTO_REDRAW, 1)
        self.choose_id = String("")
        self.choose_prompt = self.choosebox.create_icon(16, -60, 884, -16,
                                                        1 | (1 << 4) | (1 << 8) | (7 << 24), "", "", 200)
        _ = self.choosebox.label(16, -112, 196, -70, "Change to")
        self.choose_field = self.choosebox.writable(200, -112, 884, -70, "", 200)
        self.choose_icons = List[Int]()
        self.choose_sugg = List[Int]()
        self.choose_sugg_text = List[String]()
        self.choose_acts = List[Int]()
        self.choose_act_text = List[String]()
        self.qbox = Window.create("Question", 900, 220,
                                  WF_MOVEABLE | WF_TITLE_BAR | WF_NEW_FORMAT | WF_AUTO_REDRAW, 1)
        self.q_id = String("")
        self.q_lines = List[Int]()
        for i in range(3):
            self.q_lines.append(self.qbox.create_icon(16, -60 - 44 * i, 884, -16 - 44 * i,
                                                      1 | (1 << 4) | (1 << 8) | (7 << 24), "", "", 200))
        self.q_acts = List[Int]()
        self.q_act_text = List[String]()
        self.cascade = cascade
        self.props = Window.create("", PROPS_W, 2400, WINDOW_PANE, 1)
        self.props_on = cfg.props
        self.props_room = 0
        self.props_icons = List[Int]()
        self.props_names = List[String]()
        self.props_kinds = List[Int]()
        self.props_values = List[String]()
        self.props_choices = List[String]()
        self.props_hit = List[Int]()
        self.props_menu = Menu("Choose", "None")
        self.props_menu_row = -1
        self.props_line = String("")
        self.props_colour = String("")
        self.props_shape = String("")
        self.props_more = String("")
        self.props_rows = List[Int]()
        self.props_rkind = List[Int]()
        self.props_rhit = List[Int]()
        self.props_sent = -1
        self.opt_shape = String("")
        self.opt_vals = List[Int]()
        self.doc_w100 = 0
        self.doc_h100 = 0
        self.pg_w_mm = 210
        self.pg_h_mm = 297
        self.pg_h100 = 29700
        self.pages = 0
        self.win = Window.create("Document1", 1000, 1300, WINDOW_DOCUMENT, 1,
                                 button_type=BUTTON_DOUBLE_CLICK_DRAG)
        self.toolbar = Window.create("", PANE_MAX, TOOLBAR_H, WINDOW_PANE, 1)
        self.fbar = Window.create("", PANE_MAX, FORMULA_H, WINDOW_PANE, 1)
        self.fbar_ref = -1
        self.fbar_input = -1
        self.toolbar_w = 0
        self.view_w = 0
        self.view_h = 0
        self.tiles_icon = -1
        self.tiles_off = False
        self.palette = Window.create("", PALETTE_W, 2000, WINDOW_PANE, 1)
        self.inspector = Window.create("", PANE_MAX, INSPECTOR_H, WINDOW_PANE, 1)
        self.hruler = Window.create("", PANE_MAX, RULER_H, WF_PANE | WF_NEW_FORMAT, 1)
        self.vruler = Window.create("", RULER_W, PANE_MAX, WF_PANE | WF_NEW_FORMAT, 1)
        self.infobox = Window.create("About this program", 700, 284, WINDOW_DIALOGUE, 1)
        self.info_engine = 0
        # the usual RISC OS save box: a title bar, but no back or close icon
        self.savebox = Window.create("Save as", 460, 260,
                                     WF_MOVEABLE | WF_TITLE_BAR | WF_NEW_FORMAT | WF_AUTO_REDRAW, 1)
        self.askbox = Window.create("Question", 680, 196,
                                    WF_MOVEABLE | WF_TITLE_BAR | WF_NEW_FORMAT | WF_AUTO_REDRAW, 1)
        self.ask_id = String("")
        self.ask_prompt = 0
        self.ask_field = 0
        self.ask_ok = 0
        self.ask_cancel = 0
        self.save_kind = -1
        self.save_icon = 0
        self.save_name = 0
        self.save_ok = 0
        self.doc_path = String("")
        self.holds_file = False
        self.tw_ids = List[Int]()
        self.tw_txt = List[Int]()
        self.tw_child = List[Int]()
        self.menu_answered = False
        self.menu_shaded = List[Int]()
        self.tab_type = 0
        self.doc_title = String("Document1")
        self.modified = False
        self.page_label = 0
        self.sel_label = 0
        self.x_field = 0
        self.y_field = 0
        self.w_field = 0
        self.colour_button = -1
        self.colour_label = -1
        self.colour = 0
        self.h_field = 0
        self.selecting = False
        self.owns_clip = False
        self.clip_out = False
        self.sel_x = -1
        self.press_mods = 0
        self.starting = False
        self.thumb_ro = String("")
        self.play = String("")
        self.panning = False
        self.pan_x = 0
        self.pan_y = 0
        self.sel_y = -1
        var file_items = String("Save,Save as...,Print")
        for i in range(len(cfg.export_names)):
            # (spaces after: the menu is made a little narrow for its font)
            file_items += ("," if i > 0 else ",-") + "Export " + cfg.export_names[i] + "...   "
        self.file_menu = Menu("File", file_items)
        self.edit_menu = Menu("Edit", "Undo,Redo,-Cut,Copy,Paste,-Select all")
        var view_list = String("Rulers,Guides,Grid,-Zoom in,Zoom out,Actual size,-Properties")
        if cfg.view_items.byte_length() > 0:
            view_list += ",-" + cfg.view_items
        self.view_menu = Menu("View", view_list)
        var tops = String("File,Edit,View")
        self.own_menus = List[Menu]()
        for i in range(len(cfg.menu_names)):
            tops += "," + cfg.menu_names[i]
            self.own_menus.append(Menu(cfg.menu_names[i], cfg.menu_items[i]))
        self.main_menu = Menu(cfg.name, tops)
        self.docs_menu = Menu("Documents", "Document1")
        self.bar_menu = Menu(cfg.name, "Info,New,Choices...,Documents,-Quit")
        self.tb_names = List[String]()
        self.tb_icons = List[Int]()
        self.tools = List[String]()
        self.tool = 0
        self.session = 0
        self.engine = system.read_var(cfg.name + "$Dir") + ".Engine"
        self.engine_version = String("starting")
        # a spreadsheet has its own headings instead; a viewport's engine may draw them
        self.rulers = cfg.rulers and (not cfg.viewport or cfg.view_rulers)
        self.guides = cfg.guides_on
        self.grid = False                   # the drawing grid is off until asked for
        self.zoom = 100
        self.zoom_kept = 100
        self.engine_zoomed = False
        self.ppmm100 = ACTUAL
        self.page_w = 0
        self.page_h = 0
        self.mleft = RULER_W
        self.mtop = TOOLBAR_H + RULER_H
        self.ospp = 1 << vdu.mode_variable(4)
        self.scroll_x = 0
        self.scroll_y = 0
        self.last_x = -2
        self.last_y = -2
        self.hover_tool = -1
        self.fresh = True
        self.caret_x = 0
        self.caret_y = 0
        self.caret_h = 0
        self.load_choices()
        self.ppmm100 = ACTUAL * self.zoom // 100
        self.make_menus()
        self.make_panes()
        if cfg.formula_bar:
            self.make_formula_bar()
        self.make_info()
        self.make_savebox()
        self.make_askbox()
        desk.attach_pane(self.win, self.palette, PANE_LEFT, PALETTE_W, TOOLBAR_H)
        desk.attach_pane(self.win, self.inspector, PANE_BELOW, INSPECTOR_H)
        if self.props_on:
            desk.attach_pane(self.win, self.props, PANE_RIGHT, PROPS_W, TOOLBAR_H)
        self.show_props(String(""))
        self.open_engine(desk, start)

    def open_engine(mut self, mut desk: Desktop, start: String) raises:
        """Starts the engine for this window and sends its first command.
        The window is opened later, from the poll loop, once the engine has
        said how big its page is.  If it were opened before the first
        Wimp_Poll, its toolbar and panes would stay blank until the task
        polled, and a large engine can be slow to start the first time."""
        var r = Regs()
        r[0] = 2 if self.cfg.scene3d else 0     # a 3D scene's session: the bigger buffer (bit 1)
        r.set_string(1, self.engine)
        swi(ROSApps_Open, r)
        self.session = r[0]
        self.pollword = r[1]
        desk.use_pollword(r[1])
        desk.want_null_events(4)
        if self.cfg.view_toggles:
            # tell the engine whether the grid and guides are on, so that
            # it can snap to them and draw them
            self.send("grid " + ("on" if self.grid else "off"))
            self.send("guides " + ("on" if self.guides else "off"))
            if self.cfg.viewport and self.cfg.view_rulers:
                self.send("rulers " + ("on" if self.rulers else "off"))
        if self.cfg.thumbs:
            # the pages' pictures: a sprite file the engine writes in the scrap
            # directory, named for this session, with sprites named for it too
            var scrap = system.read_var("Wimp$ScrapDir")
            if scrap.byte_length() > 0:
                var ro = scrap + ".PubThumbs" + String(self.session)
                var buf = Buffer(1024)
                var lp = Regs()
                lp.set_string(0, ro)
                lp[1] = buf.addr()
                lp[2] = 1024
                if swi_x(ROSApps_LinuxPath, lp) == 0:
                    self.thumb_ro = ro
                    self.send("scrap " + get_string(buf.addr()) + " t" + String(self.session) + "_")
        if start.byte_length() > 0:
            self.send(start)

    def reuse(mut self, mut desk: Desktop, start: String, cascade: Int) raises:
        """Uses a window that was closed earlier for a new document, setting
        it back to how it was when made."""
        self.in_use = True
        self.want_new = False
        self.want_close = False
        self.close_after_save = False
        self.want_paste = False
        self.cascade = cascade
        self.doc_path = String("")
        self.holds_file = False
        self.tw_ids = List[Int]()
        self.tw_txt = List[Int]()
        self.tw_child = List[Int]()
        self.menu_answered = False
        self.menu_shaded = List[Int]()
        self.modified = False
        self.fresh = True
        self.doc_w100 = 0
        self.doc_h100 = 0
        self.page_w = 0
        self.page_h = 0
        self.view_w = 0
        self.view_h = 0
        self.caret_h = 0
        self.owns_clip = False
        self.clip_out = False
        self.selecting = False
        self.engine_version = String("starting")
        # the new engine starts with the first tool: the palette shows it
        if self.tool != 0 and self.tool < len(self.tools):
            self.palette.set_selected(self.tool, False)
            self.palette.set_selected(0, True)
        self.tool = 0
        self.savebox.set_icon_text(self.save_name, self.cfg.save_leaf)
        self.open_engine(desk, start)

    def owns(self, window: Int) -> Bool:
        """Whether a window belongs to this document: its own window, a pane
        or one of its dialogue boxes."""
        return (window == self.win.handle or window == self.toolbar.handle or window == self.fbar.handle
                or window == self.palette.handle or window == self.inspector.handle
                or window == self.hruler.handle or window == self.vruler.handle
                or window == self.savebox.handle or window == self.askbox.handle
                or window == self.infobox.handle or window == self.choosebox.handle
                or window == self.showwin.handle or window == self.props.handle
                or window == self.qbox.handle)

    def close_doc(mut self, mut desk: Desktop) raises -> Bool:
        """Closes the window and stops its engine, after asking whether to
        discard any unsaved changes, or save them.  Returns False if the user
        kept it (or is choosing where to save it: the window closes once it
        has been saved)."""
        if self.modified:
            var a = discard_or_save(self.cfg.name, "\"" + self.doc_title
                                    + "\" has changes that are not saved.  Discard them?")
            if a == 0:
                return False
            if a == 2:
                self.set_save_kind(-1)
                if self.doc_path.byte_length() == 0:
                    self.close_after_save = True
                    self.open_savebox(desk)
                    return False
                var path = self.doc_path.copy()
                if not self.save_to(desk, path):
                    return False
        self.askbox.close()
        self.savebox.close()
        self.qbox.close()
        desk.close_window(self.win.handle)
        self.close_engine()
        self.in_use = False
        return True

    # ---- making things ----

    def make_menus(mut self) raises:
        self.main_menu.set_submenu(0, self.file_menu)
        self.main_menu.set_submenu(1, self.edit_menu)
        self.main_menu.set_submenu(2, self.view_menu)
        for i in range(len(self.own_menus)):
            self.main_menu.set_submenu(3 + i, self.own_menus[i])
        self.bar_menu.set_dialogue(0, self.infobox)
        if self.cfg.save_type != 0:
            self.file_menu.set_dialogue(1, self.savebox)
        self.bar_menu.set_submenu(3, self.docs_menu)
        self.view_menu.set_ticked(0, self.rulers)
        self.view_menu.set_ticked(1, self.guides)
        self.view_menu.set_ticked(2, self.grid)
        self.view_menu.set_ticked(6, self.props_on)

    def make_formula_bar(mut self) raises:
        """The formula bar: the current cell's reference, and its contents,
        which can be edited and are put into the cell by pressing Return."""
        self.fbar_ref = self.fbar.create_icon(8, -FORMULA_H + 8, 128, -8,
                                              1 | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 8)
                                              | (7 << 24) | (1 << 28), "A1", "", 16)
        self.fbar_input = self.fbar.writable(136, -FORMULA_H + 8, PANE_MAX - 8, -8, "", 1024)

    def make_panes(mut self) raises:
        var names = toolbar_names()
        var x = 6
        for i in range(len(names)):
            if names[i] == "|":
                x += 14
                continue
            if not self.cfg.view_toggles and (names[i] == "rs_rulers" or names[i] == "rs_guides"
                                              or names[i] == "rs_grid"):
                continue
            if self.cfg.viewport and not self.cfg.view_rulers and names[i] == "rs_rulers":
                continue                    # a viewport has no rulers of the shell's to show
            if names[i] == "rs_props" and not self.cfg.props:
                x -= 14                     # no pane: no switch, and no gap before it
                continue
            if not self.cfg.view_toggles and names[i] == "rs_zoomout":
                x -= 14                     # only one gap where two groups were left out
            var icon = self.toolbar.tool_button(x, -TOOLBAR_H + 6, x + 56, -6, names[i])
            self.tb_names.append(names[i])
            self.tb_icons.append(icon)
            x += 58
        if self.cfg.big_label.byte_length() > 0:
            # the application's own wide button, after a gap, with its icon
            # on the left and its label beside it, both centred vertically
            x += 14
            var icon = self.toolbar.create_icon(x, -TOOLBAR_H + 6, x + 172, -6,
                                                1 | 2 | 4 | (1 << 4) | (1 << 5)
                                                | (3 << 12) | (7 << 24) | (1 << 28),
                                                self.cfg.big_label, "S" + self.cfg.big_sprite + ";R5", 16)
            self.tb_names.append(String("rs_big"))
            self.tb_icons.append(icon)
            x += 174
        self.toolbar_w = x + 6
        self.set_toggle("rs_rulers", self.rulers)
        self.set_toggle("rs_guides", self.guides)
        self.set_toggle("rs_grid", self.grid)
        self.set_toggle("rs_props", self.props_on)
        self.tools = self.cfg.tools.copy()
        var y = -6
        for i in range(len(self.tools)):
            var icon = self.palette.tool_radio(8, y - 56, PALETTE_W - 8, y, self.tools[i], 1)
            if i == 0:
                self.palette.set_selected(icon, True)
            y -= 60
        # the inspector shows what is selected and where, the same in every
        # application; the application's own controls follow on the right
        var row1 = -8
        var row2 = -54
        self.page_label = self.inspector.create_icon(10, row1 - 40, 280, row1,
                                                     1 | (1 << 4) | (1 << 8) | (7 << 24), "Page 1", "", 32)
        self.sel_label = self.inspector.create_icon(10, row2 - 40, 280, row2,
                                                    1 | (1 << 4) | (1 << 8) | (7 << 24), "Nothing selected", "", 40)
        if not self.cfg.geometry:
            # a line of text from the engine about the selection (such as a
            # cell's value and format), right across the pane
            self.x_field = self.inspector.create_icon(290, row1 - 40, 2400, row1,
                                                      1 | (1 << 4) | (1 << 8) | (7 << 24), "", "", 256)
            self.y_field = self.inspector.create_icon(290, row2 - 40, 2400, row2,
                                                      1 | (1 << 4) | (1 << 8) | (7 << 24), "", "", 256)
            self.w_field = -1
            self.h_field = -1
            return
        _ = self.inspector.label(290, row1 - 40, 320, row1, "X")
        self.x_field = self.place_field(320, row1 - 40, 480, row1, "0.0mm")
        _ = self.inspector.label(290, row2 - 40, 320, row2, "Y")
        self.y_field = self.place_field(320, row2 - 40, 480, row2, "0.0mm")
        _ = self.inspector.label(500, row1 - 40, 532, row1, "W")
        self.w_field = self.place_field(532, row1 - 40, 692, row1, "")
        _ = self.inspector.label(500, row2 - 40, 532, row2, "H")
        self.h_field = self.place_field(532, row2 - 40, 692, row2, "")
        if self.cfg.colours:
            # the colour to paint with, chosen in the ColourPicker's dialogue
            self.colour_button = self.inspector.button(720, row1 - 44, 900, row1, "Colour...")
            self.colour_label = self.inspector.create_icon(720, row2 - 40, 900, row2,
                                                           1 | (1 << 4) | (1 << 8) | (7 << 24), "#000000", "", 12)


    def place_field(mut self, x0: Int, y0: Int, x1: Int, y1: Int, text: String) raises -> Int:
        """One of the inspector's place and size fields: Return in it comes
        to the program (K without r: Tab still moves between them)"""
        return self.inspector.create_icon(x0, y0, x1, y1, 1 | 4 | 16 | 32 | (15 << 12) | (7 << 24),
                                          text, "Pptr_write;Kta", 16)

    def show_options(mut self, line: String) raises:
        """Shows the current tool's options in the inspector, after its own
        fields.  The engine sends them when the tool changes.  NAME=VALUE is
        a number that can be typed, with - and + buttons to change it;
        NAME:TEXT is shown but cannot be changed.  Options with the same
        names as those shown are changed in place, as the properties pane's
        rows are (show_props)."""
        var parts0 = line.split("|")
        var shape = String("")
        var vals = List[String]()
        for k in range(len(parts0)):
            var f = String(parts0[k])
            var eq = f.find("=")
            var colon = f.find(":")
            if eq > 0:
                shape += String(f[byte=0:eq + 1]) + "\n"
                vals.append(String(f[byte=eq + 1:]))
            elif colon > 0:
                shape += String(f[byte=0:colon + 1]) + "\n"
                vals.append(String(f[byte=colon + 1:]))
        var fits = len(vals) == len(self.opt_vals)
        if fits:
            for k in range(len(vals)):
                if vals[k].byte_length() > self.inspector.text_room(self.opt_vals[k]):
                    fits = False
        if line.byte_length() > 0 and shape == self.opt_shape and fits:
            for k in range(len(vals)):
                if not caret_in(self.inspector.handle, self.opt_vals[k]):
                    self.inspector.set_icon_text(self.opt_vals[k], vals[k])
            return
        self.opt_shape = shape
        self.opt_vals = List[Int]()
        for i in range(len(self.opt_icons)):
            self.inspector.delete_icon(self.opt_icons[i])
        self.opt_icons = List[Int]()
        self.opt_names = List[String]()
        self.opt_fields = List[Int]()
        self.opt_minus = List[Int]()
        self.opt_plus = List[Int]()
        var x = 920 if self.cfg.colours else 720
        if not self.cfg.geometry:
            x = 1400
        var row1 = -8
        var row2 = -54
        if line.byte_length() > 0:
            var parts = line.split("|")
            for k in range(len(parts)):
                var f = String(parts[k])
                var eq = f.find("=")
                var colon = f.find(":")
                if eq > 0:
                    var name = String(f[byte=0:eq])
                    var value = String(f[byte=eq + 1:])
                    self.opt_icons.append(self.inspector.label(x, row1 - 40, x + 170, row1, name))
                    var field = self.inspector.writable(x, row2 - 40, x + 88, row2, value, 12)
                    var minus = self.inspector.button(x + 92, row2 - 40, x + 128, row2, "-")
                    var plus = self.inspector.button(x + 132, row2 - 40, x + 168, row2, "+")
                    self.opt_icons.append(field)
                    self.opt_vals.append(field)
                    self.opt_icons.append(minus)
                    self.opt_icons.append(plus)
                    self.opt_names.append(name)
                    self.opt_fields.append(field)
                    self.opt_minus.append(minus)
                    self.opt_plus.append(plus)
                    x += 184
                elif colon > 0:
                    self.opt_icons.append(self.inspector.label(x, row1 - 40, x + 300, row1, String(f[byte=0:colon])))
                    var shown = self.inspector.create_icon(x, row2 - 40, x + 300, row2,
                                                           1 | (1 << 4) | (1 << 8) | (7 << 24),
                                                           String(f[byte=colon + 1:]), "", 48)
                    self.opt_icons.append(shown)
                    self.opt_vals.append(shown)
                    x += 320
        self.inspector.force_redraw(0, -INSPECTOR_H, PANE_MAX, 0)

    def show_start(mut self, mut desk: Desktop) raises:
        """Starts a slide show.  A black window with no furniture covers the
        whole screen, the icon bar included, and the engine's page is bound
        to it at the size of the screen.  All keys and clicks go to the
        engine."""
        var sw = (vdu.mode_variable(11) + 1) << vdu.mode_variable(4)
        var sh = (vdu.mode_variable(12) + 1) << vdu.mode_variable(5)
        self.showwin.set_extent(sw, sh)
        var b = Buffer(40)
        b.set_word(0, self.showwin.handle)
        b.set_word(4, 0)
        b.set_word(8, 0)
        b.set_word(12, sw)
        b.set_word(16, sh)
        b.set_word(20, 0)
        b.set_word(24, 0)
        b.set_word(28, -1)
        desk._open_block(b.addr())
        var margins = Buffer(16)
        for k in range(4):
            margins.set_word(4 * k, 0)
        var r = Regs()
        r[0] = self.session
        r[1] = self.showwin.handle
        r[2] = sw // self.ospp
        r[3] = sh // self.ospp
        r[4] = self.ppmm100
        r[5] = margins.addr()
        r[6] = 0
        swi(ROSApps_Bind, r)
        self.showing = True
        self.showwin.set_caret()

    def show_end(mut self, mut desk: Desktop) raises:
        """Ends a slide show, binding the engine to the document's window
        again."""
        if not self.showing:
            return
        self.showing = False
        self.showwin.close()
        self.bind()
        self.open_document(desk)
        self.win.set_caret()

    def choose(mut self, line: String) raises:
        """Opens the choice box for the engine's command
        "choose ID|TITLE|PROMPT|SUGGESTIONS|ACTIONS", where the suggestions
        and actions are lists separated by commas.  The first suggestion is
        put in the box's field.  Clicking an action (or pressing Return for
        the first one) sends "answer ID ACTION|TEXT".  The box stays open
        until the engine sends another choice or "unchoose"."""
        var f = line.split("|")
        if len(f) < 5:
            return
        self.choose_id = String(f[0])
        self.choosebox.set_title(String(f[1]))
        self.choosebox.set_icon_text(self.choose_prompt, String(f[2]))
        for i in range(len(self.choose_icons)):
            self.choosebox.delete_icon(self.choose_icons[i])
        self.choose_icons = List[Int]()
        self.choose_sugg = List[Int]()
        self.choose_sugg_text = List[String]()
        self.choose_acts = List[Int]()
        self.choose_act_text = List[String]()
        var first = String("")
        var x = 16
        var sugg = String(f[3])
        if sugg.byte_length() > 0:
            var words = sugg.split(",")
            for i in range(min(len(words), 6)):
                var w = String(words[i])
                if i == 0:
                    first = w.copy()
                var wide = min(240, 40 + 16 * w.byte_length())
                if x + wide > 884:
                    break
                var b = self.choosebox.button(x, -172, x + wide, -124, w)
                self.choose_icons.append(b)
                self.choose_sugg.append(b)
                self.choose_sugg_text.append(w)
                x += wide + 8
        var acts = String(f[4]).split(",")
        x = 16
        for i in range(len(acts)):
            var a = String(acts[i])
            var wide = 40 + 16 * a.byte_length()
            var b = self.choosebox.button(x, -284, x + wide, -228, a)
            self.choose_icons.append(b)
            self.choose_acts.append(b)
            self.choose_act_text.append(a)
            x += wide + 12
        self.choosebox.set_icon_text(self.choose_field, first)
        self.choosebox.force_redraw(0, -300, 900, 0)
        if not self.choosebox.state().is_open():
            self.choosebox.open_centred()
        self.choosebox.set_caret(self.choose_field, first.byte_length())

    def question(mut self, line: String) raises:
        """Opens the question box for the engine's command
        "question ID|TITLE|TEXT|ACTIONS": the text, in up to three lines,
        and a button for each action (a list separated by commas), the first
        on the right, which Return presses; Escape presses the last.  A
        button sends "answer ID ACTION" and closes the box; "unquestion"
        closes it unanswered.  A game asks with it (!Tower's fires and bomb
        threats)."""
        var f = line.split("|")
        if len(f) < 4:
            return
        self.q_id = String(f[0])
        self.qbox.set_title(String(f[1]))
        # the text in lines of about 52 characters, broken at spaces
        var words = String(f[2]).split(" ")
        var lines = List[String]()
        var cur = String("")
        for i in range(len(words)):
            var w = String(words[i])
            if cur.byte_length() > 0 and cur.byte_length() + 1 + w.byte_length() > 52:
                lines.append(cur)
                cur = String("")
            if cur.byte_length() > 0:
                cur += " "
            cur += w
        if cur.byte_length() > 0:
            lines.append(cur)
        for i in range(3):
            self.qbox.set_icon_text(self.q_lines[i], lines[i] if i < len(lines) else String(""))
        for i in range(len(self.q_acts)):
            self.qbox.delete_icon(self.q_acts[i])
        self.q_acts = List[Int]()
        self.q_act_text = List[String]()
        var acts = String(f[3]).split(",")
        var x = 884
        for i in range(len(acts)):
            var a = String(acts[i])
            var wide = max(160, button_width(a))
            var b = self.qbox.button(x - wide, -204, x, -152, a)
            self.q_acts.append(b)
            self.q_act_text.append(a)
            x -= wide + 16
        self.qbox.force_redraw(0, -220, 900, 0)
        if not self.qbox.state().is_open():
            self.qbox.open_centred()
        self.qbox.set_caret()

    def question_answer(mut self, i: Int) raises:
        if i < 0 or i >= len(self.q_act_text):
            return
        self.send("answer " + self.q_id + " " + self.q_act_text[i])
        self.qbox.close()
        self.win.set_caret()

    def choose_click(mut self, icon: Int) raises:
        for i in range(len(self.choose_sugg)):
            if icon == self.choose_sugg[i]:
                self.choosebox.set_icon_text(self.choose_field, self.choose_sugg_text[i])
                self.choosebox.set_caret(self.choose_field, self.choose_sugg_text[i].byte_length())
                return
        for i in range(len(self.choose_acts)):
            if icon == self.choose_acts[i]:
                self.send("answer " + self.choose_id + " " + self.choose_act_text[i] + "|"
                          + self.choosebox.icon_text(self.choose_field))
                return

    def option_click(mut self, icon: Int) raises -> Bool:
        for i in range(len(self.opt_names)):
            if icon == self.opt_minus[i] or icon == self.opt_plus[i]:
                self.send("option " + self.opt_names[i] + (" -" if icon == self.opt_minus[i] else " +"))
                return True
        return False

    def make_info(mut self) raises:
        """The usual RISC OS Info box: name, purpose, author and version,
        with the engine's version underneath, as each program has two
        parts."""
        var rows = List[String]()
        for s in ["Name", "Purpose", "Author", "Version", "Engine"]:
            rows.append(String(s))
        var values = List[String]()
        values.append(self.cfg.name)
        values.append(self.cfg.purpose)
        values.append(String("ROSAPPS"))
        values.append(self.cfg.version)
        values.append(String("starting"))
        var y = -12
        for i in range(len(rows)):
            _ = self.infobox.label(12, y - 44, 170, y, rows[i])
            var v = self.infobox.create_icon(176, y - 44, 688, y,
                                             1 | (1 << 2) | (1 << 4) | (1 << 5) | (7 << 24) | (1 << 28),
                                             values[i], "", 64)
            if i == 4:
                self.info_engine = v
            y -= 52

    def make_savebox(mut self) raises:
        """The usual RISC OS save box: a file icon to drag to a directory,
        the file name, and a Save button for when a full name is typed."""
        var sprite = String("file_") + hex3(self.cfg.save_type)
        self.save_icon = self.savebox.sprite_icon(180, -120, 280, -20, sprite)
        self.save_name = self.savebox.writable(16, -184, 444, -136, self.cfg.save_leaf, 200)
        self.save_ok = self.savebox.button(324, -248, 444, -196, "Save")

    def save_type_now(self) -> Int:
        """The file type the save box will save: the document's own, or an
        export's"""
        if self.save_kind >= 0:
            return self.cfg.export_types[self.save_kind]
        if self.cfg.tabs and self.tab_type > 0:
            return self.tab_type
        return self.cfg.save_type

    def set_save_kind(mut self, kind: Int) raises:
        """Sets the save box up to save the document (kind -1) or one of its
        exports: its title, its file icon and the name in it."""
        self.close_after_save = False           # any other save keeps the window
        if kind == self.save_kind:
            return
        self.save_kind = kind
        self.savebox.delete_icon(self.save_icon)
        var sprite = String("file_") + hex3(self.save_type_now())
        self.save_icon = self.savebox.sprite_icon(180, -120, 280, -20, sprite)
        if kind >= 0:
            self.savebox.set_title("Export " + self.cfg.export_names[kind])
            var leaf = leaf_of(self.doc_title)
            self.savebox.set_icon_text(self.save_name, leaf if leaf.byte_length() > 0 else self.cfg.save_leaf)
        else:
            self.savebox.set_title("Save as")
            var name = self.doc_path if self.doc_path.byte_length() > 0 else self.cfg.save_leaf
            self.savebox.set_icon_text(self.save_name, name)
        self.savebox.force_redraw(0, -260, 460, 0)

    def make_askbox(mut self) raises:
        """The question box: the question, a line to type the answer in, and
        Cancel and OK buttons.  The engine uses it, with "ask", for things
        such as a word to find or the text of a note."""
        self.ask_prompt = self.askbox.create_icon(16, -60, 664, -16,
                                                  1 | (1 << 4) | (1 << 8) | (7 << 24), "", "", 160)
        self.ask_field = self.askbox.writable(16, -114, 664, -70, "", 256)
        self.ask_cancel = self.askbox.button(392, -180, 520, -128, "Cancel")
        self.ask_ok = self.askbox.button(536, -180, 664, -128, "OK")

    def ask(mut self, line: String) raises:
        """Opens the question box for the engine's command
        "ask ID|TITLE|PROMPT|DEFAULT", in the middle of the screen, with the
        caret at the end of the answer."""
        var f = line.split("|")
        if len(f) < 3:
            return
        self.ask_id = String(f[0])
        self.askbox.set_title(String(f[1]))
        self.askbox.set_icon_text(self.ask_prompt, String(f[2]))
        var given = String(f[3]) if len(f) >= 4 else String("")
        self.askbox.set_icon_text(self.ask_field, given)
        self.askbox.open_centred()
        self.askbox.set_caret(self.ask_field, given.byte_length())

    def answer(mut self) raises:
        self.send("answer " + self.ask_id + " " + self.askbox.icon_text(self.ask_field))
        self.askbox.close()
        self.win.set_caret()

    # ---- saving ----

    def new_document(mut self, mut desk: Desktop) raises:
        """New: asks the engine for an empty document in this window, after
        asking whether to discard any unsaved changes."""
        if self.modified and not confirm(self.cfg.name, "The document has changes that are not saved.  Discard them?"):
            return
        self.doc_path = String("")
        self.holds_file = False
        self.fresh = True
        self.savebox.set_icon_text(self.save_name, self.cfg.save_leaf)
        self.send(self.cfg.new_line)
        self.open_document(desk)

    # ---- printing ----

    def print_doc(mut self, mut desk: Desktop) raises:
        """Print: the engine draws each page at the printer's resolution as
        a sprite in the scrap directory, and the shell plots it through the
        printer driver to the printer chosen with *PrintTo."""
        var scrap = system.read_var("Wimp$ScrapDir")
        if scrap.byte_length() == 0:
            desk.report("There is no scrap directory to print through.")
            return
        var rpath = scrap + ".PrintPage"
        var buf = Buffer(1024)
        var lp = Regs()
        lp.set_string(0, rpath)
        lp[1] = buf.addr()
        lp[2] = 1024
        if swi_x(ROSApps_LinuxPath, lp) != 0:
            desk.report("Printing needs the scrap directory on HostFS, for now.")
            return
        var lpath = get_string(buf.addr())
        var f = Regs()
        f[0] = 0x8F
        f.set_string(1, "printer:")
        if swi_x(0x0D, f) != 0 or f[0] == 0:
            desk.report("The printer could not be opened.  Choose one with *PrintTo.")
            return
        var handle = f[0]
        var j = Regs()
        j[0] = handle
        j.set_string(1, self.doc_title)
        if swi_x(PDriver_SelectJob, j) != 0:
            desk.report("There is no printer driver.")
            _ = swi2(0x0D, 0, handle)
            return
        var ok = True
        var page = 1
        var pages = 1
        while page <= pages and ok:
            self.send("printpage " + String(page) + " " + String(PRINT_DPI) + " " + lpath)
            var reply = self.wait_line(desk, "printpage ")
            if not reply.startswith("printpage "):
                ok = False
                break
            var w = reply.split(" ")
            if len(w) >= 3:
                pages = Int(w[2])
            ok = self.print_sprite(rpath, page)
            page += 1
        if ok:
            _ = swi2(PDriver_EndJob, handle)
        else:
            _ = swi2(PDriver_AbortJob, handle)
            desk.report("The document could not be printed.")
        _ = swi2(0x0D, 0, handle)
        var d = Regs()
        d[0] = 6
        d.set_string(1, rpath)
        _ = swi_x(0x08, d)

    def wait_line(mut self, mut desk: Desktop, want: String) raises -> String:
        """Waits for the engine's next line that starts with want, dealing
        with any other lines as they arrive.  Returns "" on an error or
        after ten seconds."""
        var t = Regs()
        swi(OS_ReadMonotonicTime, t)
        var until = t[0] + 1000
        while True:
            var q = Regs()
            q[0] = self.session
            swi(ROSApps_Poll, q)
            if q[0] != 0:
                var line = get_string(q[0])
                if line.startswith(want):
                    return line
                if line.startswith("error "):
                    desk.report(String(line[byte=6:]))
                    return String("")
                self.handle_line(desk, line)
            else:
                swi(OS_ReadMonotonicTime, t)
                if t[0] > until:
                    return String("")

    def print_sprite(mut self, path: String, page: Int) raises -> Bool:
        """Prints one page.  The sprite file is loaded and given to the
        printer driver as one rectangle at the bottom left of the paper,
        then plotted for each rectangle the driver asks for."""
        var info = Regs()
        info[0] = 23
        info.set_string(1, path)
        if swi_x(0x08, info) != 0 or info[0] != 1:
            return False
        var size = info[4] + 64
        var area = Buffer(size)
        area.set_word(0, size)
        area.set_word(8, 16)
        var r = Regs()
        r[0] = 256 + 9
        r[1] = area.addr()
        if swi_x(OS_SpriteOp, r) != 0:
            return False
        var l = Regs()
        l[0] = 256 + 10
        l[1] = area.addr()
        l.set_string(2, path)
        if swi_x(OS_SpriteOp, l) != 0:
            return False
        var sp = area.addr() + area.word(8)    # the sprite
        var pw = peek(sp + 16) + 1             # 32 bpp: one word per pixel
        var ph = peek(sp + 20) + 1
        var rect = Buffer(16)
        rect.set_word(0, 0)
        rect.set_word(4, 0)
        rect.set_word(8, pw)                    # OS units: one per pixel at 180 dpi
        rect.set_word(12, ph)
        var m = Buffer(16)
        m.set_word(0, 0x10000)
        m.set_word(4, 0)
        m.set_word(8, 0)
        m.set_word(12, 0x10000)
        var at = Buffer(8)
        at.set_word(0, 0)
        at.set_word(4, 0)
        var g = Regs()
        g[0] = 1
        g[1] = rect.addr()
        g[2] = m.addr()
        g[3] = at.addr()
        g[4] = Int(0xFFFFFF00)
        if swi_x(PDriver_GiveRectangle, g) != 0:
            return False
        var blk = Buffer(16)
        var dp = Regs()
        dp[0] = 1
        dp[1] = blk.addr()
        dp[2] = page
        dp[3] = 0
        if swi_x(PDriver_DrawPage, dp) != 0:
            return False
        var more = dp[0]
        while more != 0:
            var put = Regs()
            put[0] = 256 + 52
            put[1] = area.addr()
            put.set_string(2, "page")
            put[3] = 0
            put[4] = 0
            put[5] = 0
            put[6] = 0
            put[7] = 0
            _ = swi_x(OS_SpriteOp, put)
            var gr = Regs()
            gr[1] = blk.addr()
            if swi_x(PDriver_GetRectangle, gr) != 0:
                return False
            more = gr[0]
        return True

    def save_doc(mut self, mut desk: Desktop) raises:
        """Save: back to the file it came from, if there is one; otherwise
        the save box opens to ask for a name."""
        if self.doc_path.byte_length() > 0:
            var path = self.doc_path.copy()     # a copy, as saving changes self
            _ = self.save_to(desk, path)
        else:
            self.open_savebox(desk)

    def open_savebox(mut self, mut desk: Desktop) raises:
        """Opens the save box at the pointer.  Once the document has been
        saved, the box holds its full name, as Edit and Draw do; before
        that, just a leaf name."""
        var p = Buffer(20)
        var r = Regs(0, p.addr())
        _ = swi_x(Wimp_GetPointerInfo, r)
        desk.open_dialogue(self.savebox, p.word(0) - 200, p.word(4) + 100)

    def save_to(mut self, mut desk: Desktop, path: String) raises -> Bool:
        """The engine writes the document to the Linux name of path.  The
        shell waits for it to finish, then sets the file's type."""
        var buf = Buffer(1024)
        var r = Regs()
        r.set_string(0, path)
        r[1] = buf.addr()
        r[2] = 1024
        if swi_x(ROSApps_LinuxPath, r) != 0:
            desk.report("Documents can be saved only on HostFS, for now.")
            return False
        var exporting = self.save_kind >= 0
        var ftype = self.save_type_now()
        if exporting:
            self.send("export " + self.cfg.export_exts[self.save_kind] + " " + get_string(buf.addr()))
        elif self.cfg.tabs:
            # the tab keeps its RISC OS name and type, for the "tab" line
            self.send("save " + get_string(buf.addr()) + "|" + path + "|" + hex3(ftype))
        else:
            self.send("save " + get_string(buf.addr()))
        var t = Regs()
        swi(OS_ReadMonotonicTime, t)
        var until = t[0] + 1000              # ten seconds
        while True:
            var q = Regs()
            q[0] = self.session
            swi(ROSApps_Poll, q)
            if q[0] != 0:
                var line = get_string(q[0])
                if line.startswith("saved "):
                    files.set_type(path, ftype)
                    if exporting:
                        # an export is a copy, so the document keeps its name
                        desk.close_menu()
                        self.savebox.close()
                        self.set_save_kind(-1)
                        return True
                    if path != "<Wimp$Scrap>":
                        # saved to a file, not passed to another program:
                        # no longer changed (the engine says so too, later)
                        self.modified = False
                        self.doc_path = path
                        self.set_title(leaf_of(path))
                        self.savebox.set_icon_text(self.save_name, path)
                    desk.close_menu()
                    self.savebox.close()
                    if self.close_after_save:
                        # Save in the discard question: the window closes now
                        self.close_after_save = False
                        self.want_close = True
                    return True
                if line.startswith("error "):
                    desk.report(String(line[byte=6:]))
                    return False
                self.handle_line(desk, line)
            else:
                swi(OS_ReadMonotonicTime, t)
                if t[0] > until:
                    desk.report("The engine did not save the document.")
                    return False

    def save_file(mut self, mut desk: Desktop, path: String) raises -> Bool:
        if self.clip_out:
            self.clip_out = False
            return self.write_clipboard(desk, path)
        return self.save_to(desk, path)

    # ---- the clipboard (the RISC OS clipboard protocol) ----

    def claim_clipboard(mut self, mut desk: Desktop) raises:
        """Cut or Copy: claims the clipboard and tells every other program
        (Message_ClaimEntity, flags bit 2)."""
        var b = Buffer(24)
        b.set_word(0, 24)
        b.set_word(12, 0)
        b.set_word(16, MSG_CLAIM_ENTITY)
        b.set_word(20, 4)
        var r = Regs(17, b.addr(), 0)
        _ = swi_x(Wimp_SendMessage, r)
        self.owns_clip = True

    def paste(mut self, mut desk: Desktop) raises:
        """Paste: from the engine's own clipboard if we hold it.  Otherwise
        the program that holds it is asked for it as text
        (Message_DataRequest), and it arrives as a file saved to the
        window."""
        if self.owns_clip:
            self.send("paste")
            return
        # another of our documents may hold it: RosApp.after looks, and
        # asks the other programs if not
        self.want_paste = True

    def request_clip(mut self) raises:
        """Asks the program that holds the clipboard for its contents."""
        var b = Buffer(48)
        b.set_word(0, 48)
        b.set_word(12, 0)
        b.set_word(16, MSG_DATA_REQUEST)
        b.set_word(20, self.win.handle)
        b.set_word(24, -1)
        var st = self.win.state()
        b.set_word(28, st.x0 + 100)
        b.set_word(32, st.y1 - 200)
        b.set_word(36, 4)                   # flag: from the clipboard
        b.set_word(40, TYPE_TEXT)
        b.set_word(44, -1)
        var r = Regs(17, b.addr(), 0)
        _ = swi_x(Wimp_SendMessage, r)

    def choose_colour(mut self, c: Click, rgb: Int) raises:
        """Opens the ColourPicker's dialogue at the pointer, like a menu,
        showing the colour rgb (&RRGGBB).  The colour chosen arrives as
        Message_ColourPickerColourChoice."""
        var b = Buffer(48)
        b.set_word(0, 0)
        b.set_word(4, 0)
        b.set_word(8, c.x - 64)                 # visible area x0, y0, x1, y1; only the top left is used
        b.set_word(12, c.y - 600)
        b.set_word(16, c.x + 600)
        b.set_word(20, c.y + 32)
        b.set_word(24, 0)
        b.set_word(28, 0)
        b.set_word(32, ((rgb & 0xFF) << 24) | (((rgb >> 8) & 0xFF) << 16) | (((rgb >> 16) & 0xFF) << 8))
        b.set_word(36, 0)                       # the default model
        var r = Regs()
        r[0] = 1                                # transient
        r[1] = b.addr()
        _ = swi_x(ColourPicker_OpenDialogue, r)

    def show_colour(mut self):
        if self.colour_label >= 0:
            self.inspector.set_icon_text(self.colour_label, "#" + hex2((self.colour >> 16) & 255)
                                         + hex2((self.colour >> 8) & 255) + hex2(self.colour & 255))

    def message(mut self, mut desk: Desktop, number: Int, block: Int) raises:
        if number == MSG_COLOUR_CHOICE:
            var w = peek(block + 28)            # &BBGGRR00
            var rgb = (((w >> 8) & 0xFF) << 16) | (((w >> 16) & 0xFF) << 8) | ((w >> 24) & 0xFF)
            if self.props_colour.byte_length() > 0:
                # for a colour row in the properties pane
                self.send("prop " + self.props_colour + " #" + hex2((rgb >> 16) & 255)
                          + hex2((rgb >> 8) & 255) + hex2(rgb & 255))
                self.props_colour = String("")
                return
            self.colour = rgb
            self.show_colour()
            self.send("colour " + hex2((self.colour >> 16) & 255) + hex2((self.colour >> 8) & 255)
                      + hex2(self.colour & 255))
            return
        if number == MSG_CLAIM_ENTITY:
            if peek(block + 4) != desk.task and (peek(block + 20) & 4) != 0:
                self.owns_clip = False      # another program holds the clipboard now
        elif number == MSG_DATA_REQUEST:
            if self.owns_clip and (peek(block + 36) & 4) != 0:
                # we hold it: offer it as text, to where the request says
                var leaf = String("Clipboard")
                var b = Buffer(64)
                b.set_word(0, (44 + leaf.byte_length() + 1 + 3) & ~3)
                b.set_word(12, peek(block + 8))
                b.set_word(16, 1)           # Message_DataSave
                b.set_word(20, peek(block + 20))
                b.set_word(24, peek(block + 24))
                b.set_word(28, peek(block + 28))
                b.set_word(32, peek(block + 32))
                b.set_word(36, 4096)
                b.set_word(40, TYPE_TEXT)
                b.set_string(44, leaf)
                var r = Regs(18, b.addr(), peek(block + 4))
                if swi_x(Wimp_SendMessage, r) == 0:
                    self.clip_out = True
                    desk.drag_ref = b.word(8)   # the reply (DataSaveAck) goes to save_file

    def write_clipboard(mut self, mut desk: Desktop, path: String) raises -> Bool:
        """Gets the clipboard's text from the engine and saves it to path as
        a text file."""
        self.send("clipget")
        var data = List[Int]()
        var t = Regs()
        swi(OS_ReadMonotonicTime, t)
        var until = t[0] + 500
        while True:
            var q = Regs()
            q[0] = self.session
            swi(ROSApps_Poll, q)
            if q[0] == 0:
                swi(OS_ReadMonotonicTime, t)
                if t[0] > until:
                    return False
                continue
            var line = get_string(q[0])
            if line == "clipend":
                break
            if line.startswith("clip "):
                unescape_into(String(line[byte=5:]), data)
            else:
                self.handle_line(desk, line)
        var buf = Buffer(len(data) + 1)
        for i in range(len(data)):
            buf.set_byte(i, data[i])
        var r = Regs()
        r[0] = 10
        r.set_string(1, path)
        r[2] = TYPE_TEXT
        r[4] = buf.addr()
        r[5] = buf.addr() + len(data)
        return swi_x(0x08, r) == 0

    def paste_file(mut self, path: String) raises:
        """Sends text to the engine to go in at the caret.  The text comes
        from another program's clipboard, or from a text file dropped on
        the document.  It is escaped and sent in pieces."""
        var r = Regs()
        r[0] = 23
        r.set_string(1, path)
        if swi_x(0x08, r) != 0 or r[0] != 1:
            return
        var n = r[4]
        var buf = Buffer(n + 1)
        var l = Regs()
        l[0] = 16
        l.set_string(1, path)
        l[2] = buf.addr()
        l[3] = 0
        if swi_x(0x08, l) != 0:
            return
        var piece = String("")
        for i in range(n):
            var c = buf.byte(i)
            if c == 92:
                piece += "\\\\"
            elif c == 10:
                piece += "\\n"
            elif c == 13:
                piece += "\\n"
            elif c == 9:
                piece += "\\t"
            elif c >= 32 and c < 127:
                piece += chr(c)
            elif c >= 160:
                piece += "\\x" + hex2(c)
            if piece.byte_length() >= 600:
                self.send("pastechunk " + piece)
                piece = String("")
        if piece.byte_length() > 0:
            self.send("pastechunk " + piece)
        self.send("pasteend")

    def set_title(mut self, title: String):
        self.doc_title = title
        self.win.set_title(title + (" *" if self.modified else ""))
        if self.doc_path.byte_length() == 0:
            # a file's name has no spaces in RISC OS (they end it)
            self.savebox.set_icon_text(self.save_name, title.replace(" ", "_"))

    # ---- Choices ----

    def load_choices(mut self):
        """Reads the settings saved last time from Choices:<name>.Choices,
        which has a word and a number on each line."""
        try:
            var at = "Choices:" + self.cfg.name + ".Choices"
            if not files.exists(at):
                return
            var text = files.load_text(at)
            for line in text.split("\n"):
                var w = line.split(" ")
                if len(w) < 2:
                    continue
                var v = Int(w[1])
                if w[0] == "rulers":
                    self.rulers = v != 0 and self.cfg.rulers and (not self.cfg.viewport or self.cfg.view_rulers)
                elif w[0] == "guides":
                    self.guides = v != 0
                elif w[0] == "grid":
                    self.grid = v != 0
                elif w[0] == "zoom":
                    self.zoom = max(5, min(400, v))
                elif w[0] == "props":
                    self.props_on = v != 0
        except e:
            pass                            # a damaged file is treated as no file

    def save_choices(self):
        try:
            var text = String("rulers ") + ("1" if self.rulers else "0") + "\n"
            text += String("guides ") + ("1" if self.guides else "0") + "\n"
            text += String("grid ") + ("1" if self.grid else "0") + "\n"
            text += String("zoom ") + String(self.zoom_kept if self.engine_zoomed else self.zoom) + "\n"
            text += String("props ") + ("1" if self.props_on else "0") + "\n"
            try:
                files.create_dir("<Choices$Write>." + self.cfg.name)
            except e:
                pass
            files.save_text("<Choices$Write>." + self.cfg.name + ".Choices", text)
        except e:
            pass                            # nowhere to write them, so they are not kept

    # ---- the page ----

    def bind(mut self) raises:
        """Binds the engine's page to the window at the current zoom, below
        the toolbar and inside the rulers."""
        if self.doc_w100 <= 0:
            return                          # the engine has not yet said how big its picture is
        if self.cfg.tiles and not self.tiles_off and self.bind_tiles():
            return
        if self.cfg.viewport:
            self.bind_view()
            return
        self.page_w = max(1, self.doc_w100 * self.ppmm100 // 10000)
        self.page_h = max(1, self.doc_h100 * self.ppmm100 // 10000)
        # make the page at least as wide as the toolbar, so the engine's
        # desk fills the window to the end of the toolbar (an empty margin
        # there was left for the Wimp to paint, and on the Mac it could
        # keep showing an old picture)
        self.page_w = max(self.page_w, (self.min_width() - RULER_W) // self.ospp)
        # and tall enough for the whole tool palette beside it
        self.page_h = max(self.page_h, (self.min_height() - TOOLBAR_H - (RULER_H if self.rulers else 0)) // self.ospp)
        if self.page_w * self.page_h * 4 > PAGE_BYTES:
            self.page_h = PAGE_BYTES // (4 * self.page_w)    # later pages are left off
        self.mleft = RULER_W if self.rulers else 0
        self.mtop = TOOLBAR_H + (RULER_H if self.rulers else 0)
        var margins = Buffer(16)
        margins.set_word(0, self.mleft)
        margins.set_word(4, 0)
        margins.set_word(8, 0)
        margins.set_word(12, self.mtop)
        var b = Regs()
        b[0] = self.session
        b[1] = self.win.handle
        b[2] = self.page_w
        b[3] = self.page_h
        b[4] = self.ppmm100
        b[5] = margins.addr()
        swi(ROSApps_Bind, b)

    def min_width(self) -> Int:
        """The narrowest a window opens, in OS units: wide enough for the
        toolbar, and for the inspector's colour and tool options if it has
        them."""
        if self.cfg.colours:
            return max(self.toolbar_w, 1500)
        return self.toolbar_w

    def min_height(self) -> Int:
        """The shortest a window opens, in OS units: tall enough for every
        tool in the palette (which is as tall as the window) under the
        toolbar."""
        return TOOLBAR_H + 12 + 60 * len(self.tools)

    def bind_tiles(mut self) raises -> Bool:
        """Binds the engine's tile map to an icon as big as the whole map at
        the zoom, under the toolbar.  The presenter draws the map in it from
        the tiles (ROSApps_Tiles): the Wimp scrolls it, and a new zoom is a
        new size of icon, so neither costs the engine a pixel.  False if no
        presenter draws tile maps here: the caller binds a page instead."""
        var w = max(1, self.doc_w100 * self.ppmm100 // 10000)
        var h = max(1, self.doc_h100 * self.ppmm100 // 10000)
        self.mleft = 0
        self.mtop = TOOLBAR_H
        if self.tiles_icon >= 0:
            self.win.delete_icon(self.tiles_icon)
        # no text and no sprite: the Wimp draws nothing of it.  Its clicks
        # are reported as the work area's are.
        self.tiles_icon = self.win.create_icon(0, -self.mtop - h * self.ospp, w * self.ospp, -self.mtop,
                                               BUTTON_DOUBLE_CLICK_DRAG << 12, "")
        var r = Regs(self.session, self.win.handle, self.tiles_icon)
        swi(ROSApps_Scene3d if self.cfg.scene3d else (ROSApps_Tower if self.cfg.tower else ROSApps_Tiles), r)
        if r[0] == 0:
            self.win.delete_icon(self.tiles_icon)
            self.tiles_icon = -1
            self.tiles_off = True
            return False
        self.page_w = w
        self.page_h = h
        # the pointer's place is in the map, the scroll included, as on a page
        self.cfg.viewport = False
        self.win.set_extent(w * self.ospp, h * self.ospp + self.mtop)
        self.win.redraw_all()
        self.send("scale " + String(self.ppmm100))
        return True

    def bind_view(mut self) raises:
        """Binds a viewport: a picture the size of the window's visible area,
        less the toolbar and the formula bar, which stays in place however
        the window scrolls.  The engine draws the part of the document in
        view."""
        var st = self.win.state()
        self.mleft = 0
        self.mtop = TOOLBAR_H + (FORMULA_H if self.cfg.formula_bar else 0)
        var w = max(16, (st.x1 - st.x0 - self.mleft) // self.ospp)
        var h = max(16, (st.y1 - st.y0 - self.mtop) // self.ospp)
        self.view_w = w
        self.view_h = h
        self.page_w = w
        self.page_h = h
        var margins = Buffer(16)
        margins.set_word(0, self.mleft)
        margins.set_word(4, 0)
        margins.set_word(8, 0)
        margins.set_word(12, self.mtop)
        var b = Regs()
        b[0] = self.session
        b[1] = self.win.handle
        b[2] = w
        b[3] = h
        b[4] = self.ppmm100
        b[5] = margins.addr()
        b[6] = 1                            # fixed to the window
        swi(ROSApps_Bind, b)
        self.send_view()

    def send_view(mut self) raises:
        """Tells the engine where the view is in the document, in pixels."""
        var st = self.win.state()
        self.send("view " + String(st.scroll_x // self.ospp) + " " + String(-st.scroll_y // self.ospp))

    def fit_zoom(mut self):
        """For a new picture bigger than the screen, picks the zoom that
        shows it whole (never more than 100%), as a paint program does
        when it opens a photo."""
        var sw = ((vdu.mode_variable(11) + 1) << vdu.mode_variable(4)) - PALETTE_W - 120
        var sh = ((vdu.mode_variable(12) + 1) << vdu.mode_variable(5)) - 136 - INSPECTOR_H - 140 - self.mtop
        var w = self.doc_w100 * ACTUAL // 10000 * self.ospp       # OS units at 100%
        var h = self.doc_h100 * ACTUAL // 10000 * self.ospp
        if self.pages > 1:
            # pages one under another: fit one page, and the desk round it
            h = (self.pg_h100 + 2000) * ACTUAL // 10000 * self.ospp
        var z = 100
        if w > sw:
            z = min(z, sw * 100 // w)
        if h > sh:
            z = min(z, sh * 100 // h)
        self.zoom = max(5, z)
        self.ppmm100 = ACTUAL * self.zoom // 100

    def page_zoom(mut self) raises -> Int:
        """The zoom that shows a whole page, and the desk round it, in the
        window as it is now (at most 400%)."""
        var st = self.win.state()
        var vw = st.x1 - st.x0 - self.mleft
        var vh = st.y1 - st.y0 - self.mtop
        var w = self.doc_w100 * ACTUAL // 10000 * self.ospp       # OS units at 100%
        var h = (self.pg_h100 + 2000) * ACTUAL // 10000 * self.ospp
        var z = 400
        if w > 0:
            z = min(z, vw * 100 // w)
        if h > 0:
            z = min(z, vh * 100 // h)
        return max(5, z)

    def size_to_page(mut self, mut desk: Desktop) raises:
        """Sizes a new document's window to be wide enough for the whole
        page and the desk either side of it, and as tall as the screen
        allows (above the icon bar, with the inspector below it).  The tool
        palette sits outside it on the left."""
        if self.doc_w100 <= 0:
            return
        var sw = (vdu.mode_variable(11) + 1) << vdu.mode_variable(4)
        var sh = (vdu.mode_variable(12) + 1) << vdu.mode_variable(5)
        var want_w = self.mleft + (self.doc_w100 * self.ppmm100 // 10000) * self.ospp
        if self.cfg.viewport and self.cfg.formula_bar:
            want_w = sw                         # a spreadsheet: as much as the screen holds
        var x0 = PALETTE_W + 8 + self.cascade
        # no narrower than the toolbar, if the screen allows, or a small
        # picture would hide the toolbar's last buttons
        want_w = max(want_w, self.min_width())
        # room for the scroll bar, and for the properties pane if it is shown
        var w = min(want_w, sw - x0 - 48 - (PROPS_W if self.props_on else 0))
        var top = sh - 44 - self.cascade        # under the title bar
        var foot = 136 + INSPECTOR_H + 48       # the icon bar, the inspector, the scroll bar
        var want_h = max(self.mtop + (self.doc_h100 * self.ppmm100 // 10000) * self.ospp, self.min_height())
        if self.cfg.viewport:
            want_h = sh                         # a viewport: as tall as the screen allows
        var h = min(want_h, top - foot)
        if w < 400 or h < 400:
            return                              # a small screen: leave the window as it is
        var b = Buffer(40)
        b.set_word(0, self.win.handle)
        var r = Regs(0, b.addr())
        swi(Wimp_GetWindowState, r)
        b.set_word(4, x0)
        b.set_word(8, top - h)
        b.set_word(12, x0 + w)
        b.set_word(16, top)
        b.set_word(20, 0)
        b.set_word(24, 0)
        b.set_word(28, -1)
        desk._open_block(b.addr())

    def open_document(mut self, mut desk: Desktop, at_top: Bool = False) raises:
        """Opens the window with its panes outside it, and the toolbar and
        rulers inside it as child windows whose edges follow the
        window's."""
        var was_open = self.win.state().is_open()
        desk.open_window(self.win, at_top)
        var st = self.win.state()
        self.toolbar.open_in(self.win, st.x0, st.y1 - TOOLBAR_H, st.x1, st.y1,
                             nest_links(LINK_LOW, LINK_HIGH, LINK_HIGH, LINK_HIGH))
        if self.cfg.formula_bar:
            self.fbar.open_in(self.win, st.x0, st.y1 - TOOLBAR_H - FORMULA_H, st.x1, st.y1 - TOOLBAR_H,
                              nest_links(LINK_LOW, LINK_HIGH, LINK_HIGH, LINK_HIGH))
        self.open_rulers()
        # a window opened has the input focus, as Draw's and Edit's have: keys
        # go to it at once (an engine with a caret of its own places it later).
        # Opened again (zoomed, a pane shown), it keeps the caret it has.
        if not was_open or self.cfg.viewport:
            self.win.set_caret()

    def open_rulers(mut self) raises:
        if not self.rulers or self.cfg.viewport:
            self.hruler.close()
            self.vruler.close()
            return
        var st = self.win.state()
        var top = st.y1 - TOOLBAR_H
        self.hruler.open_in(self.win, st.x0 + RULER_W, top - RULER_H, st.x1, top,
                            nest_links(LINK_LOW, LINK_HIGH, LINK_HIGH, LINK_HIGH))
        self.vruler.open_in(self.win, st.x0, st.y0, st.x0 + RULER_W, top,
                            nest_links(LINK_LOW, LINK_LOW, LINK_LOW, LINK_HIGH))

    def zoom_at(mut self, mut desk: Desktop, c: Click, closer: Bool) raises:
        var st = self.win.state()
        var old = self.zoom
        var z = (old * 3 // 2 if old >= 100 else old + 25) if closer else (old * 2 // 3 if old > 100 else old - 25)
        # the point clicked, in the work area, before zooming
        var wx = c.work_x - self.mleft
        var wy = -c.work_y - self.mtop
        self.set_zoom(desk, z)
        var nx = wx * self.zoom // old
        var ny = wy * self.zoom // old
        var px = c.x - st.x0                    # the pointer, in the visible area
        var py = st.y1 - c.y
        desk.scroll_window(self.win, max(0, nx + self.mleft - px), min(0, -(ny + self.mtop - py)))

    def set_zoom(mut self, mut desk: Desktop, z: Int) raises:
        self.engine_zoomed = False              # the user's zoom now
        self.zoom = max(5, min(400, z))
        self.ppmm100 = ACTUAL * self.zoom // 100
        self.bind()
        self.open_document(desk)
        self.save_choices()

    # ---- the properties pane ----

    def toggle_props(mut self, mut desk: Desktop) raises:
        """Shows or hides the properties pane on the window's right."""
        self.props_on = not self.props_on
        self.view_menu.set_ticked(6, self.props_on)
        self.set_toggle("rs_props", self.props_on)
        if self.props_on:
            desk.attach_pane(self.win, self.props, PANE_RIGHT, PROPS_W, TOOLBAR_H)
            self.open_document(desk)
        else:
            desk.detach_pane(self.win, self.props)
        self.save_choices()

    def show_props(mut self, line: String) raises:
        """Fills the properties pane from the engine's line "props ROW|ROW...".
        A row is one of:
          #TEXT            a heading
          NAME:TEXT        a value shown, not changed here
          NAME=VALUE       a value typed in, sent with Return
          NAME?NOW?A,B,C   a choice from a menu, NOW the one chosen
          NAME!on or !off  a switch (an option icon)
          NAME~#RRGGBB     a colour, typed in or chosen in the ColourPicker's dialogue
        A field's NAME may start with ">": the caret is put in it, to type.
          *NAME            a button (*=NAME: drawn pressed in)
        The engine is sent "prop NAME VALUE" (a button: "prop NAME").

        Engines tell the pane again whenever anything in it may have changed
        -- a word count as you type, a size as you drag.  A line with the
        same rows as the last (only the values differ) is shown in place:
        each value that changed is set, nothing else is touched or drawn, and
        a field the caret is in is left as the user is typing it.  Only a
        line of another shape makes the pane again."""
        var rows = line.split("|")
        var shape = String("")
        var vals = List[String]()
        for k in range(len(rows)):
            var f = String(rows[k])
            if f.byte_length() == 0:
                continue
            var sp = prop_split(f)
            shape += sp[0] + "\n"
            vals.append(sp[1])
        if line.byte_length() > 0 and shape == self.props_shape and len(vals) == len(self.props_rows):
            # in place, if every value fits the icon made for it
            var fits = True
            for k in range(len(vals)):
                var icon = self.props_rows[k]
                var kind = self.props_rkind[k]
                if icon >= 0 and (kind == 0 or kind == 1 or kind == 5 or kind == 6):
                    var want = vals[k].byte_length() + (3 if kind == 1 else 0)
                    if want > self.props.text_room(icon):
                        fits = False
            if fits:
                for k in range(len(vals)):
                    self.prop_update(k, vals[k])
                self.props_line = line
                return
        self.props_shape = shape
        self.props_line = line
        for i in range(len(self.props_icons)):
            self.props.delete_icon(self.props_icons[i])
        self.props_icons = List[Int]()
        self.props_names = List[String]()
        self.props_kinds = List[Int]()
        self.props_values = List[String]()
        self.props_choices = List[String]()
        self.props_hit = List[Int]()
        self.props_rows = List[Int]()
        self.props_rkind = List[Int]()
        self.props_rhit = List[Int]()
        self.props_sent = -1
        var y = -12
        var right = PROPS_W - 12
        var focus = -1                      # a field marked >NAME: the caret goes in it
        var focus_at = 0
        if line.byte_length() == 0:
            self.props_icons.append(self.props.label(12, y - 44, right, y, "Nothing selected"))
        var bx = 12                         # where the next button goes on its row
        for k in range(len(rows)):
            var f = String(rows[k])
            if f.byte_length() == 0:
                continue
            var kind = prop_split(f)[2]
            self.props_rkind.append(kind)
            self.props_rhit.append(len(self.props_hit) if kind >= 0 and kind != 5 else -1)
            if f.startswith("*"):
                # buttons share a row while they fit; *=NAME is drawn pressed
                # in (the part of the pane showing, as a tab)
                var name = String(f[byte=1:])
                var pressed = name.startswith("=")
                if pressed:
                    var n = String(name[byte=1:])
                    name = n^
                var wide = button_width(name)
                if bx > 12 and bx + wide > right:
                    bx = 12
                    y -= 56
                var b = self.props.button(bx, y - 48, bx + wide, y, name)
                self.add_prop(b, name, 3, String(""), String(""))
                self.props_rows.append(b)
                if pressed:
                    self.props.set_selected(b, True)
                bx += wide + 8
                continue
            if f.startswith("^"):
                # a picture that is a button: ^NAME@WxH, ^=NAME@WxH drawn pressed in; W and H
                # are the sprite's size in pixels, and pictures share a row while they fit
                var pressed = f.startswith("^=")
                var rest = String(f[byte=2:]) if pressed else String(f[byte=1:])
                if bx > 12 and not (k > 0 and String(rows[k - 1]).startswith("^")):
                    bx = 12                 # after buttons: a row of its own
                    y -= 56
                var at = rest.find("@")
                var sname = String(rest[byte=0:at]) if at > 0 else rest.copy()
                var pw = 56
                var ph = 80
                if at > 0:
                    var wh = String(rest[byte=at + 1:]).split("x")
                    if len(wh) == 2:
                        pw = Int(String(wh[0]))
                        ph = Int(String(wh[1]))
                var bw = pw * self.ospp + 12
                var bh = ph * self.ospp + 12
                if bx > 12 and bx + bw > right:
                    bx = 12
                    y -= bh + 8
                var b = self.props.tool_button(bx, y - bh, bx + bw, y, sname)
                self.add_prop(b, sname, 7, String(""), String(""))
                self.props_rows.append(b)
                if pressed:
                    self.props.set_pressed(b, True)
                bx += bw + 8
                # the row's height is the tallest picture's: the next row starts below
                if k + 1 >= len(rows) or not String(rows[k + 1]).startswith("^"):
                    bx = 12
                    y -= bh + 8
                continue
            if bx > 12:
                bx = 12
                y -= 56
            if f.startswith("#"):
                y -= 8
                self.props_icons.append(self.props.create_icon(12, y - 44, right, y,
                                                               1 | (1 << 4) | (1 << 5) | (1 << 8)
                                                               | (7 << 24) | (2 << 28), " " + String(f[byte=1:]), "", 64))
                self.props_rows.append(-1)
                y -= 52
                continue
            var eq = f.find("=")
            var colon = f.find(":")
            var q = f.find("?")
            var bang = f.find("!")
            var tilde = f.find("~")
            var cut = f.byte_length()
            for c in [eq, colon, q, bang, tilde]:
                if c > 0 and c < cut:
                    cut = c
            if cut >= f.byte_length():
                self.props_icons.append(self.props.label(12, y - 44, right, y, f))
                self.props_rows.append(-1)
                y -= 52
                continue
            var name = String(f[byte=0:cut])
            var rest = String(f[byte=cut + 1:])
            var wants_caret = name.startswith(">")
            if wants_caret:
                var n = String(name[byte=1:])
                name = n^
            if cut != bang:
                self.props_icons.append(self.props.label(12, y - 44, PROPS_LABEL, y, name))
            if cut == bang:
                # an option icon, as RISC OS has them: the box, then its name;
                # the box is ticked when the icon is selected
                var on = rest == "on"
                var o = self.props.create_icon(12, y - 44, right, y,
                                               1 | 2 | (1 << 4) | ((1 << 21) if on else 0)
                                               | (3 << 12) | (7 << 24),
                                               name, "Soptoff,opton", 64)
                self.add_prop(o, name, 2, String("on" if on else "off"), String(""))
                self.props_rows.append(o)
            elif cut == q:
                var q2 = rest.find("?")
                var now = String(rest[byte=0:q2]) if q2 >= 0 else rest.copy()
                var items = String(rest[byte=q2 + 1:]) if q2 >= 0 else String("")
                var m = self.props.button(PROPS_LABEL, y - 48, right, y, now + "  >")
                self.add_prop(m, name, 1, now, items)
                self.props_rows.append(m)
            elif cut == tilde:
                # a field as below, with a button beside it for the dialogue
                var w = self.props.create_icon(PROPS_LABEL, y - 44, right - 56, y,
                                               1 | 4 | 16 | 32 | (15 << 12) | (7 << 24),
                                               rest, "Pptr_write;Kta", field_size(rest))
                self.add_prop(w, name, 0, rest, String(""))
                var b = self.props.button(right - 48, y - 48, right, y, "...")
                self.add_prop(b, name, 4, rest, String(""))
                self.props_rows.append(w)
            elif cut == eq:
                # a writable field whose Return comes to the program (K without
                # r: Tab and the arrows still move between the fields)
                var w = self.props.create_icon(PROPS_LABEL, y - 44, right, y,
                                               1 | 4 | 16 | 32 | (15 << 12) | (7 << 24),
                                               rest, "Pptr_write;Kta", field_size(rest))
                self.add_prop(w, name, 0, rest, String(""))
                self.props_rows.append(w)
                if wants_caret:
                    focus = w
                    focus_at = rest.byte_length()
            else:
                var v = self.props.create_icon(PROPS_LABEL, y - 44, right, y,
                                               1 | (1 << 4) | (1 << 8) | (7 << 24),
                                               rest, "", 64)
                self.props_icons.append(v)
                self.props_rows.append(v)
            y -= 52
        self.props.force_redraw(0, -2400, PROPS_W, 0)
        if focus >= 0:
            self.props.set_caret(focus, focus_at)

    def prop_update(mut self, row: Int, value: String) raises:
        """One row of the pane given its new value, in place (show_props)"""
        var icon = self.props_rows[row]
        var kind = self.props_rkind[row]
        var hit = self.props_rhit[row]
        if icon < 0 or kind < 0:
            return
        var sent = icon == self.props_sent
        if sent:
            self.props_sent = -1            # the engine's one answer to that Return, even
                                            # one that leaves the value as sent; what comes
                                            # after it is typing again, the user's
        if hit >= 0 and hit < len(self.props_values) and self.props_values[hit] == value:
            return
        if kind == 3:
            self.props.set_selected(icon, value == "1")
            return
        if kind == 7:
            self.props.set_pressed(icon, value == "1")
            return
        if kind == 2:
            self.props.set_selected(icon, value == "on")
        elif kind == 1:
            self.props.set_icon_text(icon, value + "  >")
        elif kind == 0 or kind == 6:
            if caret_in(self.props.handle, icon) and not sent:
                return                      # being typed in: the user's, not the engine's
            self.props.set_icon_text(icon, value)
            if kind == 6 and hit + 1 < len(self.props_values):
                self.props_values[hit + 1] = value      # the colour's dialogue button
        elif kind == 5:
            self.props.set_icon_text(icon, value)
        if hit >= 0 and hit < len(self.props_values):
            self.props_values[hit] = value

    def add_prop(mut self, icon: Int, name: String, kind: Int, value: String, choices: String):
        self.props_icons.append(icon)
        self.props_hit.append(icon)
        self.props_names.append(name)
        self.props_kinds.append(kind)
        self.props_values.append(value)
        self.props_choices.append(choices)

    def props_click(mut self, mut desk: Desktop, c: Click) raises:
        for i in range(len(self.props_hit)):
            if self.props_hit[i] != c.icon:
                continue
            var k = self.props_kinds[i]
            if k == 7:
                # a picture: where in it, in thousandths of its width and
                # height from its top left (a map clicked to go there), then
                # the press as a button's
                var bb = self.props.icon_screen_box(c.icon)
                var w = bb[2] - bb[0] - 12
                var h = bb[3] - bb[1] - 12
                if w > 0 and h > 0:
                    var fx = max(0, min(999, (c.x - bb[0] - 6) * 1000 // w))
                    var fy = max(0, min(999, (bb[3] - 6 - c.y) * 1000 // h))
                    self.send("pick " + self.props_names[i] + " " + String(fx) + " " + String(fy))
                self.send("prop " + self.props_names[i])
            elif k == 3:
                self.send("prop " + self.props_names[i])
            elif k == 4:
                # the dialogue opens on the colour shown, if it is one
                var v = self.props_values[i].as_bytes()
                var rgb = 0
                if len(v) == 7 and Int(v[0]) == 35:
                    for j in range(1, 7):
                        rgb = rgb * 16 + hexval(Int(v[j]))
                self.props_colour = self.props_names[i]
                self.choose_colour(c, rgb)
            elif k == 2:
                self.send("prop " + self.props_names[i] + (" off" if self.props_values[i] == "on" else " on"))
            elif k == 1:
                # the choice's menu at the pointer, the one chosen ticked
                self.props_menu = Menu(self.props_names[i], self.props_choices[i])
                var items = self.props_choices[i].split(",")
                for j in range(len(items)):
                    var t = String(items[j])
                    if t.startswith("-"):
                        var u = String(t[byte=1:])
                        t = u^
                    if t == self.props_values[i]:
                        self.props_menu.set_ticked(j, True)
                self.props_menu_row = i
                desk.open_menu(self.props_menu, c.x - 64, c.y)
            return

    def toggle_rulers(mut self, mut desk: Desktop) raises:
        if self.cfg.viewport and not self.cfg.view_rulers:
            return                          # a viewport has no rulers to show
        self.rulers = not self.rulers
        self.view_menu.set_ticked(0, self.rulers)
        self.set_toggle("rs_rulers", self.rulers)
        if self.cfg.viewport:
            # the engine draws them
            self.send("rulers " + ("on" if self.rulers else "off"))
            self.save_choices()
            return
        self.bind()
        self.open_rulers()
        self.save_choices()

    def set_toggle(mut self, name: String, on: Bool) raises:
        for i in range(len(self.tb_names)):
            if self.tb_names[i] == name:
                self.toolbar.set_pressed(self.tb_icons[i], on)

    # ---- the rulers ----

    def page_x(self, mm: Int) -> Int:
        """The work area x coordinate of a distance in mm across the page"""
        return self.mleft + (DESK_MM + mm) * self.ppmm100 * self.ospp // 100

    def page_top(self, n: Int) -> Int:
        """The work area y coordinate (negative) of the top of page n.  The
        pages are one under another with desk between them, each a whole
        number of pixels high, as the engine places them."""
        var desk = (DESK_MM * self.ppmm100 + 50) // 100
        var page = (self.pg_h100 * self.ppmm100 + 9999) // 10000
        return -self.mtop - (desk + n * (page + desk)) * self.ospp

    def redraw(mut self, mut desk: Desktop, r: Redraw) raises:
        var st = self.win.state()
        var ox = st.x0 - st.scroll_x            # where the work area's origin is on the screen
        var oy = st.y1 - st.scroll_y
        var step = max(1, self.ppmm100 * self.ospp // 100)     # one millimetre, in OS units
        # a number every so many millimetres, far enough apart to read
        var every = 10
        for e in [10, 20, 50, 100, 200, 500, 1000]:
            every = e
            if e * step >= 72:
                break
        # and tick marks no closer together than a few pixels
        var minor = 1 if step >= 6 else (5 if step * 5 >= 12 else max(1, every // 5))
        if r.window == self.hruler.handle:
            var top = r.origin_y
            var foot = top - RULER_H
            # from a number's width back: a number is drawn to the right of
            # its tick, so a strip may cut one whose tick is outside it
            var first = (r.clip_x0 - ox - self.page_x(0) - 80) // step - 1
            var last = (r.clip_x1 - ox - self.page_x(0)) // step + 1
            set_colour(7)
            for mm in range(max(first, 0), min(last, self.pg_w_mm) + 1):
                var x = ox + self.page_x(mm)
                var tick = 20 if mm % every == 0 else (12 if mm % (5 * minor) == 0 else 6)
                if mm % minor == 0:
                    vdu.line(x, foot, x, foot + tick)
                if mm % every == 0:
                    vdu.text_at(x + 4, top - 2, String(mm))
            if self.last_x >= 0:
                set_colour(11)
                var px = ox + self.mleft + self.last_x * self.ospp
                vdu.line(px, foot, px, top)
        elif r.window == self.vruler.handle:
            # each page has its own ruler, starting at 0 at its top edge
            var right = r.origin_x + RULER_W
            set_colour(7)
            for n in range(max(self.pages, 1)):
                var ptop = oy + self.page_top(n)
                if ptop - self.pg_h_mm * step > r.clip_y1 or ptop < r.clip_y0:
                    continue                # this page's ruler is outside the area to redraw
                var first = (ptop - r.clip_y1 - 40) // step - 1
                var last = (ptop - r.clip_y0 + 40) // step + 1
                for mm in range(max(first, 0), min(last, self.pg_h_mm) + 1):
                    var y = ptop - mm * self.ppmm100 * self.ospp // 100
                    var tick = 20 if mm % every == 0 else (12 if mm % (5 * minor) == 0 else 6)
                    if mm % minor == 0:
                        vdu.line(right - tick, y, right, y)
                    if mm % (every if every >= 20 else 2 * every) == 0:
                        vdu.text_at(r.origin_x + 2, y - 4, String(mm))
            if self.last_y >= 0:
                set_colour(11)
                var py = oy - self.mtop - self.last_y * self.ospp
                vdu.line(r.origin_x, py, right, py)

    # ---- the engine ----

    def send(self, line: String) raises:
        var r = Regs()
        r[0] = self.session
        r.set_string(1, line)
        swi(ROSApps_Send, r)

    def drain(mut self, mut desk: Desktop) raises:
        """Reads the lines the engine has sent and acts on them.  (Changes
        to the picture are redrawn by the ROSApps module, not here.)"""
        for _ in range(64):
            var r = Regs()
            r[0] = self.session
            swi(ROSApps_Poll, r)
            if r[0] == 0:
                return
            var line = get_string(r[0])
            self.handle_line(desk, line)

    def handle_line(mut self, mut desk: Desktop, line: String) raises:
        """Acts on one line from the engine.  Each line starts with a word
        saying what it is: the document's size, the caret, the title, a
        question to ask, an error, and so on."""
        if line == "ended":
            desk.report("The engine has stopped.")
            desk.quit()
            return
        if line.startswith("hello "):
            self.engine_version = String(line[byte=6:])
            self.infobox.set_icon_text(self.info_engine, self.engine_version)
        elif line.startswith("doc "):
            # the number of pages, a page's size and the whole picture's
            # size, in 1/100 mm
            var w = line.split(" ")
            if len(w) >= 6:
                self.pages = Int(w[1])
                self.pg_w_mm = Int(w[2]) // 100
                self.pg_h_mm = Int(w[3]) // 100
                self.pg_h100 = Int(w[3])
                var resized = self.doc_w100 != Int(w[4]) or self.doc_h100 != Int(w[5])
                self.doc_w100 = Int(w[4])
                self.doc_h100 = Int(w[5])
                if self.fresh and self.cfg.fit:
                    self.fit_zoom()
                self.bind()
                if resized and not self.fresh:
                    # smaller (a picture cropped or made smaller): opened
                    # again where it is, which the Wimp keeps within the
                    # new extent
                    var b = Buffer(40)
                    b.set_word(0, self.win.handle)
                    var r = Regs(0, b.addr())
                    swi(Wimp_GetWindowState, r)
                    if (b.word(32) & (1 << 16)) != 0:
                        # wider (a page turned to landscape): as wide as the
                        # page and its desk, as far as the screen allows
                        var sw = (vdu.mode_variable(11) + 1) << vdu.mode_variable(4)
                        var want_w = max(self.mleft + (self.doc_w100 * self.ppmm100 // 10000) * self.ospp,
                                         self.min_width())
                        var x0 = b.word(4)
                        # leaving room on the right for the properties pane
                        var room = sw - 48 - (PROPS_W if self.props_on else 0)
                        if self.cfg.viewport == False and b.word(12) - x0 < want_w:
                            b.set_word(12, max(b.word(12), min(x0 + want_w, room)))
                        desk._open_block(b.addr())
                if self.fresh:
                    self.size_to_page(desk)            # a new document: size the window
                    self.open_document(desk, True)     # and open it at the top left
                    self.win.set_caret()               # the keys: a document opened is the one typed to
                    self.fresh = False
                    if self.play.byte_length() > 0:
                        # *Present FILE: start the show now the window is open
                        self.send(self.play)
                        self.play = String("")
        elif line.startswith("caret "):
            var w = line.split(" ")
            if len(w) >= 4:
                self.caret_x = Int(w[1])
                self.caret_y = Int(w[2])
                self.caret_h = Int(w[3])
                self.show_caret(desk)
        elif line.startswith("title "):
            self.fresh = True
            self.modified = False
            self.set_title(String(line[byte=6:]))
        elif line.startswith("tab "):
            # the front tab: its title, RISC OS name (or "") and file type
            var f = String(line[byte=4:]).split("|")
            if len(f) >= 3:
                self.doc_title = String(f[0])
                self.doc_path = String(f[1])
                var t = 0
                var bs = String(f[2]).as_bytes()
                for i in range(len(bs)):
                    t = t * 16 + hexval(Int(bs[i]))
                self.tab_type = t
                self.win.set_title(self.doc_title + (" *" if self.modified else ""))
                if self.save_kind < 0:
                    # the save box's icon follows the tab's type
                    self.save_kind = -2
                    self.set_save_kind(-1)
                self.savebox.set_icon_text(self.save_name, self.doc_path if self.doc_path.byte_length() > 0 else self.doc_title)
        elif line.startswith("task "):
            # run a * command in a task window of its own, its output
            # sent back as "taskout N|text" and its end as "taskend N"
            var f = String(line[byte=5:]).split("|", 1)
            if len(f) == 2:
                self.start_taskwindow(desk, Int(String(f[0])), String(f[1]))
        elif line.startswith("tick "):
            # "tick M I S": item I of menu M (2 the View menu's own entries,
            # 3 up the application's menus) ticked (S 1), not (0), or greyed (2)
            var w = line.split(" ")
            if len(w) >= 4:
                var m = Int(String(w[1]))
                var i = Int(String(w[2]))
                var st = Int(String(w[3]))
                if m == 2 and self.cfg.view_items.byte_length() > 0:
                    self.view_menu.set_ticked(VIEW_OWN + i, st == 1)
                    self.view_menu.set_shaded(VIEW_OWN + i, st == 2)
                elif m - 3 >= 0 and m - 3 < len(self.own_menus):
                    self.own_menus[m - 3].set_ticked(i, st == 1)
                    self.own_menus[m - 3].set_shaded(i, st == 2)
        elif line.startswith("menustate"):
            self.apply_menu_state(String(line[byte=9:]))
        elif line.startswith("debug "):
            # a command for the box's debugger (BoxDebug_Command), answered
            # at once with "debugreply" and its text, or "!" and the error
            self.send("debugreply " + self.debug_command(String(line[byte=6:])))
        elif line.startswith("taskstop "):
            self.stop_taskwindow(Int(String(line[byte=9:])))
        elif line.startswith("taskin "):
            # "taskin N|TEXT": a line for the program in task window N to
            # read, as if typed (TaskWindow_Input), Return after it
            var f = String(line[byte=7:]).split("|", 1)
            if len(f) == 2:
                self.input_taskwindow(Int(String(f[0])), String(f[1]))
        elif line.startswith("ask "):
            self.ask(String(line[byte=4:]))
        elif line.startswith("question "):
            self.question(String(line[byte=9:]))
        elif line == "unquestion":
            self.qbox.close()
        elif line.startswith("fullscreen "):
            if line == "fullscreen on":
                self.show_start(desk)
            else:
                self.show_end(desk)
        elif line.startswith("setmenu "):
            # new entries for one of the application's own menus
            var rest = String(line[byte=8:])
            var sp = rest.find(" ")
            if sp > 0:
                var n = Int(String(rest[byte=0:sp]))
                var items = String(rest[byte=sp + 1:])
                var k = n - 3
                if k >= 0 and k < len(self.own_menus) and items.byte_length() > 0:
                    self.own_menus[k] = Menu(self.cfg.menu_names[k], items)
                    self.main_menu.set_submenu(n, self.own_menus[k])
        elif line.startswith("thumbs "):
            # the pages' pictures are written: merged into the Wimp's pool
            # (the same names are replaced), and the pane drawn again
            if self.thumb_ro.byte_length() > 0:
                try:
                    system.cli("IconSprites " + self.thumb_ro)
                except e:
                    pass
                self.props.force_redraw(0, -2400, PROPS_W, 0)
        elif line.startswith("note "):
            desk.report(String(line[byte=5:]))
        elif line.startswith("choose "):
            self.choose(String(line[byte=7:]))
        elif line.startswith("unchoose"):
            self.choosebox.close()
            self.win.set_caret()
        elif line == "hourglass on":
            _ = swi2(0x406C0, 0)            # Hourglass_On: a long piece of work begins
        elif line == "hourglass off":
            _ = swi2(0x406C1, 0)            # Hourglass_Off
        elif line.startswith("propsmore "):
            # the first rows of a pane too long for one line; "props" ends it
            self.props_more += String(line[byte=10:]) + "|"
        elif line.startswith("props"):
            self.show_props(self.props_more + String(String(line[byte=5:]).strip()))
            self.props_more = String("")
        elif line.startswith("options"):
            self.show_options(String(String(line[byte=7:]).strip()))
        elif line.startswith("extent "):
            # a viewport: the work area is made the size of the whole document
            var w = line.split(" ")
            if len(w) >= 3:
                self.win.set_extent(Int(w[1]) * self.ospp, Int(w[2]) * self.ospp + self.mtop)
        elif line.startswith("centre "):
            # a place in the page, in pixels, scrolled to the middle of the view
            var w = line.split(" ")
            if len(w) >= 3:
                var st = self.win.state()
                var vw = st.x1 - st.x0 - self.mleft
                var vh = st.y1 - st.y0 - self.mtop
                desk.scroll_window(self.win, max(0, Int(w[1]) * self.ospp - vw // 2),
                                   -max(0, Int(w[2]) * self.ospp - vh // 2))
                self.send_view()
        elif line.startswith("scrollto "):
            var w = line.split(" ")
            if len(w) >= 3:
                desk.scroll_window(self.win, Int(w[1]) * self.ospp, -Int(w[2]) * self.ospp)
                self.send_view()
        elif line.startswith("zoom "):
            # the engine wants another zoom: a view that needs more room
            # (!Draft's source and page side by side) or is done with it
            # (not kept in Choices: the zoom chosen there is the user's)
            var z = Int(String(line[byte=5:]))
            if not self.engine_zoomed:
                self.zoom_kept = self.zoom
                self.engine_zoomed = True
            elif z == self.zoom_kept:
                self.engine_zoomed = False      # back to the user's own
            self.zoom = max(5, min(400, z))
            self.ppmm100 = ACTUAL * self.zoom // 100
            self.bind()
            self.open_document(desk)
        elif line.startswith("cell "):
            # the current cell's reference and contents, for the formula bar
            var f = String(line[byte=5:]).split("|")
            if len(f) >= 2 and self.fbar_ref >= 0:
                self.fbar.set_icon_text(self.fbar_ref, String(f[0]))
                var rest = String(line[byte=5 + String(f[0]).byte_length() + 1:])
                self.fbar.set_icon_text(self.fbar_input, rest)
        elif line.startswith("type "):
            # typing began in a cell, so the formula bar takes over (F2 edits
            # the cell's current contents)
            if self.fbar_input >= 0:
                var t = String(line[byte=5:])
                if t.byte_length() > 0:
                    self.fbar.set_icon_text(self.fbar_input, t)
                self.fbar.set_caret(self.fbar_input, self.fbar.icon_text(self.fbar_input).byte_length())
        elif line.startswith("usetool "):
            # the engine chose a tool (its key was pressed): as a click on
            # the tool in the palette
            var name = String(line[byte=8:])
            for k in range(len(self.tools)):
                if self.tools[k] == name and k != self.tool:
                    self.palette.set_selected(self.tool, False)
                    self.tool = k
                    self.palette.set_selected(k, True)
                    self.send("tool " + name)
        elif line.startswith("fg "):
            # the engine changed the colour (with the eyedropper): RRGGBB
            var hx = String(line[byte=3:])
            var v = 0
            var bs = hx.as_bytes()
            for i in range(min(len(bs), 6)):
                v = v * 16 + hexval(Int(bs[i]))
            self.colour = v
            self.show_colour()
        elif line == "claim":
            self.claim_clipboard(desk)
        elif line == "wantpaste":
            self.paste(desk)
        elif line.startswith("modified "):
            self.modified = line == "modified 1"
            var title = self.doc_title.copy()
            self.set_title(title)
        elif line.startswith("inspect "):
            var f = String(line[byte=8:]).split("|")
            if len(f) >= 4:
                self.inspector.set_icon_text(self.page_label, String(f[0]))
                self.inspector.set_icon_text(self.sel_label, String(f[1]))
                self.inspector.set_icon_text(self.x_field, String(f[2]))
                self.inspector.set_icon_text(self.y_field, String(f[3]))
                if self.w_field >= 0:
                    self.inspector.set_icon_text(self.w_field, String(f[4]) if len(f) >= 6 else String(""))
                    self.inspector.set_icon_text(self.h_field, String(f[5]) if len(f) >= 6 else String(""))
        elif line.startswith("error "):
            desk.report(String(line[byte=6:]))

    def pollword_non_zero(mut self, mut desk: Desktop) raises:
        self.drain(desk)

    def end_stroke(mut self) raises:
        """A stroke ended where the pointer was last seen held: when a
        click comes (the button has gone up and down again since the last
        null event, so it was never seen up), or the pointer is found over
        another window"""
        if self.selecting:
            self.selecting = False
            self.send("release " + String(self.sel_x) + " " + String(self.sel_y) + " " + String(self.press_mods))

    def null_event(mut self, mut desk: Desktop) raises:
        # where the pointer is on the page, in page pixels (y downwards)
        var p = Buffer(20)
        var r = Regs()
        r[1] = p.addr()
        _ = swi_x(Wimp_GetPointerInfo, r)
        if self.panning:
            if (p.word(8) & 5) == 0:
                self.panning = False
            else:
                desk.scroll_window(self.win, self.pan_x - p.word(0), self.pan_y - p.word(4))
        if self.selecting and p.word(12) != self.win.handle:
            # held over another window -- the Colour button, a dialogue: the
            # press there is a new one, the stroke's release never seen
            self.end_stroke()
        if self.selecting:
            # a button was pressed on the page: the engine is told where the
            # pointer moves while it is held, then when it is released
            var ws = self.win.state()
            var sx = (p.word(0) - ws.x0 + ws.scroll_x - self.mleft) // self.ospp
            var sy = (ws.y1 - p.word(4) - ws.scroll_y - self.mtop) // self.ospp
            if self.cfg.viewport:           # a viewport's pixels do not include the scroll
                sx = (p.word(0) - ws.x0 - self.mleft) // self.ospp
                sy = (ws.y1 - p.word(4) - self.mtop) // self.ospp
            if (p.word(8) & 5) == 0:
                # released: use where the pointer was last seen while the
                # button was held, not where it is now.  The release is only
                # noticed at the next null event, and by then the pointer
                # may have moved on (to the inspector below the page, say),
                # which would draw a line out to it.
                self.selecting = False
                self.send("release " + String(self.sel_x) + " " + String(self.sel_y) + " " + String(self.press_mods))
            elif sx != self.sel_x or sy != self.sel_y:
                self.sel_x = sx
                self.sel_y = sy
                self.send("drag " + String(sx) + " " + String(sy) + " " + String(self.press_mods))
        # the palette's tool under the pointer, for the engine to say what
        # it is ("hover NAME", "hover -" when it leaves)
        var ht = p.word(16) if p.word(12) == self.palette.handle else -1
        if ht >= len(self.tools):
            ht = -1
        if ht != self.hover_tool:
            self.hover_tool = ht
            self.send("hover " + (self.tools[ht] if ht >= 0 else String("-")))
        var x = -1
        var y = -1
        if p.word(12) == self.win.handle:
            var s = Buffer(36)
            s.set_word(0, self.win.handle)
            var q = Regs()
            q[1] = s.addr()
            swi(Wimp_GetWindowState, q)
            var st = WindowState(s.addr())
            x = (p.word(0) - st.x0 + st.scroll_x - self.mleft) // self.ospp
            y = (st.y1 - p.word(4) - st.scroll_y - self.mtop) // self.ospp
            if self.cfg.viewport:
                x = (p.word(0) - st.x0 - self.mleft) // self.ospp
                y = (st.y1 - p.word(4) - self.mtop) // self.ospp
            if x < 0 or y < 0 or x >= self.page_w or y >= self.page_h:
                x = -1
                y = -1
        if x != self.last_x or y != self.last_y:
            self.last_x = x
            self.last_y = y
            self.send("pointer " + String(x) + " " + String(y))
            if self.rulers and not self.cfg.viewport:
                # the whole of both: a narrower part, which would be less to
                # draw, came back cleared without the ticks
                self.hruler.redraw_all()
                self.vruler.redraw_all()
        var ds = self.win.state()
        if self.props_on and ds.is_open():
            # the pane has no scroll bar: the engine is told how tall it is
            # on the screen ("room H", OS units), and keeps its rows to that
            var ps = self.props.state()
            var room = ps.y1 - ps.y0
            if ps.is_open() and room != self.props_room:
                self.props_room = room
                self.send("room " + String(room))
        if self.cfg.viewport and self.view_w > 0 and ds.is_open():
            # if the window was resized, bind a picture of the new size; if
            # it was scrolled, tell the engine where the view is now
            var vw = max(16, (ds.x1 - ds.x0 - self.mleft) // self.ospp)
            var vh = max(16, (ds.y1 - ds.y0 - self.mtop) // self.ospp)
            if vw != self.view_w or vh != self.view_h:
                self.bind_view()
            elif ds.scroll_x != self.scroll_x or ds.scroll_y != self.scroll_y:
                self.send_view()
        if ds.scroll_x != self.scroll_x or ds.scroll_y != self.scroll_y:
            self.scroll_x = ds.scroll_x         # the page scrolled, so the rulers follow it
            self.scroll_y = ds.scroll_y
            if not self.cfg.viewport and self.page_w > 0:
                self.send_view()                # where the view is, for Page Up and Down
            if self.rulers and not self.cfg.viewport:
                self.hruler.redraw_all()
                self.vruler.redraw_all()
        self.drain(desk)

    # ---- the user ----

    def click(mut self, mut desk: Desktop, c: Click) raises:
        if c.window == self.showwin.handle:
            # a click during a show: SELECT goes on, ADJUST goes back, MENU ends it
            if c.is_menu():
                self.send("key 27 0")
            else:
                self.send("key " + ("32" if (c.buttons & 0x404) != 0 else "8") + " 0")
            return
        if c.window == self.win.handle and c.is_menu():
            self.set_save_kind(-1)          # Save as... in the menu saves the document itself
            if self.cfg.menu_context:
                self.ask_menu_state(desk, c)
            desk.open_menu(self.main_menu, c.x - 64, c.y)
        elif c.window == self.inspector.handle and c.icon == self.colour_button and c.icon >= 0:
            self.props_colour = String("")
            self.choose_colour(c, self.colour)
        elif c.window == self.inspector.handle and c.icon >= 0 and self.option_click(c.icon):
            pass
        elif c.window == self.props.handle and c.icon >= 0:
            self.props_click(desk, c)
        elif c.window == self.choosebox.handle and c.icon >= 0:
            self.choose_click(c.icon)
        elif c.window == self.qbox.handle and c.icon >= 0:
            for i in range(len(self.q_acts)):
                if self.q_acts[i] == c.icon:
                    self.question_answer(i)
                    break
        elif c.window == self.askbox.handle:
            if c.icon == self.ask_ok and c.icon >= 0:
                self.answer()
            elif c.icon == self.ask_cancel and c.icon >= 0:
                self.askbox.close()
                self.win.set_caret()
        elif c.window == self.savebox.handle:
            if c.icon == self.save_icon and c.is_drag():
                desk.start_save_drag(self.savebox, self.save_icon, self.save_type_now(),
                                     leaf_of(self.savebox.icon_text(self.save_name)))
            elif c.icon == self.save_ok:
                self.save_typed(desk)
        elif c.window == self.win.handle and (c.buttons & 0x505) != 0:
            # the pointer on the page, which every engine handles the same
            # way: a press (SELECT, or ADJUST as modifier 8), then the drag
            # while it is held and the release (both in null_event), or a
            # double click.  The window's button type is double/click/drag,
            # so a click is reported as the button goes down, as the
            # buttons x 256.
            var x = (c.work_x - self.mleft) // self.ospp
            var y = (-c.work_y - self.mtop) // self.ospp
            if self.cfg.viewport:
                var vs = self.win.state()
                x = (c.work_x - vs.scroll_x - self.mleft) // self.ospp
                y = (vs.scroll_y - c.work_y - self.mtop) // self.ospp
            var mods = modifiers() | (8 if (c.buttons & 0x101) != 0 else 0)
            self.win.set_caret()        # take the input focus; an engine with a caret says where
            var tool = self.tools[self.tool] if self.tool < len(self.tools) else String("")
            if tool == "rs_pan" and (c.buttons & 0x500) != 0:
                # the hand tool: the page follows the pointer while it is held
                var st = self.win.state()
                self.panning = True
                self.pan_x = c.x + st.scroll_x
                self.pan_y = c.y + st.scroll_y
            elif tool == "rs_zoomtool" and (c.buttons & 0x500) != 0:
                # the magnifier: zoom in with SELECT and out with ADJUST,
                # keeping the place clicked under the pointer
                self.zoom_at(desk, c, (c.buttons & 0x100) == 0)
            elif (c.buttons & 0x500) != 0:
                self.selecting = True
                self.press_mods = mods
                self.sel_x = x
                self.sel_y = y
                self.send("press " + String(x) + " " + String(y) + " " + String(mods))
            else:
                self.send("dclick " + String(x) + " " + String(y) + " " + String(mods))
        elif c.window == self.toolbar.handle and c.icon >= 0:
            var name = String("")
            for i in range(len(self.tb_icons)):
                if self.tb_icons[i] == c.icon:
                    name = self.tb_names[i]         # a copy, as the action changes self
            if name.byte_length() > 0:
                self.toolbar_action(desk, name, c)
        elif c.window == self.palette.handle and c.icon >= 0 and c.icon < len(self.tools):
            self.tool = c.icon
            self.palette.set_selected(c.icon, True)     # a radio button: it stays selected
            self.send("tool " + self.tools[c.icon])

    def show_caret(mut self, mut desk: Desktop) raises:
        """Puts the Wimp's caret where the engine says, and scrolls the
        window to show it.  If the engine shows a selection instead, the
        window keeps the input focus with no caret drawn; a negative height
        is a selection's end the engine wants in view (a word found)."""
        if self.caret_h == 0:
            self.win.set_caret()
            return
        var ch = self.caret_h if self.caret_h > 0 else -self.caret_h
        var cx = self.mleft + self.caret_x * self.ospp
        var top = -self.mtop - self.caret_y * self.ospp
        var foot = top - ch * self.ospp
        if self.caret_h > 0:
            self.win.place_caret(cx, foot, ch * self.ospp)
        else:
            self.win.set_caret()
        # scroll so that the caret is in view, below the toolbar and rulers,
        # with a little room round it
        var st = self.win.state()
        var w = st.x1 - st.x0
        var h = st.y1 - st.y0
        var sx = st.scroll_x
        var sy = st.scroll_y
        var room = 48
        if top > sy - self.mtop - room:
            sy = top + self.mtop + room
        elif foot < sy - h + room:
            sy = foot + h - room
        if cx < sx + self.mleft + room:
            sx = cx - self.mleft - room
        elif cx > sx + w - room:
            sx = cx - w + room
        sx = max(sx, 0)
        sy = min(sy, 0)
        if sx != st.scroll_x or sy != st.scroll_y:
            desk.scroll_window(self.win, sx, sy)

    def save_typed(mut self, mut desk: Desktop) raises:
        var name = self.savebox.icon_text(self.save_name)
        var full = False
        for i in range(name.byte_length()):
            var ch = name[byte=i:i+1]
            if ch == "." or ch == ":":
                full = True
        if not full:
            desk.report("To save, drag the icon to a directory display.")
            return
        _ = self.save_to(desk, name)

    def key(mut self, mut desk: Desktop, window: Int, icon: Int, code: Int) raises -> Bool:
        """Keys in the document go to the engine, except the function keys
        and Escape, which are passed on to other programs.  F3 and Ctrl-S
        save."""
        if self.cfg.formula_bar and window == self.fbar.handle:
            if code == 13 or code == 0x18A:
                # Return: put the text in the current cell, and go back to the sheet
                self.send("set " + self.fbar.icon_text(self.fbar_input))
                self.win.set_caret()
                return True
            if code == 27:
                self.send("key 27 0")
                self.win.set_caret()
                return True
            return False
        if self.showing:
            # during a show every key goes to it, Escape included, whichever
            # window has the caret (starting the show from a menu can leave
            # the caret elsewhere)
            self.send("key " + String(code) + " " + String(modifiers()))
            if window != self.showwin.handle:
                self.showwin.set_caret()
            return True
        if window == self.inspector.handle:
            # Return in the place and size fields: "geom X Y W H", in mm as
            # typed, for the engine to move or size what is selected
            if code == 13 and icon >= 0 and self.w_field >= 0 and (icon == self.x_field or icon == self.y_field
                                                                   or icon == self.w_field or icon == self.h_field):
                self.send("geom " + self.inspector.icon_text(self.x_field) + " " + self.inspector.icon_text(self.y_field)
                          + " " + self.inspector.icon_text(self.w_field) + " " + self.inspector.icon_text(self.h_field))
                self.win.set_caret()
                return True
            # Return in an option's field sends the number to the engine
            for i in range(len(self.opt_fields)):
                if icon == self.opt_fields[i] and code == 13:
                    self.send("option " + self.opt_names[i] + " " + self.inspector.icon_text(icon))
                    self.win.set_caret()
                    return True
            return False
        if window == self.props.handle:
            # Return in a property's field sends its value to the engine
            if code == 13:
                for i in range(len(self.props_hit)):
                    if self.props_hit[i] == icon and self.props_kinds[i] == 0:
                        # what the field holds now, so that the engine's
                        # answer (a value it changed, or a field it
                        # emptied, as !Code's Input) is shown in it
                        var text = self.props.icon_text(icon)
                        self.props_values[i] = text
                        self.props_sent = icon
                        self.send("prop " + self.props_names[i] + " " + text)
                        return True
            return False
        if window == self.qbox.handle:
            if code == 13:
                self.question_answer(0)
                return True
            if code == 27:
                self.question_answer(len(self.q_acts) - 1)
                return True
            return False
        if window == self.choosebox.handle:
            if code == 13 and len(self.choose_acts) > 0:
                self.choose_click(self.choose_acts[0])
                return True
            if code == 27:
                self.send("answer " + self.choose_id + " Close|")
                self.choosebox.close()
                self.win.set_caret()
                return True
            return False
        if window == self.askbox.handle:
            if code == 13:
                self.answer()
                return True
            if code == 27:
                self.askbox.close()
                self.win.set_caret()
                return True
            return False
        if window == self.savebox.handle:
            if code == 13:
                self.save_typed(desk)
                return True
            if code == 27:
                desk.close_menu()
                self.savebox.close()
                self.close_after_save = False
                return True
            return False
        if window != self.win.handle:
            return False
        if self.cfg.zoom_keys and (modifiers() & 2) != 0:
            # Ctrl with the keypad's + and -, Ctrl-- (which the keyboard gives
            # as &1F) and Ctrl-0 (0): closer, further, and the page whole.
            # Ctrl with = and the other digits give nothing of their own.
            if code == 0x2B:
                self.set_zoom(desk, self.zoom * 3 // 2 if self.zoom >= 100 else self.zoom + 25)
                return True
            if code == 0x2D or code == 0x1F:
                self.set_zoom(desk, self.zoom * 2 // 3 if self.zoom > 100 else self.zoom - 25)
                return True
            if code == 0:
                self.set_zoom(desk, self.page_zoom())
                return True
        if code == 16 and not self.cfg.dev_keys:     # Ctrl-P: print
            self.print_doc(desk)
            return True
        if self.cfg.save_type != 0 and ((code == 0x183 and not self.cfg.dev_keys) or code == 19):
            self.set_save_kind(-1)
            if code == 0x183:
                self.open_savebox(desk)         # F3 always opens the save box
            else:
                self.save_doc(desk)
            return True
        if code == 27:
            # Escape goes to the engine (to end a path being drawn, or let
            # go of a drag) and is passed on as well, as usual
            self.send("key 27 " + String(modifiers()))
            return False
        if (code >= 0x1CC and (code & 0xF) == 0xC) or code == 0x180:
            return False                    # F12 and Print are passed on
        self.send("key " + String(code) + " " + String(modifiers()))
        return True

    def toolbar_action(mut self, mut desk: Desktop, name: String, c: Click) raises:
        if name == "rs_info":
            desk.open_dialogue(self.infobox, c.x - 64, c.y - 8)
        elif name == "rs_new":
            self.want_new = True            # the application opens another window
        elif name == "rs_print":
            self.print_doc(desk)
        elif name == "rs_save" and self.cfg.save_type != 0:
            self.set_save_kind(-1)
            self.save_doc(desk)
        elif name == "rs_rulers":
            self.toggle_rulers(desk)
        elif name == "rs_grid":
            self.grid = not self.grid
            self.view_menu.set_ticked(2, self.grid)
            self.set_toggle("rs_grid", self.grid)
            self.send("grid " + ("on" if self.grid else "off"))
            self.save_choices()
        elif name == "rs_guides":
            self.guides = not self.guides
            self.view_menu.set_ticked(1, self.guides)
            self.set_toggle("rs_guides", self.guides)
            self.send("guides " + ("on" if self.guides else "off"))
            self.save_choices()
        elif name == "rs_props":
            self.toggle_props(desk)
        elif name == "rs_zoomin":
            self.set_zoom(desk, self.zoom * 3 // 2 if self.zoom >= 100 else self.zoom + 25)
        elif name == "rs_zoomout":
            self.set_zoom(desk, self.zoom * 2 // 3 if self.zoom > 100 else self.zoom - 25)
        elif name == "rs_zoom100":
            self.set_zoom(desk, 100)
        elif name == "rs_big":
            self.send(self.cfg.big_line)
        else:
            self.send("action " + name)     # anything else is for the engine to do

    def menu_selection(mut self, mut desk: Desktop, menu: Int, item: Int, sub_item: Int) raises:
        if menu == self.props_menu.id():
            # a choice made in a property's menu
            var row = self.props_menu_row
            if row >= 0 and row < len(self.props_names) and item >= 0:
                var items = self.props_choices[row].split(",")
                if item < len(items):
                    var t = String(items[item])
                    if t.startswith("-"):
                        var u = String(t[byte=1:])  # the gap above it is not part of its name
                        t = u^
                    self.send("prop " + self.props_names[row] + " " + t)
            return
        if menu == self.main_menu.id():
            if item == 2 and sub_item == 0:
                self.toggle_rulers(desk)
            elif item == 2 and sub_item == 1:
                var c = Click(0, 0, 0, -1, -1, 0, 0)
                self.toolbar_action(desk, "rs_guides", c)
            elif item == 2 and sub_item == 2:
                var c = Click(0, 0, 0, -1, -1, 0, 0)
                self.toolbar_action(desk, "rs_grid", c)
            elif item == 2 and sub_item == 3:
                var c = Click(0, 0, 0, -1, -1, 0, 0)
                self.toolbar_action(desk, "rs_zoomin", c)
            elif item == 2 and sub_item == 4:
                var c = Click(0, 0, 0, -1, -1, 0, 0)
                self.toolbar_action(desk, "rs_zoomout", c)
            elif item == 2 and sub_item == 5:
                self.set_zoom(desk, 100)
            elif item == 2 and sub_item == 6:
                self.toggle_props(desk)
            elif item == 2 and sub_item >= VIEW_OWN:
                self.send("menu 2 " + String(sub_item - VIEW_OWN))
            elif item == 0 and sub_item == 0 and self.cfg.save_type != 0:
                self.set_save_kind(-1)
                self.save_doc(desk)
            elif item == 0 and sub_item == 2:
                self.print_doc(desk)
            elif item == 0 and sub_item >= 3 and sub_item - 3 < len(self.cfg.export_names):
                self.set_save_kind(sub_item - 3)
                self.open_savebox(desk)
            elif item == 0 or item == 1 or (item >= 3 and sub_item >= 0):
                if item >= 3 and self.modified:
                    for k in range(len(self.cfg.replacing_menus)):
                        if self.cfg.replacing_menus[k] == item:
                            if not confirm(self.cfg.name, "The document has changes that are not saved.  Discard them?"):
                                return
                            break
                self.send("menu " + String(item) + " " + String(sub_item))

    def opens(mut self, file_type: Int) -> Bool:
        """Whether it opens a double-clicked file: only its own document
        types, never text, which belongs to Edit"""
        for i in range(len(self.cfg.types)):
            if self.cfg.types[i] == file_type:
                return True
        return False

    def accepts(mut self, file_type: Int) -> Bool:
        if file_type == TYPE_TEXT and self.cfg.pastes_text:
            return True
        if self.cfg.imports_any:
            return True
        for i in range(len(self.cfg.imports)):
            if self.cfg.imports[i] == file_type:
                return True
        for i in range(len(self.cfg.types)):
            if self.cfg.types[i] == file_type:
                return True
        return False

    def load_file(mut self, mut desk: Desktop, path: String, file_type: Int,
                  window: Int) raises:
        """A document dropped on the window or the icon, or double-clicked,
        is opened by the engine, using its Linux name.  Text pasted or
        dropped on the document goes in at the caret instead."""
        if file_type == TYPE_TEXT and self.cfg.pastes_text and window != -1 and window != -2:
            self.paste_file(path)
            return
        var buf = Buffer(1024)
        var r = Regs()
        r.set_string(0, path)
        r[1] = buf.addr()
        r[2] = 1024
        swi(ROSApps_LinuxPath, r)
        if self.cfg.places and window != -1 and window != -2 and not (self.cfg.imports_any and self.opens(file_type)):
            # placed in the document where it was dropped: the pointer is
            # still there, in the view's pixels as a click's
            var at = String("")
            if self.cfg.viewport and window == self.win.handle:
                var pb = Buffer(20)
                var pr = Regs(0, pb.addr())
                if swi_x(0x400CF, pr) == 0:         # Wimp_GetPointerInfo
                    var vs = self.win.state()
                    var vx = (pb.word(0) - vs.x0 - self.mleft) // self.ospp
                    var vy = (vs.y1 - pb.word(4) - self.mtop) // self.ospp
                    at = String(vx) + " " + String(vy) + " "
            self.send("drop " + at + get_string(buf.addr()))
            return
        if self.cfg.tabs:
            # a new tab: the engine says what is in front with "tab"
            self.send("open " + get_string(buf.addr()) + "|" + path + "|" + hex3(file_type))
            self.holds_file = True
            if not self.starting:
                self.open_document(desk)
            return
        self.send("open " + get_string(buf.addr()))
        self.holds_file = True
        # it is saved back to the same file only if it is of the type saved
        self.doc_path = path if file_type == self.cfg.save_type else String("")
        if self.doc_path.byte_length() > 0:
            self.savebox.set_icon_text(self.save_name, path)
        if not self.starting:
            self.open_document(desk)        # otherwise from the poll loop, once the page size is known

    def help(mut self, mut desk: Desktop, window: Int, icon: Int) raises -> String:
        if window == self.toolbar.handle:
            for i in range(len(self.tb_icons)):
                if self.tb_icons[i] == icon:
                    if self.tb_names[i] == "rs_big":
                        return self.cfg.big_help
                    return help_for(self.tb_names[i])
            return "This is the toolbar."
        if window == self.palette.handle:
            if icon >= 0 and icon < len(self.cfg.tool_help) and self.cfg.tool_help[icon].byte_length() > 0:
                return self.cfg.tool_help[icon]
            if icon >= 0 and icon < len(self.tools):
                return help_for(self.tools[icon])
            return "This is the tool palette: choose a tool."
        if window == self.inspector.handle:
            return "This is the inspector: what is selected, and where on the page, in millimetres."
        if window == self.hruler.handle or window == self.vruler.handle:
            return "This ruler is in millimetres; the red mark is the pointer."
        if window == self.win.handle:
            return "This is the document.|MClick MENU for the menu."
        if window == self.props.handle:
            for i in range(len(self.props_hit)):
                if self.props_hit[i] == icon:
                    var k = self.props_kinds[i]
                    if k == 0:
                        return "Type a new " + self.props_names[i] + " and press Return."
                    if k == 1:
                        return "Click SELECT to choose the " + self.props_names[i] + "."
                    if k == 2:
                        return "Click SELECT to turn " + self.props_names[i] + " on or off."
                    if k == 4:
                        return "Click SELECT to choose the " + self.props_names[i] + " colour."
                    return "Click SELECT to " + self.props_names[i] + "."
            return "This is the properties pane: the settings of what is selected."
        if window == self.infobox.handle:
            return "This window shows the program's name, purpose, author and version, and its engine's."
        return ""

    # ---- the debugger ----

    def debug_command(self, command: String) raises -> String:
        """Runs a BoxDebug command (BoxDebug_Command, &C0240) and gives
        its text, each newline as \\n, or "!" and the error"""
        var buf = Buffer(9000)
        var r = Regs()
        r.set_string(0, command)
        r[1] = buf.addr()
        r[2] = 9000
        var err = swi_x(0xC0240, r)
        if err != 0:
            return String("!") + get_string(err + 4)
        var n = r[0]
        var out = String("")
        for i in range(n):
            var c = buf.byte(i)
            if c == 10:
                out += "\\n"
            elif c == 92:
                out += "\\\\"
            elif c >= 32 and c < 127:
                out += chr(c)
        return out

    # ---- the context menu ----

    def ask_menu_state(mut self, mut desk: Desktop, c: Click) raises:
        """Tells the engine where MENU was clicked, and waits a moment
        (a fifth of a second at most) for it to say which items to shade,
        so that the menu opens as it should be for that place."""
        var vs = self.win.state()
        var x = (c.work_x - vs.scroll_x - self.mleft) // self.ospp
        var y = (vs.scroll_y - c.work_y - self.mtop) // self.ospp
        self.menu_answered = False
        self.send("menuat " + String(x) + " " + String(y))
        var t = Regs()
        swi(0x42, t)                        # OS_ReadMonotonicTime
        var until = t[0] + 20
        while not self.menu_answered:
            self.drain(desk)
            var now = Regs()
            swi(0x42, now)
            if now[0] - until >= 0:
                break

    def apply_menu_state(mut self, items: String) raises:
        """Shades the items "M.I ..." of the engine's own menus (3 is the
        first), after unshading those shaded last time"""
        self.menu_answered = True
        for i in range(len(self.menu_shaded)):
            var m = self.menu_shaded[i] >> 8
            if m - 3 >= 0 and m - 3 < len(self.own_menus):
                self.own_menus[m - 3].set_shaded(self.menu_shaded[i] & 255, False)
        self.menu_shaded = List[Int]()
        var words = items.split(" ")
        for k in range(len(words)):
            var w = String(words[k])
            var parts = w.split(".")
            if len(parts) != 2:
                continue
            var m = Int(String(parts[0]))
            var i = Int(String(parts[1]))
            if m - 3 >= 0 and m - 3 < len(self.own_menus):
                self.own_menus[m - 3].set_shaded(i, True)
                self.menu_shaded.append(m * 256 + i)

    # ---- task windows: the engine's commands, run as !Edit runs them ----

    def start_taskwindow(mut self, mut desk: Desktop, id: Int, command: String) raises:
        """Starts a task window running a * command, with this task as
        its parent: what it prints comes as TaskWindow_Output, and it ends
        with Morio (the TaskWindow protocol, as StrongED and Edit use it).
        Its text handle is the engine's session and the engine's number
        for it, so that the messages find their document."""
        var txt = (self.session & 0xFFFFF) * 256 + (id & 255)
        self.tw_ids.append(id)
        self.tw_txt.append(txt)
        self.tw_child.append(0)
        var cmd = (String("TaskWindow \"") + command + "\" -name \"" + self.cfg.name
                   + "\" -quit -task &" + hex8(desk.task) + " -txt &" + hex8(txt))
        try:
            start_task(cmd)
        except e:
            self.send("taskout " + String(id) + "|" + String(e) + "\\n")
            self.task_ended(len(self.tw_ids) - 1)

    def stop_taskwindow(mut self, id: Int) raises:
        """Tells a task window to stop (TaskWindow_Morite)"""
        for i in range(len(self.tw_ids)):
            if self.tw_ids[i] == id and self.tw_child[i] != 0:
                var b = Buffer(20)
                b.set_word(0, 20)
                b.set_word(12, 0)
                b.set_word(16, MSG_TW_MORITE)
                var r = Regs(17, b.addr(), self.tw_child[i])
                _ = swi_x(Wimp_SendMessage, r)

    def input_taskwindow(mut self, id: Int, text: String) raises:
        """Gives a task window's program a line to read (TaskWindow_Input),
        ended by a Return as the keyboard ends one"""
        for i in range(len(self.tw_ids)):
            if self.tw_ids[i] == id and self.tw_child[i] != 0:
                var t = String(text[byte=0:min(text.byte_length(), 200)]) + "\r"
                var b = Buffer(240)
                b.set_word(0, (24 + t.byte_length() + 4) & ~3)
                b.set_word(12, 0)
                b.set_word(16, MSG_TW_INPUT)
                b.set_word(20, t.byte_length())
                b.set_string(24, t)
                var r = Regs(17, b.addr(), self.tw_child[i])
                _ = swi_x(Wimp_SendMessage, r)

    def task_ended(mut self, i: Int) raises:
        self.send("taskend " + String(self.tw_ids[i]))
        _ = self.tw_ids.pop(i)
        _ = self.tw_txt.pop(i)
        _ = self.tw_child.pop(i)

    def task_message(mut self, mut desk: Desktop, number: Int, block: Int) raises -> Bool:
        """A TaskWindow message for one of this document's task windows:
        True if it was"""
        var sender = peek(block + 4)
        if number == MSG_TW_EGO:
            var txt = peek(block + 20)
            for i in range(len(self.tw_txt)):
                if self.tw_txt[i] == txt:
                    self.tw_child[i] = sender
                    return True
            return False
        for i in range(len(self.tw_child)):
            if self.tw_child[i] != sender or sender == 0:
                continue
            if number == MSG_TW_OUTPUT:
                var n = peek(block + 20)
                var piece = String("")
                for k in range(n):
                    var c = peek_byte(block + 24 + k)
                    if c == 92:
                        piece += "\\\\"
                    elif c == 10:
                        piece += "\\n"
                    elif c == 9:
                        piece += "\\t"
                    elif c >= 32 and c < 127:
                        piece += chr(c)
                    elif c >= 160:
                        piece += "\\x" + hex2(c)
                if piece.byte_length() > 0:
                    self.send("taskout " + String(self.tw_ids[i]) + "|" + piece)
            elif number == MSG_TW_MORIO:
                self.task_ended(i)
            return True
        return False

    def close_engine(mut self) raises:
        if self.session != 0:
            var r = Regs()
            r[0] = self.session
            _ = swi_x(ROSApps_Close, r)
            self.session = 0

    def quit(mut self, mut desk: Desktop) raises:
        self.close_engine()


struct RosApp(WimpApp):
    """The application: its icon bar icon and menu, and its documents, each
    with one window and one engine.  Each Wimp event goes to the document
    whose window it is for.  When a document's window is closed, it is kept
    for the next new document."""

    var cfg: AppConfig
    var docs: List[Doc]
    var bar_menu: Menu
    var docs_menu: Menu
    var focus: Int                      # the document last clicked or typed in
    var menu_doc: Int                   # the document whose menu is open
    var clip_doc: Int                   # the document holding the clipboard, or -1

    def __init__(out self, mut desk: Desktop, cfg: AppConfig) raises:
        self.cfg = cfg.copy()
        _ = iconbar_icon(cfg.icon)
        self.docs = List[Doc]()
        self.bar_menu = Menu(cfg.name, "Info,New,Choices...,Documents,-Quit")
        self.docs_menu = Menu("Documents", "None")
        self.focus = 0
        self.menu_doc = -1
        self.clip_doc = -1
        # a document named on the command line: one double-clicked in the
        # Filer while the program was not running (Alias$@RunType runs !Run
        # with it).  "-play" before it presses the toolbar's big button once
        # the document is open (so *Present FILE starts the show at once).
        var given = String("")
        var play = False
        var a = 1
        if system.arg_count() > 2 and system.arg(1) == "-play":
            play = True
            a = 2
        if system.arg_count() > a:
            given = system.arg(a)
        var gtype = file_type_of(given) if given.byte_length() > 0 else -1
        if gtype >= 0 and self.docs_accepts(gtype):
            self.docs.append(Doc(desk, self.cfg, "", 0))
            self.docs[0].starting = True
            self.docs[0].load_file(desk, given, gtype, -1)
            self.docs[0].starting = False
            if play:
                self.docs[0].play = self.cfg.big_line.copy()
        else:
            self.docs.append(Doc(desk, self.cfg, self.cfg.start, 0))
            if self.cfg.start.byte_length() == 0:
                self.docs[0].open_document(desk)
        self.bar_menu.set_dialogue(0, self.docs[0].infobox)
        self.bar_menu.set_submenu(3, self.docs_menu)

    def docs_accepts(self, file_type: Int) -> Bool:
        if file_type == TYPE_TEXT and self.cfg.pastes_text:
            return True
        if self.cfg.imports_any:
            return True
        for i in range(len(self.cfg.imports)):
            if self.cfg.imports[i] == file_type:
                return True
        return self.docs_opens(file_type)

    def docs_opens(self, file_type: Int) -> Bool:
        for i in range(len(self.cfg.types)):
            if self.cfg.types[i] == file_type:
                return True
        return False

    def owner(self, window: Int) -> Int:
        """The document a window belongs to, or -1"""
        if window < 0:
            return -1
        for i in range(len(self.docs)):
            if self.docs[i].in_use and self.docs[i].owns(window):
                return i
        return -1

    def open_count(self) -> Int:
        var n = 0
        for i in range(len(self.docs)):
            if self.docs[i].in_use:
                n += 1
        return n

    def new_doc(mut self, mut desk: Desktop, start: String) raises -> Int:
        """Opens another document window, reusing a closed one if there is
        one.  Each is placed down and to the right of the one before."""
        var cascade = (self.open_count() % 8) * 48
        for i in range(len(self.docs)):
            if not self.docs[i].in_use:
                self.docs[i].reuse(desk, start, cascade)
                self.focus = i
                return i
        self.docs.append(Doc(desk, self.cfg, start, cascade))
        self.focus = len(self.docs) - 1
        return self.focus

    def after(mut self, mut desk: Desktop, i: Int) raises:
        """After an event, does what a document has asked of the
        application: open a new window, or deal with the clipboard, which
        only one document can hold."""
        if i < 0 or i >= len(self.docs):
            return
        if self.docs[i].owns_clip and self.clip_doc != i:
            for k in range(len(self.docs)):
                if k != i:
                    self.docs[k].owns_clip = False
            self.clip_doc = i
        if self.docs[i].want_paste:
            # if another of our documents holds the clipboard, pass its
            # text across through a scrap file; otherwise ask the program
            # that holds it
            self.docs[i].want_paste = False
            var k = self.clip_doc
            if k >= 0 and k != i and k < len(self.docs) and self.docs[k].in_use and self.docs[k].owns_clip:
                var scrap = system.read_var("Wimp$ScrapDir")
                if scrap.byte_length() == 0:
                    scrap = String("<Wimp$ScrapDir>")
                var path = scrap + ".ROSAppsClip"
                if self.docs[k].write_clipboard(desk, path):
                    self.docs[i].paste_file(path)
                    var r = Regs()
                    r[0] = 6
                    r.set_string(1, path)
                    _ = swi_x(0x08, r)          # OS_File 6: delete the scrap file
            else:
                self.docs[i].request_clip()
        if self.docs[i].want_new:
            self.docs[i].want_new = False
            var nl = self.cfg.new_line.copy()
            _ = self.new_doc(desk, nl)
        if self.docs[i].want_close:
            self.docs[i].want_close = False
            self.close_window(desk, self.docs[i].win.handle)

    def show_docs_menu(mut self) raises:
        """Makes the Documents submenu, listing the open windows by title."""
        var names = String("")
        for i in range(len(self.docs)):
            if self.docs[i].in_use:
                if names.byte_length() > 0:
                    names += ","
                var t = self.docs[i].doc_title.copy()
                names += t.replace(",", " ")
        if names.byte_length() == 0:
            names = String("None")
        self.docs_menu = Menu("Documents", names)
        self.bar_menu.set_submenu(3, self.docs_menu)

    # ---- the Wimp's events, to the document they are for ----

    def redraw(mut self, mut desk: Desktop, r: Redraw) raises:
        var i = self.owner(r.window)
        if i >= 0:
            self.docs[i].redraw(desk, r)

    def click(mut self, mut desk: Desktop, c: Click) raises:
        # a click means the button went up since a stroke began: the stroke
        # is over, wherever this click is (it was drawn out to the Colour
        # button when the release fell between two null events)
        # -- but not the Wimp's drag event (the buttons x 16), which comes
        # while the button is still held in the stroke itself
        if (c.buttons & 0x50) == 0 or (c.buttons & ~0x50) != 0:
            for k in range(len(self.docs)):
                if self.docs[k].in_use:
                    self.docs[k].end_stroke()
        if c.on_iconbar():
            if c.is_menu():
                self.show_docs_menu()
                desk.open_iconbar_menu(self.bar_menu, c.x)
            else:
                var nl = self.cfg.new_line.copy()
                _ = self.new_doc(desk, nl)
            return
        var i = self.owner(c.window)
        if i < 0:
            return
        self.focus = i
        self.menu_doc = i                   # a menu opened from any of its windows
        self.docs[i].click(desk, c)
        self.after(desk, i)

    def key(mut self, mut desk: Desktop, window: Int, icon: Int, code: Int) raises -> Bool:
        var i = self.owner(window)
        if i < 0:
            return False
        self.focus = i
        var used = self.docs[i].key(desk, window, icon, code)
        self.after(desk, i)
        return used

    def menu_selection(mut self, mut desk: Desktop, menu: Int, item: Int, sub_item: Int) raises:
        if menu == self.bar_menu.id():
            if item == 1:
                var nl = self.cfg.new_line.copy()
                _ = self.new_doc(desk, nl)
            elif item == 3 and sub_item >= 0:
                # a window chosen from the Documents list comes to the front
                var k = 0
                for i in range(len(self.docs)):
                    if self.docs[i].in_use:
                        if k == sub_item:
                            self.docs[i].open_document(desk)
                            self.focus = i
                        k += 1
            elif item == 4:
                for i in range(len(self.docs)):
                    if self.docs[i].in_use and self.docs[i].modified:
                        if not confirm(self.cfg.name,
                                       "Some documents have changes that are not saved.  Quit and lose them?"):
                            return
                        break
                for i in range(len(self.docs)):
                    self.docs[i].close_engine()
                desk.quit()
            return
        var i = self.menu_doc
        if i >= 0 and i < len(self.docs) and self.docs[i].in_use:
            self.docs[i].menu_selection(desk, menu, item, sub_item)
            self.after(desk, i)

    def close_window(mut self, mut desk: Desktop, window: Int) raises:
        var i = self.owner(window)
        if i < 0:
            desk.close_window(window)
            return
        if window != self.docs[i].win.handle:
            desk.close_window(window)       # a box or a pane: close only that
            return
        if not self.docs[i].close_doc(desk):
            return
        if self.clip_doc == i:
            self.clip_doc = -1
        # Wimp_Poll watches the poll word of an open engine, if there is one
        desk.no_pollword()
        for k in range(len(self.docs)):
            if self.docs[k].in_use:
                desk.use_pollword(self.docs[k].pollword)

    def null_event(mut self, mut desk: Desktop) raises:
        for i in range(len(self.docs)):
            if self.docs[i].in_use:
                self.docs[i].null_event(desk)
                self.after(desk, i)

    def pollword_non_zero(mut self, mut desk: Desktop) raises:
        for i in range(len(self.docs)):
            if self.docs[i].in_use:
                self.docs[i].drain(desk)
                # the pointer looked at here too: an engine that writes many
                # lines a second (a game drawing frames) keeps the poll word
                # set, and the Wimp gives no null events while it is, so a
                # drag's last move and its release were being missed
                if self.docs[i].in_use:
                    self.docs[i].null_event(desk)
                self.after(desk, i)

    def accepts(mut self, file_type: Int) -> Bool:
        return self.docs_accepts(file_type)

    def opens(mut self, file_type: Int) -> Bool:
        """Whether it opens a double-clicked file: only its own document
        types, never text, which belongs to Edit"""
        return self.docs_opens(file_type)

    def load_file(mut self, mut desk: Desktop, path: String, file_type: Int,
                  window: Int) raises:
        """A file dropped on a document goes to that document.  One that is
        double-clicked or dropped on the icon gets a window of its own, or
        reuses the first window if that holds an untouched sample or empty
        document."""
        var i = self.owner(window)
        if i >= 0:
            self.docs[i].load_file(desk, path, file_type, window)
            self.after(desk, i)
            return
        if self.cfg.tabs and self.open_count() > 0:
            # one window, many tabs: the window last used takes it
            var k = self.focus
            if k < 0 or k >= len(self.docs) or not self.docs[k].in_use:
                k = 0
                while k < len(self.docs) and not self.docs[k].in_use:
                    k += 1
            if k < len(self.docs):
                self.docs[k].load_file(desk, path, file_type, -2)
                self.focus = k
                return
        if self.open_count() == 1:
            for k in range(len(self.docs)):
                if (self.docs[k].in_use and not self.docs[k].modified and not self.docs[k].holds_file
                        and self.docs[k].doc_path.byte_length() == 0):
                    self.docs[k].load_file(desk, path, file_type, -1)
                    self.focus = k
                    return
        var n = self.new_doc(desk, "")
        self.docs[n].starting = True
        self.docs[n].load_file(desk, path, file_type, -1)
        self.docs[n].starting = False

    def save_file(mut self, mut desk: Desktop, path: String) raises -> Bool:
        # either the clipboard's text is being sent, or a document saved by a drag
        for i in range(len(self.docs)):
            if self.docs[i].in_use and self.docs[i].clip_out:
                return self.docs[i].save_file(desk, path)
        if self.focus >= 0 and self.focus < len(self.docs) and self.docs[self.focus].in_use:
            return self.docs[self.focus].save_file(desk, path)
        return False

    def help(mut self, mut desk: Desktop, window: Int, icon: Int) raises -> String:
        var i = self.owner(window)
        if i >= 0:
            return self.docs[i].help(desk, window, icon)
        return ""

    def message(mut self, mut desk: Desktop, number: Int, block: Int) raises:
        # clipboard messages go to every document (each knows whether it
        # holds the clipboard); a task window's to the document that
        # started it; others, such as a colour chosen, go to the document
        # in use
        if number == MSG_TW_EGO or number == MSG_TW_OUTPUT or number == MSG_TW_MORIO:
            for i in range(len(self.docs)):
                if self.docs[i].in_use and self.docs[i].task_message(desk, number, block):
                    self.after(desk, i)
                    return
            return
        if number == MSG_CLAIM_ENTITY or number == MSG_DATA_REQUEST:
            for i in range(len(self.docs)):
                if self.docs[i].in_use:
                    self.docs[i].message(desk, number, block)
            return
        if self.focus >= 0 and self.focus < len(self.docs) and self.docs[self.focus].in_use:
            self.docs[self.focus].message(desk, number, block)
            self.after(desk, self.focus)

    def quit(mut self, mut desk: Desktop) raises:
        for i in range(len(self.docs)):
            self.docs[i].close_engine()
