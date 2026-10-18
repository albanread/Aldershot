# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Ledger: the ROSAPPS spreadsheet.  It is the shared shell (rosapp.mojo)
over an engine built on gridcraft.

It opens workbooks (.xlsx, type &A7F) and CSV files (type &DFE) that are
double-clicked or dropped on it, edits them and saves them.  The engine
draws only the part of the sheet that is in view, so the shell fixes the
picture to the window and tells the engine when the window scrolls.  The
current cell's contents are shown in the formula bar under the toolbar.
The menus and the look of the grid are our own; gridcraft holds the
workbook, works out the formulas and reads and writes the files."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Ledger", "ROSAPPS' spreadsheet, on gridcraft", "0.1 (17 Oct 2026)", "!ledger")
        cfg.types.append(0xA7F)
        cfg.types.append(0xDFE)
        for t in ["rs_select"]:
            cfg.tools.append(String(t))
        cfg.start = String("sample budget")
        cfg.save_type = 0xA7F
        cfg.save_leaf = String("Workbook")
        cfg.menu_names.append(String("Format"))
        cfg.menu_items.append(String("Bold,Italic,Underline,-Align left,Centre,Align right,"
                                     + "-General,Number,Currency,Percent,More decimals,Fewer decimals,"
                                     + "-Wrap text,Fit column width"))
        cfg.menu_names.append(String("Sheet"))
        cfg.menu_items.append(String("Insert rows,Delete rows,Insert columns,Delete columns,"
                                     + "-Sort ascending,Sort descending,-Fill down,Fill right,-Clear formats,"
                                     + "-Sum,-Next sheet,Previous sheet,New sheet,Delete sheet,"
                                     + "-Gridlines,Headings,-Find...,Rename sheet..."))
        cfg.viewport = True
        cfg.geometry = False
        cfg.view_toggles = False
        cfg.formula_bar = True
        # The File menu's Export entries, which save a copy in another format.
        cfg.export_names.append(String("CSV"))
        cfg.export_types.append(0xDFE)
        cfg.export_exts.append(String("csv"))
        cfg.export_names.append(String("HTML"))
        cfg.export_types.append(0xFAF)
        cfg.export_exts.append(String("html"))
        var desk = Desktop("Ledger")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Ledger")
