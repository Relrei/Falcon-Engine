# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

bl_info = {
    "name": "Cycles-expansion",
    "author": "",
    # Falcon Engine 0.7.0 (after the demo v0.5 and v0.6)
    "version": (0, 7, 0),
    "blender": (2, 80, 0),
    "description": "Neural Render renderer integration",
    "warning": "",
    "doc_url": "https://docs.blender.org/manual/en/latest/render/cycles/",
    "tracker_url": "",
    "support": 'OFFICIAL',
    "category": "Render"}

# Support 'reload' case.
if "bpy" in locals():
    import importlib
    if "engine" in locals():
        importlib.reload(engine)
    if "version_update" in locals():
        importlib.reload(version_update)
    if "ui" in locals():
        importlib.reload(ui)
    if "operators" in locals():
        importlib.reload(operators)
    if "properties" in locals():
        importlib.reload(properties)
    if "presets" in locals():
        importlib.reload(presets)
    if "maketx" in locals():
        importlib.reload(maketx)

import bpy

from . import (
    engine,
    version_update,
)


class NeuralRenderRender(bpy.types.RenderEngine):
    bl_idname = 'NEURAL_RENDERING'
    bl_label = "Cycles-expansion"
    bl_use_eevee_viewport = True
    bl_use_preview = True
    bl_use_exclude_layers = True
    bl_use_spherical_stereo = True
    bl_use_custom_freestyle = True

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.session = None

    def __del__(self):
        engine.free(self)

    # final render
    def update(self, data, depsgraph):
        if not self.session:
            if self.is_preview:
                cscene = bpy.context.scene.cycles
                use_osl = cscene.shading_system

                engine.create(self, data, preview_osl=use_osl)
            else:
                engine.create(self, data)

        engine.reset(self, data, depsgraph)

    def render(self, depsgraph):
        engine.render(self, depsgraph)

    def render_frame_finish(self):
        engine.render_frame_finish(self)

    def draw(self, context, depsgraph):
        engine.draw(self, depsgraph, context.space_data)

    def bake(self, depsgraph, obj, pass_type, pass_filter, width, height):
        engine.bake(self, depsgraph, obj, pass_type, pass_filter, width, height)

    # viewport render
    def view_update(self, context, depsgraph):
        if not self.session:
            engine.create(self, context.blend_data,
                          context.region, context.space_data, context.region_data)

        engine.reset(self, context.blend_data, depsgraph)
        engine.sync(self, depsgraph, context.blend_data)

    def view_draw(self, context, depsgraph):
        engine.view_draw(self, depsgraph, context.region, context.space_data, context.region_data)

    def view_pause(self, context):
        engine.view_pause(self, True)

    def view_resume(self, context):
        engine.view_pause(self, False)

    def update_script_node(self, node):
        if engine.with_osl():
            from . import osl
            osl.update_script_node(node, self.report)
        else:
            self.report({'ERROR'}, "OSL support disabled in this build")

    def update_custom_camera(self, cam):
        if engine.with_osl():
            from . import osl
            osl.update_custom_camera_shader(cam, self.report)
        else:
            self.report({'ERROR'}, "OSL support disabled in this build")

    def update_render_passes(self, scene, srl):
        engine.register_passes(self, scene, srl)


def engine_exit():
    engine.exit()


classes = (
    NeuralRenderRender,
)


cli_commands = []


def register():
    from bpy.utils import register_class
    from . import ui
    from . import operators
    from . import properties
    from . import presets
    from . import light_mixer
    from .maketx import maketx_command
    import atexit

    # Make sure we only registered the callback once.
    atexit.unregister(engine_exit)
    atexit.register(engine_exit)

    engine.init()

    properties.register()
    ui.register()
    operators.register()
    presets.register()
    light_mixer.register()

    for cls in classes:
        register_class(cls)

    # Falcon: the DLSS runtime folder from Preferences > Falcon (the setting is read with the preferences).
    addon = bpy.context.preferences.addons.get(__package__)
    properties.falcon_dlss_runtime_apply(addon.preferences if addon else None)

    # Falcon 1.0: 公式の Cycles を外したので、古い場面の直し(と前の NR の設定の引き継ぎ)はこちらで行う。
    bpy.app.handlers.version_update.append(version_update.do_versions)

    cli_commands.append(bpy.utils.register_cli_command("neural_render_maketx", maketx_command))


def unregister():
    from bpy.utils import unregister_class
    from . import ui
    from . import operators
    from . import properties
    from . import presets

    if version_update.do_versions in bpy.app.handlers.version_update:
        bpy.app.handlers.version_update.remove(version_update.do_versions)

    for cmd in cli_commands:
        bpy.utils.unregister_cli_command(cmd)
    cli_commands.clear()

    from . import light_mixer
    light_mixer.unregister()
    ui.unregister()
    operators.unregister()
    properties.unregister()
    presets.unregister()

    for cls in classes:
        unregister_class(cls)
