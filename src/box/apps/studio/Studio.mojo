# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alban Read

"""!Studio: ROSAPPS' multitrack studio: the
shell, rosapp's RosApp, over SoundCraft.

Sound files dropped on it -- WAV, AIFF and FLAC -- become clips on the
first track with room for them, drawn with their waveforms along a
timeline.  Clips are picked up and moved, dropped on another track, cut in
two at the playhead and thrown away; the whole arrangement is mixed down
to a sound file again.  It saves the arrangement as a session (&A7D).

The window is a viewport, as !Ledger's is: a timeline is longer than any
sprite, so the engine is told where the window has been scrolled to and
draws only what can be seen.

The document, the clips and the sound files are SoundCraft's; the
arrangement as you see it, and every command, are our own."""

from rosapp import RosApp, AppConfig
from riscos.desktop import Desktop, report_error


def main():
    try:
        var cfg = AppConfig("Studio", "ROSAPPS' multitrack studio, on SoundCraft",
                            "0.1 (20 Oct 2026)", "!studio")
        # its own documents: an arrangement
        cfg.types.append(0xA7D)
        # sound files dropped on it go in as clips.  They are not claimed by
        # RunType: a sound file double-clicked belongs to whatever plays it,
        # not to a studio that would open it for editing.
        for t in [0xFB1, 0xFC2, 0x1CF]:
            cfg.imports.append(t)
        for t in ["rs_select", "rs_zoomin", "rs_zoomout"]:
            cfg.tools.append(String(t))
        cfg.start = String("new")
        cfg.save_type = 0xA7D
        cfg.save_leaf = String("Session")
        cfg.viewport = True             # a timeline is never one picture
        cfg.geometry = False            # the inspector shows the time and the clip
        cfg.view_toggles = False
        cfg.places = True               # a sound file dropped on the window goes in there
        cfg.menu_names.append(String("Track"))
        cfg.menu_items.append(String("New track,Delete track"))
        cfg.menu_names.append(String("Clip"))
        cfg.menu_items.append(String("Split at playhead,Delete,-Move to playhead"))
        # the File menu's exports: the mix, or the arrangement as a picture
        cfg.export_names.append(String("mix as WAV"))
        cfg.export_types.append(0xFB1)
        cfg.export_exts.append(String("wav"))
        cfg.export_names.append(String("mix as AIFF"))
        cfg.export_types.append(0xFC2)
        cfg.export_exts.append(String("aiff"))
        cfg.export_names.append(String("mix as FLAC"))
        cfg.export_types.append(0x1CF)
        cfg.export_exts.append(String("flac"))
        cfg.export_names.append(String("arrangement as sprite"))
        cfg.export_types.append(0xFF9)
        cfg.export_exts.append(String("spr"))
        var desk = Desktop("Studio")
        var app = RosApp(desk, cfg)
        desk.run(app)
    except e:
        _ = report_error(String(e), app_name="Studio")
