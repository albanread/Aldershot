# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Workflows: ROSAPPS' workflow maker: the shell, rosapp's
RosApp, over the diagram engine, which is
!Draft's engine started as !Workflows' own.

A workflow is a Tcl script (&A6C) that takes documents through stages:
its inputs, its outputs, its stages and its tests.  Dropped on !Workflows
it is shown as a page made from it -- what it takes and makes, the stages
drawn as a flowchart, the tests -- and its source, the Tcl, is edited in
the source view, as !Draft edits Markdown.  The wide button swaps the two.

A file dropped on the page is given to the input of its type.  Run (in the
pane, or the Workflow menu) saves the workflow if it was changed and runs
it with *Workflow in a task window; the stages light up as they run, green
when done, red where it failed, and the run's log is put at the page's
foot.  Test runs the workflow's own tests.  Record hears what is done in
the other applications until Stop, and puts it into the workflow as a stage
for each document used.  New makes a workflow from a template to change."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Workflows", "ROSAPPS' workflow maker", "0.1 (20 Oct 2026)", "!workflows")
        cfg.types.append(0xA6C)
        for t in ["rs_select", "rs_text"]:
            cfg.tools.append(String(t))
        cfg.start = String("new")
        cfg.save_type = 0xA6C
        cfg.save_leaf = String("Workflow")
        cfg.props = True
        # any file dropped on the page is an input for the workflow; a
        # workflow dropped there opens
        cfg.places = True
        cfg.imports_any = True
        cfg.new_line = String("new")
        cfg.geometry = False
        cfg.view_toggles = False
        # the wide button: the workflow drawn, or its Tcl
        cfg.big_label = String("Source")
        cfg.big_sprite = String("rs_show")
        cfg.big_line = String("big")
        cfg.big_help = String("Click to swap between the workflow drawn and its Tcl source.")
        cfg.menu_names.append(String("Workflow"))
        cfg.menu_items.append(String("Run,Test,Record,Stop"))
        cfg.menu_names.append(String("Show"))
        cfg.menu_items.append(String("Page,Source,Side by side"))
        cfg.export_names.append(String("web page"))
        cfg.export_types.append(0xFAF)
        cfg.export_exts.append(String("html"))
        cfg.export_names.append(String("text"))
        cfg.export_types.append(0xFFF)
        cfg.export_exts.append(String("txt"))
        var desk = Desktop("Workflows")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Workflows")
