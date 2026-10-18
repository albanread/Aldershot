# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Shell: the empty container that every ROSAPPS application is built
from.  It runs the shared shell (rosapp.mojo) over a stub engine, so it has
nothing to edit.  It is used to try out the window, the panes, the rulers,
the page and the protocol between the shell and its engine."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Shell", "ROSAPPS' container application", "0.03 (17 Oct 2026)", "!shell")
        for t in ["rs_select", "rs_pan", "rs_zoomtool", "rs_measure"]:
            cfg.tools.append(String(t))
        var desk = Desktop("Shell")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Shell")
