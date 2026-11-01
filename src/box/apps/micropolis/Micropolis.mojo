# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Micropolis: the city simulation, as a ROSAPPS program.  It is the
shared shell (rosapp.mojo) over an engine that runs Micropolis, Electronic
Arts' 2008 GPL release of the original city simulation, on the box's Linux
side (ports/rosapps/micropolis-engine, GPL-3 with EA's additional terms).

The map is the document: the engine draws it at the window's zoom and
keeps the city running while nothing is done.  The tool palette lays
roads, rails and power lines, zones the land and builds; the properties
pane shows the city's population, funds, score and demand, and holds the
budget (tax rate, funding) and the game's speed, level and disasters.
The status bar has the date and the city's news.  The City menu starts a
new city, Scenarios loads the eight scenarios, Disasters sets one off.

Micropolis is a registered trademark of Micropolis Corporation (Micropolis
GmbH) and is licensed here as a courtesy of the owner under the Micropolis
Public Name License."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Micropolis",
                            "Micropolis, the city simulation. Micropolis is a registered trademark of Micropolis GmbH, "
                            + "licensed here as a courtesy of the owner under the Micropolis Public Name License",
                            "0.1 (24 Oct 2026)", "!micropolis")
        cfg.types.append(0xA9C)
        for t in ["mp_query", "rs_pan", "mp_bulldozer", "mp_road", "mp_rail", "mp_wire", "mp_park", "mp_residential",
                  "mp_commercial", "mp_industrial", "mp_police", "mp_fire", "mp_stadium", "mp_seaport", "mp_coal",
                  "mp_nuclear", "mp_airport"]:
            cfg.tools.append(String(t))
        for h in ["Query: click on the map to see what is there: a zone's density, land value, crime and pollution.",
                  "Move the map: drag it to see another part of the city.",
                  "Bulldozer ($1 a square): click or drag to clear the land, rubble and old buildings.",
                  "Road ($10 a square): drag to lay a road. Traffic needs roads between the zones.",
                  "Rail ($20 a square): drag to lay a railway. Trains carry people without making traffic.",
                  "Power line ($5 a square): drag to carry power from a power station to the zones.",
                  "Park ($10): click or drag to plant a park. People like to live near one.",
                  "Residential zone ($100): click to zone 3 by 3 squares for houses.",
                  "Commercial zone ($100): click to zone 3 by 3 squares for shops and offices.",
                  "Industrial zone ($100): click to zone 3 by 3 squares for factories.",
                  "Police station ($500): click to build one. It keeps crime down nearby.",
                  "Fire station ($500): click to build one. It puts fires out nearby.",
                  "Stadium ($5,000): click to build one, 4 by 4 squares, when the people ask for it.",
                  "Seaport ($3,000): click to build one, 4 by 4, on the shore; industry needs it.",
                  "Coal power station ($3,000): click to build one, 4 by 4. It pollutes.",
                  "Nuclear power station ($5,000): click to build one, 4 by 4. Clean, but it can melt down.",
                  "Airport ($10,000): click to build one, 6 by 6; commerce needs it."]:
            cfg.tool_help.append(String(h))
        cfg.start = String("new")
        cfg.new_line = String("new")
        cfg.save_type = 0xA9C
        cfg.save_leaf = String("City")
        cfg.props = True
        cfg.geometry = False                # the status bar has the date, funds and news instead
        cfg.view_toggles = False
        cfg.rulers = False                  # a map of squares, not paper
        cfg.zoom_keys = True
        cfg.thumbs = True               # the pane's pictures: demand, graphs, the small map
        cfg.viewport = True             # where no presenter draws the tiles
        cfg.tiles = True
        cfg.menu_names.append(String("City"))
        cfg.menu_items.append(String("New easy city,New medium city,New hard city"))
        cfg.menu_names.append(String("Scenarios"))
        cfg.menu_items.append(String("Dullsville 1900,San Francisco 1906,Hamburg 1944,Bern 1965,Tokyo 1957,"
                                     + "Detroit 1972,Boston 2010,Rio de Janeiro 2047"))
        cfg.view_items = String("Classic,Smooth,Enhanced,HD,-Night")
        cfg.replacing_menus.append(3)           # City and Scenarios: a city in place of this one
        cfg.replacing_menus.append(4)
        cfg.menu_names.append(String("Disasters"))
        cfg.menu_items.append(String("Fire,Flood,Tornado,Earthquake,Monster,Meltdown"))
        var desk = Desktop("Micropolis")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Micropolis")
