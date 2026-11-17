# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Falcon: Preferences > Falcon. What this build does differently from the official Blender, and the
# settings that belong to Falcon.

import os

import bpy
from bpy.types import Panel


# The add-on of Cycles-expansion, which holds the Falcon settings that the render engine reads.
EXPANSION_ADDON = "neural_render"


class FalconPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "falcon"


def _text(layout, text, icon='NONE'):
    layout.label_multiline(text=text, icon=icon)


def _expansion_prefs(context):
    addon = context.preferences.addons.get(EXPANSION_ADDON)
    return addon.preferences if addon else None


class USERPREF_PT_falcon_about(FalconPanel, Panel):
    bl_label = "Falcon Engine"
    bl_options = {'HIDE_HEADER'}

    def draw(self, context):
        layout = self.layout
        col = layout.column()
        build = os.environ.get("FALCON_BUILD_LABEL", "")
        col.label(text="Falcon Engine, based on Blender {:s}".format(bpy.app.version_string), icon='BLENDER')
        if build:
            col.label(text="Build: {:s}".format(build))
        col.separator()
        _text(
            col,
            "Falcon renders with Cycles-expansion in place of the official Cycles. "
            "The panels below explain what differs from the official Blender, and hold the settings of Falcon.",
        )


class USERPREF_PT_falcon_dlss(FalconPanel, Panel):
    bl_label = "DLSS Ray Reconstruction"

    def draw(self, context):
        from os.path import isdir

        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        prefs = _expansion_prefs(context)
        if prefs is None or not hasattr(prefs, "dlss_runtime_path"):
            _text(layout, "Cycles-expansion is not enabled, so its settings cannot be shown.", icon='ERROR')
            return

        layout.prop(prefs, "dlss_runtime_path", text="Runtime Folder")

        col = layout.column()
        try:
            from neural_render.properties import falcon_dlss_runtime_files
        except ImportError:
            falcon_dlss_runtime_files = None

        chosen = bpy.path.abspath(prefs.dlss_runtime_path).strip()
        if chosen and falcon_dlss_runtime_files is not None:
            if not isdir(chosen):
                col.label(text="The folder does not exist", icon='ERROR')
            else:
                files = falcon_dlss_runtime_files(chosen)
                if files:
                    col.label(text="Found: {:s}".format(", ".join(files)), icon='CHECKMARK')
                else:
                    col.label(text="No DLSS Ray Reconstruction runtime in this folder", icon='ERROR')

        launcher = os.environ.get("FALCON_DLSS_RUNTIME_PATH", "")
        if launcher:
            col.label(text="Searched in order: {:s}".format(launcher.replace(os.pathsep, ", ")), icon='VIEWZOOM')

        layout.separator()
        _text(
            layout,
            "DLSS Ray Reconstruction is one of the denoisers of Cycles-expansion. "
            "It needs an NVIDIA RTX GPU selected in System > Render Devices, and its runtime "
            "(not shipped with Falcon). The runtime is looked for in this folder first, then in the NVIDIA driver. "
            "Restart Blender after changing the folder.",
        )


class USERPREF_PT_falcon_features(FalconPanel, Panel):
    bl_label = "Features"

    def draw(self, _context):
        _text(self.layout, "How Falcon differs from the official Blender.")


class FalconFeaturePanel(FalconPanel):
    bl_parent_id = "USERPREF_PT_falcon_features"
    bl_options = {'DEFAULT_CLOSED'}


class USERPREF_PT_falcon_features_expansion(FalconFeaturePanel, Panel):
    bl_label = "Cycles-expansion"

    def draw(self, _context):
        _text(
            self.layout,
            "The render engine of Falcon. It takes the place of the official Cycles, which is not included. "
            "It keeps the settings of Cycles under the same names (scene.cycles, material.cycles, ...), "
            "so files and add-ons made for Cycles find them, and adds the sampling of Neural Rendering.",
        )


class USERPREF_PT_falcon_features_denoisers(FalconFeaturePanel, Panel):
    bl_label = "Denoisers"

    def draw(self, _context):
        _text(
            self.layout,
            "Choose DLSS Ray Reconstruction, OpenImageDenoise or OptiX in the Denoise panels of the render settings. "
            "The chosen denoiser is the one that runs: Falcon never switches to another one by itself. "
            "When it cannot run on the device, the image is not denoised, and the render window, the viewport "
            "and the status bar say so.",
        )


class USERPREF_PT_falcon_features_bidirectional(FalconFeaturePanel, Panel):
    bl_label = "Bidirectional Path Tracing"

    def draw(self, _context):
        _text(
            self.layout,
            "Render Properties > Light Paths > Bidirectional turns on paths that start from the lights: "
            "light tracing, photon mapping and manifold walks, each by itself. "
            "Their kernels are not written yet, so the image is still path traced as without them.",
            icon='INFO',
        )


class USERPREF_PT_falcon_features_exchange(FalconFeaturePanel, Panel):
    bl_label = "Files of the Official Blender"

    def draw(self, _context):
        _text(
            self.layout,
            "Opening: scenes saved with Cycles open with Cycles-expansion, from File > Open and from "
            "File > Import > Blender (.blend).",
        )
        self.layout.separator()
        _text(
            self.layout,
            "Saving for the official Blender: File > Export > Blender 5.3 (.blend) saves a copy that the official "
            "Blender 5.3 opens with Cycles. The open file is not changed. A file saved with File > Save keeps "
            "Cycles-expansion, which the official Blender does not have.",
        )


class USERPREF_PT_falcon_features_import_settings(FalconFeaturePanel, Panel):
    bl_label = "Settings of Other Blender Versions"

    def draw(self, _context):
        layout = self.layout
        _text(
            layout,
            "Falcon keeps its own preferences. The add-ons, extensions and settings of another Blender "
            "can be brought in, from the first start, the Edit menu, or here.",
        )
        layout.operator("preferences.import_other_blender", text="Import From Other Blender...")


classes = (
    USERPREF_PT_falcon_about,
    USERPREF_PT_falcon_dlss,
    USERPREF_PT_falcon_features,
    USERPREF_PT_falcon_features_expansion,
    USERPREF_PT_falcon_features_denoisers,
    USERPREF_PT_falcon_features_bidirectional,
    USERPREF_PT_falcon_features_exchange,
    USERPREF_PT_falcon_features_import_settings,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
