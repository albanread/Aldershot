# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Palette: the ROSAPPS paint program.  It is the shared shell
(rosapp.mojo) over an engine built on PhotoCraft.

It opens PNG, JPEG, TIFF, GIF and BMP pictures that are double-clicked or
dropped on it.  RISC OS sprites can be dropped on it too, but they are
still opened by Paint when double-clicked.  It can paint, select (by
box, ellipse, outline, colour, or the main subject of a photo), copy,
paste and move what is selected, fill, crop, turn, resize, correct
colours, apply effects and remove red eyes, and it saves pictures as PNG.
Its properties pane holds the tool's settings, the adjustments and the
picture's size.  The engine uses PhotoCraft's pictures, file formats and
brushes, with commands of our own."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Palette", "ROSAPPS' paint package, on PhotoCraft", "0.1 (17 Oct 2026)",
                            "!palette")
        # It opens PNG, JPEG, TIFF, GIF and BMP files.  Sprites belong to Paint,
        # so they are only taken when dropped on it.
        for t in [0xB60, 0xC85, 0xFF0, 0x695, 0x69C]:
            cfg.types.append(t)
        cfg.imports.append(0xFF9)
        for t in ["rs_marquee", "rs_lasso", "rs_wand", "rs_brush", "rs_spray", "rs_smudge", "rs_eraser", "rs_fill",
                  "rs_gradient", "rs_eyedrop", "rs_redeye", "rs_crop", "rs_move", "rs_pan", "rs_zoomtool"]:
            cfg.tools.append(String(t))
        cfg.start = String("new")
        cfg.save_type = 0xB60
        cfg.save_leaf = String("Picture")
        cfg.fit = True
        cfg.colours = True
        cfg.props = True
        cfg.menu_names.append(String("Picture"))
        cfg.menu_items.append(String("Rotate right,Rotate left,Turn over,Flip across,Flip down,-Half size,"
                                     + "Double size,-Brighter,Darker,More contrast,Less contrast,More colour,"
                                     + "Less colour,Grey,-Auto colour"))
        cfg.menu_names.append(String("Effects"))
        cfg.menu_items.append(String("Blur,Blur more,Sharpen,Sharpen more,-Reduce noise,Add noise,-Pixelate,"
                                     + "Emboss,Find edges,Invert,Posterize,Sepia,-Oil paint,Cutout,Watercolour"))
        cfg.menu_names.append(String("Select"))
        cfg.menu_items.append(String("All,None,Inverse,-Subject,-Grow,Shrink,Soften the edge,Edge only,-Crop to it,Fill in"))
        cfg.menu_names.append(String("Distort"))
        cfg.menu_items.append(String("Twirl,Pinch,Bulge,Ripple,-Motion blur,Wind,-Crystals,Dots,Mezzotint,Halftone,"
                                     + "-Diffuse,Solarise,-Smooth,Dust and scratches"))
        cfg.menu_names.append(String("Brushes"))
        cfg.menu_items.append(String("Plain round,Soft round,Hard round,-Pencil,Soft pencil,Ink pen,Calligraphy,"
                                     + "-Chalk,Charcoal,Pastel,Crayon,-Watercolour,Watercolour edges,Oil bristle,"
                                     + "Dry bristle,Gouache,Sponge,Ink wash,-Spatter,Stipple,Grass,Leaves,Confetti,"
                                     + "Sparkle,Rake,Halftone,Canvas"))
        # The File menu's Export entries, which save a copy in another format.
        # sprites: 16 million colours, or 256 with a palette made for the
        # picture for programs older than RISC OS 3.5 (the engine writes both)
        cfg.export_names.append(String("Sprite"))
        cfg.export_types.append(0xFF9)
        cfg.export_exts.append(String("spr"))
        cfg.export_names.append(String("Sprite (256 colours)"))
        cfg.export_types.append(0xFF9)
        cfg.export_exts.append(String("spr8"))
        cfg.export_names.append(String("JPEG"))
        cfg.export_types.append(0xC85)
        cfg.export_exts.append(String("jpg"))
        cfg.export_names.append(String("TIFF"))
        cfg.export_types.append(0xFF0)
        cfg.export_exts.append(String("tif"))
        cfg.export_names.append(String("GIF"))
        cfg.export_types.append(0x695)
        cfg.export_exts.append(String("gif"))
        cfg.export_names.append(String("BMP"))
        cfg.export_types.append(0x69C)
        cfg.export_exts.append(String("bmp"))
        cfg.export_names.append(String("WebP"))
        cfg.export_types.append(0xA66)
        cfg.export_exts.append(String("webp"))
        cfg.export_names.append(String("Photoshop"))
        cfg.export_types.append(0xF98)
        cfg.export_exts.append(String("psd"))
        var desk = Desktop("Palette")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Palette")
