# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Turns DLSS on for F-Cycles.

The NVIDIA DLSS runtime is not shipped with Falcon Engine. It is placed in a Falcon plugin folder
(`falcon_plugins/dlss/`, see `cycles/falcon_plugins.py`), and only then is this add-on listed in
Preferences > Add-ons (`addon_utils._addons_listed_if`). It is off by default: the runtime folder is
handed to NVIDIA NGX, and DLSS offered as a denoiser, only while this add-on is enabled. Disabling it
sets the scenes that use DLSS back to the default denoisers.
"""

bl_info = {
    "name": "NVIDIA DLSS denoiser",
    "author": "Falcon Engine",
    "version": (1, 0, 0),
    "blender": (5, 2, 0),
    "location": "Render Properties > Sampling > Denoise",
    "description": "Use the NVIDIA DLSS runtime in the Falcon plugin folder for F-Cycles denoising",
    "category": "Render",
}

import sys

import bpy

# Read by `cycles.falcon_plugins.dlss_addon_enabled()`.
active = False


def _plugins():
    # The plugin loader is part of Cycles and is imported when Cycles registers.
    return sys.modules.get("cycles.falcon_plugins")


class FalconDLSSPreferences(bpy.types.AddonPreferences):
    bl_idname = __name__

    def draw(self, context):
        layout = self.layout
        plugins = _plugins()
        if plugins is None:
            layout.label(text="Needs the Cycles Render Engine add-on", icon='INFO')
        elif plugins.dlss_status() == 'NO_BUILD':
            layout.label(text="This build has no DLSS support", icon='INFO')
        else:
            plugins.draw_plugins(layout, context)
        layout.label(text="NVIDIA and DLSS are trademarks of NVIDIA Corporation")


def register():
    global active
    bpy.utils.register_class(FalconDLSSPreferences)
    active = True
    if (plugins := _plugins()) is not None:
        plugins.dlss_addon_changed(True)


def unregister():
    global active
    active = False
    if (plugins := _plugins()) is not None:
        plugins.dlss_addon_changed(False)
    bpy.utils.unregister_class(FalconDLSSPreferences)
