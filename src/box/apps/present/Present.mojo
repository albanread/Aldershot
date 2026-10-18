# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Present: the ROSAPPS presentation program.  It is the shared shell
(rosapp.mojo) over an engine built on DeckCraft.

It opens PowerPoint presentations (.pptx, type &A80) that are
double-clicked or dropped on it.  It can edit the slides (text, shapes,
tables and themes), save them, export them as PDF or as pictures, and show
them on the whole screen.  DeckCraft holds and draws the slides; the
tools, menus and commands are our own."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Present", "ROSAPPS' presentations, on DeckCraft", "0.1 (18 Oct 2026)", "!present")
        cfg.types.append(0xA80)
        for t in ["rs_select", "rs_text", "rs_rect", "rs_rrect", "rs_ellipse", "rs_triangle", "rs_star",
                  "rs_arrow", "rs_line", "rs_pan", "rs_zoomtool"]:
            cfg.tools.append(String(t))
        cfg.start = String("sample")
        cfg.save_type = 0xA80
        cfg.save_leaf = String("Presentation")
        cfg.fit = True                      # a slide is shown whole when opened
        cfg.colours = True                  # the inspector's colour fills the selected shapes
        # A button that starts the show from the first slide.  It does the same
        # as "Show from the start" in the Slide menu (menu 3, item 8) and F5.
        cfg.big_label = String("Show")
        cfg.big_sprite = String("rs_show")
        cfg.big_line = String("menu 3 8")
        cfg.big_help = String("This button shows the presentation on the whole screen, from the first slide.|M"
                              + "Space or Right goes on, Left goes back, Escape ends the show.")
        cfg.menu_names.append(String("Slide"))
        cfg.menu_items.append(String("New slide,New title slide,New blank slide,Copy slide,Delete slide,"
                                     + "-Move up,Move down,-Hide slide,-Show from the start,Show from this slide"))
        cfg.menu_names.append(String("Text"))
        cfg.menu_items.append(String("Bold,Italic,Underline,-Bigger,Smaller,-Align left,Centre,Align right,"
                                     + "-Bullets,Numbers"))
        cfg.menu_names.append(String("Arrange"))
        cfg.menu_items.append(String("To front,Forward,Backward,To back,-Group,Ungroup,-Turn right,Flip across"))
        cfg.menu_names.append(String("Insert"))
        cfg.menu_items.append(String("Text box,Table,-Slide number,Date"))
        # The engine fills in the Theme and Font menus.
        cfg.menu_names.append(String("Theme"))
        cfg.menu_items.append(String("None"))
        cfg.menu_names.append(String("Font"))
        cfg.menu_items.append(String("None"))
        # The File menu's Export entries, which save a copy in another format.
        cfg.export_names.append(String("PDF"))
        cfg.export_types.append(0xADF)
        cfg.export_exts.append(String("pdf"))
        cfg.export_names.append(String("slide as PNG"))
        cfg.export_types.append(0xB60)
        cfg.export_exts.append(String("png"))
        cfg.export_names.append(String("slide as JPEG"))
        cfg.export_types.append(0xC85)
        cfg.export_exts.append(String("jpg"))
        var desk = Desktop("Present")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Present")
