# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Draft: ROSAPPS' Markdown and diagram editor: the shell, rosapp's
RosApp, over the diagram engine.  It opens
Markdown files (text, &FFF) dropped on it, shows them as a page --
headings, tables, pictures, code in colour, and any fenced ```mermaid block
drawn as a diagram -- and edits their source.

There are three views: the page, the source, and the two side by side, so
that what is typed is seen drawn.  The wide button at the toolbar's end
swaps the page and the source, and a double click on the page opens the
source at the block clicked.  The properties pane formats the text, finds
and changes, and works on the table or the diagram the caret is in; the
Insert menu puts in tables, pictures and a diagram of each kind, ready to
change.  A picture dropped on the page is put in at the caret.  The page
can be exported as a Draw file, its diagrams and tables as Draw objects.

Eleven kinds of diagram are drawn: flowchart, sequence, class, entity
relationship, state, gantt, git, journey, timeline, C4 and architecture.
The engine is our own over selkie's Mermaid grammars, painted with the same
rasteriser !Trace draws artwork with."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Draft", "ROSAPPS' Markdown and diagram editor", "0.2 (19 Oct 2026)", "!draft")
        # Markdown is a text file in the box (Resources.Internet.files.MimeMap
        # maps text/markdown to &FFF), so that is what it opens and saves.
        # It does not take over RunType_FFF: !Edit keeps text files, and a
        # document comes here by being dropped on it or on its icon.
        cfg.types.append(0xFFF)
        for t in ["rs_select", "rs_text"]:
            cfg.tools.append(String(t))
        cfg.start = String("new")
        cfg.save_type = 0xFFF
        cfg.save_leaf = String("Document")
        cfg.pastes_text = True
        cfg.props = True
        # a picture dropped on the page is put in at the caret
        cfg.places = True
        for t in [0xB60, 0xC85, 0x695, 0x69C]:
            cfg.imports.append(t)
        cfg.new_line = String("new")
        cfg.geometry = False            # the inspector shows where the caret is
        cfg.view_toggles = False        # there are no rulers on a Markdown page
        # the wide button: the two views, one click apart
        cfg.big_label = String("Source")
        cfg.big_sprite = String("rs_show")
        cfg.big_line = String("big")
        cfg.big_help = String("Click to swap between the page and its Markdown source.")
        cfg.menu_names.append(String("Insert"))
        cfg.menu_items.append(String("Heading,Bullet list,Numbered list,Quote,Code block,Table,Rule,Page break,"
                                     + "-Link...,Picture...,-Flowchart,Sequence,Class,Entity relationship,State,"
                                     + "Gantt,Git graph,Journey,Timeline,C4 context,Architecture"))
        # "Show", as the shell's own menu is View (the zoom, the pane)
        cfg.menu_names.append(String("Show"))
        cfg.menu_items.append(String("Page,Source,Side by side,-Light page,Dark page,Crimson page"))
        # the File menu's exports: a copy in another format
        cfg.export_names.append(String("text"))
        cfg.export_types.append(0xFFF)
        cfg.export_exts.append(String("txt"))
        cfg.export_names.append(String("Draw file"))
        cfg.export_types.append(0xAFF)
        cfg.export_exts.append(String("aff"))
        cfg.export_names.append(String("web page"))
        cfg.export_types.append(0xFAF)
        cfg.export_exts.append(String("html"))
        cfg.export_names.append(String("page as sprite"))
        cfg.export_types.append(0xFF9)
        cfg.export_exts.append(String("spr"))
        var desk = Desktop("Draft")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Draft")
