# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Canvas: ROSAPPS' painting application:
the shell, rosapp's RosApp, over PhotoCraft's brush engine.

!Palette is the photo editor -- you open a photograph and put it right.
!Canvas is the other thing: you start with nothing and make a picture.
Both stand on PhotoCraft, but this one is built on the parts !Palette has
never shown: the brush engine with its dynamics, the Mixer Brush, whose
reservoir of paint picks colour up off the canvas as it goes, and
painting symmetry.

Thirty-six brushes in the four groups PhotoCraft arranges them in:
General, Dry Media, Wet Media and Special Effects.  Each painting tool --
the brush, pencil, airbrush, wet brush and eraser -- keeps a brush of its
own, and the properties pane shows all of it: the preset, size, opacity,
flow, hardness, spacing, angle and roundness, the jitters, taper, scatter
and texture, the colour, the wet paint and the canvas.  The box has a
mouse and no tablet, so the pointer's speed stands in for pressure: slow
presses hard, quick skims.

It paints at the speed of the hand: the engine lays the dabs for a move
of the pointer in well under a millisecond, and never stops to wait for
the window to catch up.
"""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Canvas", "ROSAPPS' painting application, on PhotoCraft",
                            "0.2 (20 Oct 2026)", "!canvas")
        # It takes PNG pictures dropped on it and saves them, but does not
        # claim RunType_B60: a picture double-clicked is !Palette's, which
        # is the editor for one that already exists.
        cfg.imports.append(0xB60)
        # each painting tool keeps a brush of its own (the engine's slots)
        for t in ["rs_brush", "rs_pencil", "rs_spray", "rs_smudge", "rs_eraser", "rs_eyedrop"]:
            cfg.tools.append(String(t))
        cfg.tool_help.append(String("The brush: drag to paint. Its brush, size and everything else are in the properties pane; [ and ] make it smaller and larger."))
        cfg.tool_help.append(String("The pencil: a fine hard line. It keeps its own brush, set in the properties pane."))
        cfg.tool_help.append(String("The airbrush: paint builds up while you hold the pointer still."))
        cfg.tool_help.append(String("The wet brush: picks up the paint already on the canvas and pushes it along. Its paint (wetness, load, mix) is in the pane's Paint part."))
        cfg.tool_help.append(String("The eraser: paints the paper's colour back."))
        cfg.tool_help.append(String("The eyedropper: click the picture to take its colour to paint with."))
        cfg.start = String("new")
        cfg.save_type = 0xB60
        cfg.save_leaf = String("Painting")
        cfg.fit = True
        cfg.colours = True              # the colour to paint with, in the inspector
        cfg.props = True
        cfg.geometry = False
        cfg.view_toggles = False
        cfg.menu_names.append(String("Brushes"))
        cfg.menu_items.append(String("Hard Round,Soft Round,Hard Round Fine,Soft Round Large,"
                                     + "Hard Round Pressure Size,Soft Round Pressure Size,"
                                     + "Hard Round Pressure Opacity,Soft Round Pressure Opacity,"
                                     + "Hard Round Pressure Opacity and Flow,Airbrush Soft,"
                                     + "Airbrush Pressure Flow,Calligraphy Flat,-Hard Pencil,Soft Pencil,"
                                     + "Chalk,Charcoal,Pastel,Wax Crayon,Dry Bristle,Rake,"
                                     + "-Watercolor Wet Edges,Watercolor Wash,Ink Pen,Oil Bristle,"
                                     + "Gouache Flat,Ink Wash Sponge,-Spatter,Stipple,Confetti,Foliage,Grass,"
                                     + "Sparkle,Sponge Texture,Canvas Texture,Halftone Dots,Dual Grain"))
        cfg.menu_names.append(String("Canvas"))
        cfg.menu_items.append(String("New 1024 x 768,New 1600 x 1200,-Clear it"))
        cfg.menu_names.append(String("Symmetry"))
        cfg.menu_items.append(String("None,Left and right,Top and bottom,Both ways"))
        cfg.export_names.append(String("PNG"))
        cfg.export_types.append(0xB60)
        cfg.export_exts.append(String("png"))
        var desk = Desktop("Canvas")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Canvas")
