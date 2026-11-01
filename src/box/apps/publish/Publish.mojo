# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Publish: the ROSAPPS page layout program.  It is the shared shell
(rosapp.mojo) over an engine built on DesignCraft.

A document is pages with frames on them.  A text frame holds a story that
can run on from one frame to another; a picture frame holds a picture; a
box, an oval or a line is a shape.  The tools in the palette draw frames
and shapes and choose them again to move and size them, and the text tool
puts the caret into text.  The engine draws only the part of the pages in
view, so the shell fixes the picture to the window and tells the engine
when the window scrolls.

It saves its documents as DesignCraft packages (type &A9B) and opens them
and IDML files, which a double click or a drop on the icon bar icon opens.
A picture, a PDF page, or text dropped on the window goes into the frame
selected or onto the current page.  The File menu exports the document as
PDF (or a booklet to fold), EPUB, a web page, IDML or PowerPoint, a story's
text as plain text or RTF, and the current page as Draw or ArtWorks (by way
of !Trace) or as a sprite, PNG or JPEG.

The properties pane has four parts: the frame (colours, lines, columns,
how a picture fits it, its place in the stack), the text (the characters'
font, size and colour, the paragraph's alignment and spacing), the pages
(adding, moving and going to them) and the layout (the paper, margins,
columns and guides).  The inspector tells which page is current, what is
selected and where it is and how big, in millimetres; typing in its fields
moves and sizes the selection.

Master pages carry what is the same on many pages (a page number, a
running head); the Styles menu applies, makes and updates paragraph
styles.  Text can wrap round frames, run on from frame to frame, and carry
page numbers and line, column and page breaks.  The keys are those a page
layout program is expected to have (dtp.rs in the engine lists them).

The menus, the tools and the panes are our own.  DesignCraft holds the
document, composes the text, draws the pages and reads and writes the
files."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Publish", "Page layout, on DesignCraft", "0.1 (22 Oct 2026)",
                            "!publish")
        # its own documents; IDML files (zip) and pictures are taken when
        # dropped
        cfg.types.append(0xA9B)
        for t in [0xA91, 0xB60, 0xC85, 0x695, 0x69C, 0xFF0, 0xADF, 0xAAD]:
            cfg.imports.append(t)
        for t in ["rs_select", "rs_text", "rs_picture", "rs_rect", "rs_ellipse", "rs_line", "rs_pan",
                  "rs_zoomtool"]:
            cfg.tools.append(String(t))
        cfg.start = String("new")
        cfg.new_line = String("newask")
        cfg.save_type = 0xA9B
        cfg.save_leaf = String("Layout")
        cfg.viewport = True             # the pages are longer than any one picture
        cfg.fit = True                  # a page wider than the screen is shown by its width
        cfg.view_rulers = True          # rulers in millimetres, drawn by the engine
        cfg.guides_on = True            # margins, columns and frames show from the start
        cfg.props = True
        cfg.thumbs = True               # the pages part of the properties pane shows each page
        cfg.places = True               # a picture dropped on the window goes into the layout
        cfg.pastes_text = True
        cfg.zoom_keys = True            # Ctrl with keypad + and -, - and 0
        # The engine sends these three again with their keys (^ Ctrl, ⇧
        # Shift), which need RISC OS Latin-1.
        cfg.menu_names.append(String("Page"))
        cfg.menu_items.append(String("Add page,Insert after,Delete page,Duplicate page,"
                                     + "-Move to start,Move earlier,Move later,Move to end,"
                                     + "-Go to page...,Next page,Previous page,-Paper and margins...,"
                                     + "-New master page,Edit master pages,Apply master page...,Page numbering..."))
        cfg.menu_names.append(String("Frame"))
        cfg.menu_items.append(String("Bring to front,Bring forward,Send backward,Send to back,"
                                     + "-Group,Ungroup,-Lock,Unlock all,"
                                     + "-Picture fills frame,Picture fits frame,Frame fits picture,Centre picture,"
                                     + "-Align left,Align centre,Align right,Align top,Align middle,Align bottom,"
                                     + "-Turn left,Turn right,Flip across,Flip down,-Space out across,Space out down,"
                                     + "-Text wrap round frame,No text wrap,-Link text frames,Unlink text frame,"
                                     + "-Round corners,Square corners,Drop shadow,No drop shadow,"
                                     + "-Make text frame,Delete"))
        cfg.menu_names.append(String("Text"))
        cfg.menu_items.append(String("Bold,Italic,Underline,-Larger,Smaller,"
                                     + "-Align left,Centre,Align right,Justify,"
                                     + "-Capitals,Small capitals,Superscript,Subscript,"
                                     + "-Upper case,Lower case,Title case,-Drop capital,"
                                     + "-Page number,New line,Column break,Page break,"
                                     + "-Sample text,-Find...,Find again,Replace...,Check spelling"))
        # The engine fills in the Font menu, and the Styles menu with the
        # document's paragraph styles.
        cfg.menu_names.append(String("Font"))
        cfg.menu_items.append(String("None"))
        cfg.menu_names.append(String("Styles"))
        cfg.menu_items.append(String("New style from text...,Update style from text"))
        # The File menu's Export entries, which save a copy in another
        # format: the document, the text of a story, or the current page
        var names: List[String] = ["PDF", "PDF booklet", "EPUB", "EPUB fixed pages", "Web page", "IDML",
                                   "PowerPoint", "text", "text as RTF", "page as Draw", "page as ArtWorks",
                                   "page as Sprite", "page as PNG", "page as JPEG"]
        var types: List[Int] = [0xADF, 0xADF, 0xA91, 0xA91, 0xFAF, 0xA91, 0xA80, 0xFFF, 0xC32, 0xAFF, 0xD94,
                                0xFF9, 0xB60, 0xC85]
        var exts: List[String] = ["pdf", "booklet", "epub", "fepub", "html", "idml", "pptx", "txt", "rtf",
                                  "draw", "aw", "spr", "png", "jpg"]
        for i in range(len(names)):
            cfg.export_names.append(names[i])
            cfg.export_types.append(types[i])
            cfg.export_exts.append(exts[i])
        var desk = Desktop("Publish")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Publish")
