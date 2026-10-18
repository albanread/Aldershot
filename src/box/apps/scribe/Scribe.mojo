# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Scribe: the ROSAPPS word processor.  It is the shared shell
(rosapp.mojo) over an engine built on WordCraft.

It opens Word documents (.docx, type &A7E, and the older .doc, type &AE6)
that are double-clicked or dropped on it, and saves them as .docx.  When
it starts with no document, it shows WordCraft's sample document."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Scribe", "ROSAPPS' word processor, on WordCraft", "0.2 (17 Oct 2026)",
                            "!scribe")
        cfg.types.append(0xA7E)
        cfg.types.append(0xAE6)
        for t in ["rs_select", "rs_pan", "rs_zoomtool"]:
            cfg.tools.append(String(t))
        cfg.start = String("sample")
        cfg.save_type = 0xA7E
        cfg.save_leaf = String("Document")
        cfg.pastes_text = True
        cfg.menu_names.append(String("Text"))
        cfg.menu_items.append(String("Bold,Italic,Underline,-Superscript,Subscript,"
                                     + "-Capitals,Small letters,Sentence case,-Plain text,"
                                     + "-Find...,Find again,Replace all...,"
                                     + "-Check spelling...,Word count"))
        cfg.menu_names.append(String("Paragraph"))
        cfg.menu_items.append(String("Align left,Centre,Align right,Justify,-Indent more,Indent less,"
                                     + "-Single spaced,One and a half,Double spaced,-Bullets,Numbers,"
                                     + "-Body text,Heading 1,Heading 2,Heading 3"))
        cfg.menu_names.append(String("Insert"))
        cfg.menu_items.append(String("Page break,Table,-Today's date,Page number,-Dash,Ellipsis,Copyright sign"))
        cfg.menu_names.append(String("Page"))
        cfg.menu_items.append(String("A4,A5,Letter,-Portrait,Landscape,-Normal margins,Narrow margins,Wide margins"))
        # The engine fills in the Font and Style menus.
        cfg.menu_names.append(String("Font"))
        cfg.menu_items.append(String("None"))
        cfg.menu_names.append(String("Style"))
        cfg.menu_items.append(String("None"))
        # The File menu's Export entries, which save a copy in another format.
        cfg.export_names.append(String("PDF"))
        cfg.export_types.append(0xADF)
        cfg.export_exts.append(String("pdf"))
        cfg.export_names.append(String("Rich Text"))
        cfg.export_types.append(0xC32)
        cfg.export_exts.append(String("rtf"))
        cfg.export_names.append(String("OpenDocument"))
        cfg.export_types.append(0xA81)
        cfg.export_exts.append(String("odt"))
        cfg.export_names.append(String("plain text"))
        cfg.export_types.append(0xFFF)
        cfg.export_exts.append(String("txt"))
        cfg.export_names.append(String("HTML"))
        cfg.export_types.append(0xFAF)
        cfg.export_exts.append(String("html"))
        var desk = Desktop("Scribe")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Scribe")
