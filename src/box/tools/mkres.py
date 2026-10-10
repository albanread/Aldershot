#!/usr/bin/env python3
"""mkres.py -- Toolbox resource files (Res, &FAE) from Python descriptions.

A library: tools/mktbtest.py describes !TBTest's objects with it.  The file
format is the Toolbox manual's (Appendix A, Resource file formats: the
'RESF' header, each object template's three table offsets, its 36-byte
header, its body, its string and messages tables, its relocations); each
class's body is laid out as RISC OS's own Toolbox modules read it -- their
template structures in ROOL's tboxlib headers (Toolbox/tboxlib/objects,
Apache 2.0), the authority where the manual differs (the Window template's
hide event before its toolbar names; a string set's title; ProgInfo
version 101's URI and event).

A body is a list of fields:

    W(n)          a word
    H(a, b)       two half-words in a word
    B(a, b, c, d) four bytes in a word
    Msg(s)        a pointer into the messages table (text the user sees),
                  or -1 for none (s None)
    Str(s)        a pointer into the strings table, or -1 for none
    Spr()         the application's sprite area (SpriteAreaReference)
    Off(label)    a pointer to a place in the body (ObjectOffset)
    Label(name)   marks that place

An absent string is -1 with its relocation still listed, as ResEd writes
them; the Toolbox leaves -1 alone.
"""
import struct


class W:
    def __init__(self, n): self.n = n


class H:
    def __init__(self, a, b): self.a, self.b = a, b


class B:
    def __init__(self, *b): self.b = (list(b) + [0, 0, 0, 0])[:4]


class Msg:
    def __init__(self, s): self.s = s


class Str:
    def __init__(self, s): self.s = s


class Spr:
    """the client's sprite area (Toolbox_Initialise's), set as it loads"""


class Off:
    def __init__(self, label): self.label = label


class Label:
    def __init__(self, name): self.name = name


STRING_REF, MSG_REF, SPRITE_AREA_REF, OBJECT_OFFSET = 1, 2, 3, 4


class Template:
    def __init__(self, cls, name, version, body, flags=0):
        assert len(name) < 12, name
        self.cls, self.name, self.version, self.body, self.flags = cls, name, version, body, flags

    def build(self):
        """The template: three offsets, header, body, tables, relocations"""
        strings, messages = bytearray(), bytearray()
        words, relocs, labels, fixups = [], [], {}, []

        def table(t, s):
            at = len(t)
            t += s.encode("latin-1") + b"\0"
            return at

        for f in self.body:
            if isinstance(f, Label):
                labels[f.name] = 4 * len(words)
                continue
            off = 4 * len(words)
            if isinstance(f, W):
                words.append(f.n & 0xFFFFFFFF)
            elif isinstance(f, H):
                words.append((f.a & 0xFFFF) | (f.b & 0xFFFF) << 16)
            elif isinstance(f, B):
                words.append(sum((v & 0xFF) << 8 * i for i, v in enumerate(f.b)))
            elif isinstance(f, Msg):
                words.append(0xFFFFFFFF if f.s is None else table(messages, f.s))
                relocs.append((off, MSG_REF))
            elif isinstance(f, Str):
                words.append(0xFFFFFFFF if f.s is None else table(strings, f.s))
                relocs.append((off, STRING_REF))
            elif isinstance(f, Spr):
                words.append(0)
                relocs.append((off, SPRITE_AREA_REF))
            elif isinstance(f, Off):
                words.append(0)
                fixups.append((len(words) - 1, f.label))
                relocs.append((off, OBJECT_OFFSET))
            else:
                raise TypeError(f)
        for i, label in fixups:
            words[i] = labels[label]
        body = struct.pack("<%dI" % len(words), *words)

        def pad(t):
            while len(t) % 4:
                t += b"\0"
            return bytes(t)

        strings, messages = pad(strings), pad(messages)
        header_size = 36
        body_at = 12 + header_size                       # from the template's start
        str_at = body_at + len(body) if strings else -1
        msg_at = body_at + len(body) + len(strings) if messages else -1
        rel_at = body_at + len(body) + len(strings) + len(messages) if relocs else -1
        total = header_size + len(body) + len(strings) + len(messages)
        name = self.name.encode("latin-1").ljust(12, b"\0")
        out = struct.pack("<iii", str_at, msg_at, rel_at)
        out += struct.pack("<III", self.cls, self.flags, self.version) + name
        out += struct.pack("<iii", total, header_size, len(body))
        out += body + strings + messages
        if relocs:
            out += struct.pack("<i", len(relocs))
            for off, kind in relocs:
                out += struct.pack("<ii", off, kind)
        return out


def resfile(templates):
    """A resource file of the templates, version 1.01"""
    out = struct.pack("<4sii", b"RESF", 101, 12)
    for t in templates:
        out += t.build()
    return out


# ---- the object flags (the template header's) ----
CREATE_ON_LOAD, SHOW_ON_CREATE, SHARED, ANCESTOR = 1, 2, 4, 8

# ---- classes (tboxlib objects/*.h, the SWI chunk bases) ----
WINDOW, MENU, ICONBAR = 0x82880, 0x828C0, 0x82900
COLOURMENU, COLOURDBOX = 0x82980, 0x829C0
FONTDBOX, FONTMENU = 0x82A00, 0x82A40
DCS, QUIT, FILEINFO = 0x82A80, 0x82A90, 0x82AC0
PRINTDBOX, PROGINFO, SAVEAS, SCALE = 0x82B00, 0x82B40, 0x82BC0, 0x82C00


# ---- gadgets: the 36-byte header, then the gadget's data ----

def gadget(gtype, size, box, cid, data, flags=0, help=None):
    """size: the gadget's data's bytes (the type word holds header + data)"""
    x0, y0, x1, y1 = box
    return [W(flags), W(((36 + size) << 16) | gtype), W(x0), W(y0), W(x1), W(y1), W(cid),
            Msg(help), W(0 if help is None else len(help) + 1)] + data


def action_button(box, cid, text, flags=0, click_show=None, event=0, help=None):
    return gadget(128, 16, box, cid, [Msg(text), W(len(text) + 1), Str(click_show), W(event)], flags, help)


def option_button(box, cid, label, flags=1, event=0, help=None):
    return gadget(192, 12, box, cid, [Msg(label), W(len(label) + 1), W(event)], flags, help)


def labelled_box(box, cid, label, flags=0):
    return gadget(256, 4, box, cid, [Msg(label)], flags)


def label(box, cid, text, flags=0):
    return gadget(320, 4, box, cid, [Msg(text)], flags)


def radio_button(box, cid, group, text, flags=1, event=0, help=None):
    return gadget(384, 16, box, cid, [W(group), Msg(text), W(len(text) + 1), W(event)], flags, help)


def display_field(box, cid, text, max_len=64, flags=0, help=None):
    return gadget(448, 8, box, cid, [Msg(text), W(max_len)], flags, help)


def writable_field(box, cid, text, max_len=64, allowable=None, flags=1, before=-1, after=-1, help=None):
    return gadget(512, 24, box, cid, [Msg(text), W(max_len), Msg(allowable),
                                      W(0 if allowable is None else len(allowable) + 1), W(before), W(after)],
                  flags, help)


def slider(box, cid, lo, hi, step, value, flags, help=None):
    return gadget(576, 16, box, cid, [W(lo), W(hi), W(step), W(value)], flags, help)


def draggable(box, cid, text, sprite=None, flags=0, help=None):
    return gadget(640, 16, box, cid, [Msg(text), W(len(text) + 1), Str(sprite),
                                      W(0 if sprite is None else len(sprite) + 1)], flags, help)


def popup(box, cid, menu, flags=0, help=None):
    return gadget(704, 4, box, cid, [Str(menu)], flags, help)


def adjuster(box, cid, flags, help=None):
    return gadget(768, 4, box, cid, [W(0)], flags, help)


def number_range(box, cid, lo, hi, step, value, precision=0, display_length=0, flags=0,
                 before=-1, after=-1, help=None):
    return gadget(832, 32, box, cid, [W(lo), W(hi), W(step), W(value), W(precision), W(before), W(after),
                                      W(display_length)], flags, help)


def string_set(box, cid, strings, title, initial, max_len=32, allowable=None, flags=1, before=-1, after=-1,
               help=None):
    return gadget(896, 32, box, cid, [Msg(strings), Msg(title), Msg(initial), W(max_len), Msg(allowable),
                                      W(0 if allowable is None else len(allowable) + 1), W(before), W(after)],
                  flags, help)


def button(box, cid, value, button_flags, validation=None, max_value=None, flags=0, help=None):
    return gadget(960, 20, box, cid, [W(button_flags), Msg(value),
                                      W(max_value or len(value) + 1), Str(validation),
                                      W(0 if validation is None else len(validation) + 1)], flags, help)


# TextGadgets' gadgets (tboxlib objects/gadgets.h): their structures hold
# the header, and a type word of their own after it
def text_area(box, cid, text, flags=4, event=0, fg=0x00000000, bg=0xFFFFFF00, help=None):
    return gadget(0x4018, 20, box, cid, [W(0), W(event), Msg(text), W(fg), W(bg)], flags, help)


def scroll_list(box, cid, flags=0, event=0, fg=0x00000000, bg=0xFFFFFF00, help=None):
    return gadget(0x401A, 12, box, cid, [W(event), W(fg), W(bg)], flags, help)


def scrollbar(box, cid, lo, hi, value, visible, line_inc, page_inc, flags=0, event=0, help=None):
    return gadget(0x401B, 32, box, cid, [W(0), W(event), W(lo), W(hi), W(value), W(visible), W(line_inc),
                                         W(page_inc)], flags, help)


# ---- objects ----

def window(name, title, box, work, gadgets, flags=0, menu=None, help=None, default_focus=-1,
           show_event=0, hide_event=0, window_flags=0x87000012, objflags=0,
           work_flags=0x00003000, title_max=None, colours=(7, 2, 7, 1), toolbars=(None,) * 4,
           own_sprites=False):
    """box: the visible area (x0, y0, x1, y1) on the screen; work: the work
    area's extent; work_flags, the work area's icon flags (its button type);
    title_max, the title's buffer (Window_SetTitle's limit); toolbars, the
    names of its internal bottom-left, internal top-left, external
    bottom-left and external top-left toolbars' windows; own_sprites, its
    icons' sprites from the application's Sprites file, not the Wimp's
    pool.  Version 102."""
    vx0, vy0, vx1, vy1 = box
    wx0, wy0, wx1, wy1 = work
    body = [W(flags), Msg(help), W(0 if help is None else len(help) + 1),
            Str(None), W(0), W(0), W(0),                  # pointer shape, its length, hot spot
            Str(menu), W(0), Off("keys"), W(len(gadgets)), Off("gadgets"),
            W(default_focus), W(show_event), W(hide_event),
            Str(toolbars[0]), Str(toolbars[1]), Str(toolbars[2]), Str(toolbars[3]),
            # the Wimp window block
            W(vx0), W(vy0), W(vx1), W(vy1), W(0), W(0), W(-1), W(window_flags),
            B(*colours), B(3, 1, 12, 0),                  # title fg/bg, work fg/bg, scroll, focus
            W(wx0), W(wy0), W(wx1), W(wy1),
            W(0x0000013D),                                # title: text, border, centred, filled, indirected
            W(work_flags),                                # work area: its button type
            Spr() if own_sprites else W(1),               # the sprite area: the Wimp's pool
            H(0, 0),
            Msg(title), Str(None), W(title_max or len(title) + 1), W(0),
            Label("keys"), Label("gadgets")]
    for g in gadgets:
        body += g
    return Template(WINDOW, name, 102, body, objflags)


def menu_entry(cid, text, flags=0, click_show=None, submenu_show=None, submenu_event=0, click_event=0,
               help=None, text_max=None):
    """text_max: the text's buffer (Menu_SetEntryText's limit)"""
    return [W(flags), W(cid), Msg(text), W(text_max or len(text) + 1), Str(click_show), Str(submenu_show),
            W(submenu_event), W(click_event), Msg(help), W(0 if help is None else len(help) + 1)]


def menu(name, title, entries, flags=0, help=None, show_event=0, hide_event=0, objflags=0):
    """Version 102"""
    body = [W(flags), Msg(title), W(len(title) + 1), Msg(help), W(0 if help is None else len(help) + 1),
            W(show_event), W(hide_event), W(len(entries))]
    for e in entries:
        body += e
    return Template(MENU, name, 102, body, objflags)


def iconbar(name, sprite, text=None, menu=None, select_show=None, adjust_show=None, select_event=0,
            adjust_event=0, help=None, flags=0, position=-1, priority=0, objflags=0):
    return Template(ICONBAR, name, 100, [
        W(flags), W(position), W(priority), Str(sprite), W(len(sprite) + 1),
        Msg(text), W(0 if text is None else len(text) + 1), Str(menu), W(select_event), W(adjust_event),
        Str(select_show), Str(adjust_show), Msg(help), W(0 if help is None else len(help) + 1)], objflags)


def proginfo(name, title, purpose, author, licence, version, uri=None, event=0, flags=0, objflags=0):
    """Version 101: the URI and its event after the window"""
    return Template(PROGINFO, name, 101, [
        W(flags), Msg(title), W(len(title) + 1), Msg(purpose), Msg(author), W(licence), Msg(version),
        Str(None), Msg(uri), W(event)], objflags)


def saveas(name, title, filename, filetype, flags=0, objflags=0):
    return Template(SAVEAS, name, 100, [
        W(flags), Msg(filename), W(filetype), Msg(title), W(len(title) + 1), Str(None)], objflags)


def fileinfo(name, title, filename, filetype, size, modified=0, date=(0, 0), flags=0, objflags=0):
    return Template(FILEINFO, name, 100, [
        W(flags), Msg(title), W(len(title) + 1), W(modified), W(filetype), Msg(filename), W(size),
        W(date[0]), W(date[1]), Str(None)], objflags)


def scale(name, title, lo, hi, step, std=(33, 80, 100, 120), flags=0, objflags=0):
    return Template(SCALE, name, 100, [
        W(flags), W(lo), W(hi), W(step), Msg(title), W(len(title) + 1), Str(None)] + [W(v) for v in std],
        objflags)


def printdbox(name, page_from=1, page_to=1, copies=1, scale=100, flags=0, objflags=0):
    return Template(PRINTDBOX, name, 100, [
        W(flags), W(page_from), W(page_to), W(copies), W(scale), Str(None), Str(None)], objflags)


def fontdbox(name, title, font, height, aspect, try_string, flags=0, objflags=0):
    return Template(FONTDBOX, name, 100, [
        W(flags), Msg(title), W(len(title) + 1), Str(font), W(height), W(aspect), Msg(try_string),
        Str(None)], objflags)


def fontmenu(name, ticked=None, flags=0, objflags=0):
    return Template(FONTMENU, name, 100, [W(flags), Str(ticked)], objflags)


def colourdbox(name, title, colour, flags=0, objflags=0):
    return Template(COLOURDBOX, name, 100, [W(flags), Msg(title), W(len(title) + 1), W(colour)], objflags)


def colourmenu(name, title, colour, flags=0, objflags=0):
    return Template(COLOURMENU, name, 100, [W(flags), Msg(title), W(len(title) + 1), W(colour)], objflags)


def dcs(name, title, message, flags=0, objflags=0):
    return Template(DCS, name, 100, [W(flags), Msg(title), W(len(title) + 1), Msg(message),
                                     W(len(message) + 1), Str(None)], objflags)


def quit(name, title, message, flags=0, objflags=0):
    return Template(QUIT, name, 100, [W(flags), Msg(title), W(len(title) + 1), Msg(message),
                                      W(len(message) + 1), Str(None)], objflags)
