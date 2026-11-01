# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Tower: the tower game, as a ROSAPPS program.  It is the shared shell
(rosapp.mojo) over an engine that runs the tower simulation on the box's
Linux side (ports/rosapps/tower-engine, over ports/tower/sim: a
translation of tower-together, Patrick Hulin's MIT reimplementation of the
1994 tower simulation).

The tower is the document.  The GPU presenter draws it from the blocks the
engine publishes (the tower surface, ports/tower/docs/graphics-design.md),
so scrolling and zooming cost the engine nothing.  The tool palette has
every room the game has, a tool to a group: the kinds of a group (the
hotel's three rooms, the three elevator shafts ...) are chosen on the
properties pane, which says what the tool costs, needs and gives and why
it is refused.  The pane's pages are Tower (the time, the funds, the
stars and what the next star needs, the news), Room (the room inspected:
its state, rent, evaluation, people and their stress), Finances,
Evaluation and Options.  The game's questions (a fire, a bomb threat, a
car taken from a busy shaft) come up in the shell's question box.

Towers are saved as JSON, file type &A9D (Tower).  The Tower menu starts a
new tower (BOX rules unless Options says otherwise) or opens one of the
fixture towers the game is tested with."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Tower", "Tower, a tower-building game. Its simulation is tower-together's (MIT)",
                            "0.1 (25 Oct 2026)", "!tower")
        cfg.types.append(0xA9D)
        for t in ["tw_inspect", "rs_pan", "tw_bulldoze", "tw_floor", "tw_lobby", "tw_office", "tw_condo",
                  "tw_hotel", "tw_food", "tw_shop", "tw_leisure", "tw_services", "tw_amenity", "tw_parking", "tw_metro",
                  "tw_stairs", "tw_elevator", "tw_cars"]:
            cfg.tools.append(String(t))
        for h in ["Inspect: click on a room to see how it is doing, set its rent, or go to it.",
                  "Move the view: drag it to see another part of the tower.",
                  "Bulldoze: click on something built to take it away. Nothing is refunded.",
                  "Floor ($500 a cell): drag along a row to lay floor, on top of floor or the lobby.",
                  "Lobby ($3,000 a cell): drag along the ground floor; every 15 floors up for a sky lobby.",
                  "Office ($40,000): click on a floor. Six workers, and rent every three days.",
                  "Condominium ($80,000): click on a floor. Sold to a household of three.",
                  "Hotel rooms (2 stars): single, twin or suite, chosen on the properties pane.",
                  "Food: fast food (1 star) or a restaurant (3 stars), chosen on the properties pane.",
                  "Shop ($100,000, 3 stars): click on a floor; rent every three days.",
                  "Party hall, cinema (3 stars) or the cathedral (5 stars), chosen on the properties pane.",
                  "Services: security, housekeeping, a medical centre or recycling, chosen on the properties pane.",
                  "Parking (3 stars): spaces, ramps, and under BOX rules the car elevator.",
                  "Metro station ($45,000, 4 stars): three basement floors deep.",
                  "Stairs ($5,000) or an escalator (3 stars): click on the lower of the two floors.",
                  "Elevators: standard, express (3 stars) or service (2 stars); drag up or down over the floors.",
                  "Cars: click on a shaft to add a car or take one away, chosen on the properties pane."]:
            cfg.tool_help.append(String(h))
        cfg.start = String("new")
        cfg.new_line = String("new")
        cfg.save_type = 0xA9D
        cfg.save_leaf = String("Tower")
        cfg.props = True
        cfg.geometry = False                # the status bar has the time, funds and news instead
        cfg.view_toggles = False
        cfg.rulers = False                  # a tower of cells, not paper
        cfg.zoom_keys = True
        cfg.viewport = True                 # where no presenter draws it (it then says so)
        cfg.tiles = True                    # the presenter draws it...
        cfg.tower = True                    # ...as a tower (ROSApps_Tower)
        cfg.menu_names.append(String("Tower"))
        cfg.menu_items.append(String("New tower,Test game (5 stars and $10M),Unlimited tower ($1 billion),-Mixed (test),Sky office (test),Dense hotel (test),Offices (test),Dense office at dawn (test)"))
        cfg.replacing_menus.append(3)
        cfg.view_items = String("Clock runs,Noon,Midnight")
        var desk = Desktop("Tower")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Tower")
