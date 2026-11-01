# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Trace: the ROSAPPS drawing program.  It is the shared shell
(rosapp.mojo) over an engine built on VectorCraft.

It opens and saves SVG drawings (type &AAD), and also opens PostScript,
EPS, DXF and Illustrator files.  Draw and ArtWorks files dropped on it
are imported.
When it starts with no document, it shows VectorCraft's example poster.
Its tools are VectorCraft's: selection, direct selection, the pen, the
shapes and text."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Trace", "ROSAPPS' illustrator, on VectorCraft", "0.1 (17 Oct 2026)",
                            "!trace")
        cfg.types.append(0xAAD)
        cfg.types.append(0xFF5)             # PostScript and EPS
        cfg.types.append(0xDEA)             # DXF drawings from AutoCAD
        cfg.types.append(0xB2D)             # Illustrator drawings (.ai)
        # Files dropped on a drawing are placed in it; dropped on the icon, they
        # are opened.  These are pictures, PDF pages, Windows metafiles,
        # Photoshop files, and untyped files known by their extension
        # (.afdesign, .emf).
        for t in [0xB60, 0xC85, 0x695, 0xFF0, 0x69C, 0xA66, 0xADF, 0xB2F, 0xF98, 0xFFD]:
            cfg.imports.append(t)
        cfg.places = True
        cfg.imports.append(0xAFF)           # Draw files, when dropped on it
        cfg.imports.append(0xD94)           # ArtWorks files, when dropped on it
        for t in ["rs_select", "rs_direct", "rs_pen", "rs_pencil", "rs_line", "rs_rect", "rs_rrect",
                  "rs_ellipse", "rs_polygon", "rs_star", "rs_text", "rs_eyedrop", "rs_pan", "rs_zoomtool"]:
            cfg.tools.append(String(t))
        cfg.start = String("sample")
        cfg.save_type = 0xAAD
        cfg.save_leaf = String("Drawing")
        cfg.props = True
        cfg.fit = True                      # a drawing is shown whole when opened
        cfg.zoom_keys = True                # Ctrl with + and - zooms, Ctrl-0 the page whole
        cfg.menu_context = True             # MENU shades what the selection cannot have
        cfg.colours = True                  # the inspector's colour fills the selection
        cfg.menu_names.append(String("Arrange"))
        cfg.menu_items.append(String("To front,Forward,Backward,To back,-Group,Ungroup,-Copy beside,"
                                     + "-Turn left,Turn right,Flip across,Flip down,-Lock,Unlock all,Hide,Show all"))
        cfg.menu_names.append(String("Align"))
        cfg.menu_items.append(String("Left edges,Centres across,Right edges,Tops,Middles,Bottoms,"
                                     + "-Centre on the page,-Space across,Space down"))
        cfg.menu_names.append(String("Combine"))
        # the shapes together; then the paths' own points; then lines.  Join
        # the ends is Ctrl-J (and closes a path alone)
        cfg.menu_items.append(String("Add together,Cut away the front,Cut away the back,Overlap only,"
                                     + "Not the overlap,Divide,Trim,Merge,Crop,"
                                     + "-Join the ends,Average the points,Reverse the direction,"
                                     + "Cut at the points,Add points,Offset...,"
                                     + "-Outline the line,Simplify...,Clean up,-Swap fill and line,"
                                     + "-Hairline,Thin line,Medium line,Thick line"))
        cfg.menu_names.append(String("Style"))
        cfg.menu_items.append(String("Colour fills,Colour draws lines,-No fill,No line,-Solid line,Dashed line,"
                                     + "Dotted line,-Arrow at the end,Arrow at the start,Arrows at both ends,"
                                     + "No arrows,-Square corners,Round corners,-Gradient fill,Radial gradient fill"))
        # The engine fills in the Font and Layers menus.
        cfg.menu_names.append(String("Font"))
        cfg.menu_items.append(String("None"))
        cfg.menu_names.append(String("Layers"))
        cfg.menu_items.append(String("None"))
        # Image Trace: a picture made into paths.  The engine fills in the
        # presets, then Expand, Release and the views.
        cfg.menu_names.append(String("Image trace"))
        cfg.menu_items.append(String("None"))
        # The File menu's Export entries, which save a copy in another format.
        # first: a Draw file is what a drawing goes to on RISC OS
        cfg.export_names.append(String("Draw file"))
        cfg.export_types.append(0xAFF)
        cfg.export_exts.append(String("aff"))
        # ArtWorks: for OvationPro and the other programs that place it
        cfg.export_names.append(String("ArtWorks"))
        cfg.export_types.append(0xD94)
        cfg.export_exts.append(String("aw"))
        cfg.export_names.append(String("PNG picture"))
        cfg.export_types.append(0xB60)
        cfg.export_exts.append(String("png"))
        cfg.export_names.append(String("PDF"))
        cfg.export_types.append(0xADF)
        cfg.export_exts.append(String("pdf"))
        cfg.export_names.append(String("JPEG picture"))
        cfg.export_types.append(0xC85)
        cfg.export_exts.append(String("jpg"))
        cfg.export_names.append(String("EPS"))
        cfg.export_types.append(0xFF5)
        cfg.export_exts.append(String("eps"))
        cfg.export_names.append(String("DXF"))
        cfg.export_types.append(0xDEA)
        cfg.export_exts.append(String("dxf"))
        var desk = Desktop("Trace")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Trace")
