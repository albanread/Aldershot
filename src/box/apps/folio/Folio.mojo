# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Folio: the ROSAPPS PDF editor.  It is the shared shell (rosapp.mojo)
over an engine built on PDFCraft.

It opens PDF documents (type &ADF) that are double-clicked or dropped on
it.  A picture or a text file dropped on it becomes a new PDF.  It can add
notes, highlights, ink and rectangles to a page, insert, delete and turn
pages, and save the result.  The tools and commands are our own; PDFCraft
reads, edits and draws the documents."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Folio", "ROSAPPS' PDF editor, on PDFCraft", "0.1 (17 Oct 2026)", "!folio")
        cfg.types.append(0xADF)
        # A picture or text file dropped on it becomes a new PDF.
        for t in [0xB60, 0xC85, 0xFF0, 0x695, 0x69C, 0xFFF]:
            cfg.imports.append(t)
        for t in ["rs_select", "rs_note", "rs_highlight", "rs_ink", "rs_rect", "rs_pan", "rs_zoomtool"]:
            cfg.tools.append(String(t))
        cfg.start = String("new")
        cfg.save_type = 0xADF
        cfg.save_leaf = String("Document")
        # Only the pages in view are drawn, so a long PDF is never one huge picture.
        cfg.viewport = True
        cfg.menu_names.append(String("Pages"))
        cfg.menu_items.append(String("Insert page,Delete page,Copy page,-Move up,Move down,"
                                     + "-Rotate right,Rotate left,Rotate all,-Find...,Find again"))
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
