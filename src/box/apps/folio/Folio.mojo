# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Folio: the ROSAPPS PDF editor.  It is the shared shell (rosapp.mojo)
over an engine built on PDFCraft.

It opens PDF documents (type &ADF) that are double-clicked or dropped on
its icon.  A picture or a text file dropped on its icon becomes a new PDF.
Dropped on an open document, a PDF's pages are put in after the page in
view, and a picture is put on that page.

Its tools read (follow links, select and copy text, fill in forms), edit
what is on a page (select, move, retype and restyle the page's own text,
added text and comments, delete them), add text, notes, highlights, ink
and shapes, and mark text or areas for redaction.  The properties pane
shows the tool's settings, what is selected, the page and the document's
details.  Pages can be inserted, deleted, copied, moved and turned, and
page numbers and watermarks added.  The tools and commands are our own;
PDFCraft reads, edits and draws the documents."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Folio", "ROSAPPS' PDF editor, on PDFCraft", "0.1 (17 Oct 2026)", "!folio")
        cfg.types.append(0xADF)
        # A picture or text file dropped on it becomes a new PDF.
        for t in [0xB60, 0xC85, 0xFF0, 0x695, 0x69C, 0xFFF]:
            cfg.imports.append(t)
        # each tool with its own help: the shell's is written for drawing programs
        var tools = ["rs_select", "rs_edit", "rs_text", "rs_note", "rs_highlight", "rs_ink", "rs_rect",
                     "rs_redact", "rs_pan", "rs_zoomtool"]
        var helps = [
            "Read: click a link to follow it, click a form's field to fill it in, drag over text to select and copy it.",
            "Edit: click text, a picture or a comment on the page to select it; drag it to move it.  Change it in the properties pane.  Delete removes it.",
            "Add text: click on the page where the text is to go, and type it.",
            "Note: click on the page to put a note there.",
            "Highlight: drag over text to highlight, underline or strike it out.",
            "Ink: draw on the page.",
            "Shapes: drag to draw a rectangle, an oval, a line or an arrow.",
            "Redact: drag over what is to be removed.  Nothing goes until Apply redactions in the Document menu.",
            "",
            "",
        ]
        for i in range(len(tools)):
            cfg.tools.append(String(tools[i]))
            cfg.tool_help.append(String(helps[i]))
        cfg.props = True
        cfg.places = True
        # rulers, guides and the grid are for drawing, not for a PDF's pages
        cfg.view_toggles = False
        cfg.start = String("new")
        cfg.save_type = 0xADF
        cfg.save_leaf = String("Document")
        # Only the pages in view are drawn, so a long PDF is never one huge picture.
        cfg.viewport = True
        cfg.menu_names.append(String("Pages"))
        cfg.menu_items.append(String("Insert page,Delete page,Copy page,-Move up,Move down,"
                                     + "-Rotate right,Rotate left,Rotate all,-Find...,Find again"))
        cfg.menu_names.append(String("Document"))
        cfg.menu_items.append(String("Add page numbers,Add a watermark...,-Apply redactions,"
                                     + "Clear redactions"))
        # The File menu's Export entries, which save a copy in another format.
        cfg.export_names.append(String("text"))
        cfg.export_types.append(0xFFF)
        cfg.export_exts.append(String("txt"))
        cfg.export_names.append(String("page as sprite"))
        cfg.export_types.append(0xFF9)
        cfg.export_exts.append(String("spr"))
        var desk = Desktop("Folio")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Folio")
