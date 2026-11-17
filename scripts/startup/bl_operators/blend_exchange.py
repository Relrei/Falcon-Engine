# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Falcon 1.0: exchanging .blend files with the official Blender.
#
# Falcon renders with Cycles-expansion (render engine "NEURAL_RENDERING") in place of the official Cycles,
# with the same settings (scene.cycles, material.cycles, ...).
# - Opening: scenes saved with Cycles open with Cycles-expansion (windowmanager/intern/wm_files.cc), both
#   from File > Open and from File > Import > Blender (.blend).
# - Export: File > Export > Blender 5.3 (.blend) saves a copy whose scenes use "CYCLES", so the official
#   Blender 5.3 opens them with Cycles. The open file is not changed.

__all__ = (
    "WM_OT_export_blend_official",
)

import os

import bpy
from bpy.types import Operator
from bpy.props import (
    BoolProperty,
    StringProperty,
)
from bpy_extras.io_utils import ExportHelper

# The engine the official Blender knows, and the one taking its place in Falcon.
ENGINE_OFFICIAL = 'CYCLES'
ENGINE_FALCON = 'NEURAL_RENDERING'
# The official version the copy is meant for (the version Falcon is based on).
OFFICIAL_VERSION = "5.3"


class WM_OT_export_blend_official(Operator, ExportHelper):
    """Save a copy of this file that the official Blender 5.3 opens with Cycles"""
    bl_idname = "wm.export_blend_official"
    bl_label = "Export Blender 5.3 (.blend)"
    bl_options = {'REGISTER'}

    filename_ext = ".blend"
    filter_glob: StringProperty(
        default="*.blend",
        options={'HIDDEN'},
    )
    compress: BoolProperty(
        name="Compress",
        description="Write compressed .blend file",
        default=False,
    )
    relative_remap: BoolProperty(
        name="Remap Relative",
        description="Remap relative paths when saving to a different directory",
        default=True,
    )

    def invoke(self, context, event):
        if not self.filepath:
            # Not the name of the open file, so the Falcon file is not written over by accident.
            stem = os.path.splitext(bpy.data.filepath)[0] if bpy.data.filepath else "untitled"
            self.filepath = "{:s}_blender{:s}.blend".format(stem, OFFICIAL_VERSION.replace(".", ""))
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        filepath = bpy.path.ensure_ext(self.filepath, self.filename_ext)
        if bpy.data.filepath and os.path.normcase(os.path.abspath(filepath)) == os.path.normcase(
                os.path.abspath(bpy.data.filepath)):
            self.report({'ERROR'}, "Choose another file than the open one, it is kept for Falcon")
            return {'CANCELLED'}

        changed = []
        for scene in bpy.data.scenes:
            if scene.library is not None:
                continue
            if scene.render.falcon_engine_idname == ENGINE_FALCON:
                scene.render.falcon_engine_idname = ENGINE_OFFICIAL
                changed.append(scene)
        try:
            bpy.ops.wm.save_as_mainfile(
                filepath=filepath,
                check_existing=False,
                copy=True,
                compress=self.compress,
                relative_remap=self.relative_remap,
            )
        finally:
            for scene in changed:
                scene.render.falcon_engine_idname = ENGINE_FALCON

        self.report(
            {'INFO'},
            "Saved for Blender {:s} ({:d} scene(s) set to Cycles): {:s}".format(
                OFFICIAL_VERSION, len(changed), filepath,
            ),
        )
        return {'FINISHED'}


classes = (
    WM_OT_export_blend_official,
)
