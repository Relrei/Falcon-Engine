# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Ctrl+A selects every file in the File Browser.

Blender's File Browser selects all with A (Alt+A deselects, Ctrl+I inverts) and has nothing on
Ctrl+A, the key most people try first. Picking a long rendered image sequence for the VSE
otherwise means shift-clicking the first and the last file, and the last one is far down the
list. The default keymap has no Ctrl+A in any File Browser keymap (checked against
blender_default.py), so this only adds a key; A, Alt+A and Ctrl+I stay as they are.

`FALCON_FILEBROWSER_CTRL_A=0` leaves the keymap alone (read when the add-on registers).
"""

import os

import bpy

_items = []


def enabled():
    return os.environ.get("FALCON_FILEBROWSER_CTRL_A", "1").strip().lower() not in (
        "", "0", "off", "false", "no",
    )


def register():
    if not enabled():
        return
    keyconfig = bpy.context.window_manager.keyconfigs.addon
    if keyconfig is None:  # background mode
        return
    keymap = keyconfig.keymaps.new(name="File Browser Main", space_type='FILE_BROWSER')
    item = keymap.keymap_items.new("file.select_all", type='A', value='PRESS', ctrl=True)
    item.properties.action = 'SELECT'
    _items.append((keymap, item))


def unregister():
    for keymap, item in _items:
        try:
            keymap.keymap_items.remove(item)
        except Exception:  # noqa: BLE001  the keyconfig can already be gone at exit
            pass
    _items.clear()
