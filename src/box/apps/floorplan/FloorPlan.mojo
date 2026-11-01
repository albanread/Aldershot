# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!FloorPlan: the ROSAPPS floor plan program, for drawing the plan of a
house, a flat or a small office.  It is the shared shell (rosapp.mojo)
over an engine built on CADCraft's geometry, drawing and file formats.

A plan is walls, the doors and windows in them, rooms with their names
and areas, furniture and fittings from a library, dimensions and labels,
drawn to scale (1:20 to 1:200) on an A4 or A3 sheet with a title block.
A wall is drawn by clicking its corners, or by typing its length; a door
or window is put in by clicking on a wall; a room is found by clicking
inside it.  Every size is in metres in the properties pane, which also
has the plan's scale, sheet and grid.

Plans are kept in their own files (FloorPln, &A9A).  They can be printed
to scale, and exported as Draw files, PDF, DXF (for CAD programs), SVG
and PNG pictures."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("FloorPlan", "ROSAPPS' floor plans, on CADCraft", "0.1 (19 Oct 2026)", "!floorplan")
        cfg.types.append(0xA9A)
        for t in ["rs_select", "rs_wall", "rs_door", "rs_window", "rs_room", "rs_furniture", "rs_dimension", "rs_text"]:
            cfg.tools.append(String(t))
        for h in ["The selector: click to select a wall, door, room or piece of furniture, drag to move it.  "
                  + "Drag a selected wall's end to move the corner.",
                  "Walls: click at each corner; type a length (4.2, or 4200 mm) and press Return to set it.  "
                  + "Double-click or press Escape to stop.",
                  "Doors: click on a wall to put a door in.  The door opens towards the side you click.",
                  "Windows: click on a wall to put a window in.",
                  "Rooms: click inside a room the walls close in, to name it and find its area.",
                  "Furniture: choose an item in the properties pane, then click to put it in.  R turns it.",
                  "Dimensions: click at the two ends, then where the dimension line goes.",
                  "Labels: click where a label goes, and type it."]:
            cfg.tool_help.append(String(h))
        cfg.start = String("new")
        cfg.save_type = 0xA9A
        cfg.save_leaf = String("Plan")
        cfg.fit = True                      # the whole sheet is shown when opened
        cfg.props = True
        cfg.geometry = False                # the inspector shows where the pointer is, in metres
        cfg.view_toggles = False
        cfg.rulers = False                  # the sheet is drawn to a scale: paper millimetres mislead
        cfg.menu_names.append(String("Plan"))
        cfg.menu_items.append(String("Find rooms,Dimension outside walls,Centre on the sheet,-Example plan"))
        # the File menu's exports
        cfg.export_names.append(String("Draw file"))
        cfg.export_types.append(0xAFF)
        cfg.export_exts.append(String("aff"))
        cfg.export_names.append(String("PDF"))
        cfg.export_types.append(0xADF)
        cfg.export_exts.append(String("pdf"))
        cfg.export_names.append(String("DXF"))
        cfg.export_types.append(0xDEA)
        cfg.export_exts.append(String("dxf"))
        cfg.export_names.append(String("SVG"))
        cfg.export_types.append(0xAAD)
        cfg.export_exts.append(String("svg"))
        cfg.export_names.append(String("PNG picture"))
        cfg.export_types.append(0xB60)
        cfg.export_exts.append(String("png"))
        var desk = Desktop("FloorPlan")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="FloorPlan")
