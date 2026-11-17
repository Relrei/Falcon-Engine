# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: Apache-2.0

# Falcon Cycles-expansion Light Mixer (like LuxCore's light groups: change the brightness and color of
# lights after rendering): a gain and a color for each light group of the view layer, applied
# in the compositor without rendering again:
#
#   image = Combined + sum over the groups (color * gain - 1) * Combined_<group>
#
# so the lights in no group stay as they are. The mixer owns one compositor node group ("Falcon Light
# Mixer") and rebuilds it whenever a gain or color changes; a scene with a compositor set up by hand is
# left alone.

from __future__ import annotations

import bpy
from bpy.types import Operator, Panel

MIXER_TREE_NAME = "Falcon Light Mixer"


def mixer_item(scene, name):
    for item in scene.cycles.light_mixer:
        if item.name == name:
            return item
    return None


def mixer_tree_owned(scene):
    tree = scene.compositing_node_group
    return tree is None or tree.get("falcon_light_mixer", False)


def rebuild(scene):
    """Rebuild the mixer's compositor tree from the gains and colors (or drop it when off)."""
    cscene = scene.cycles
    if not mixer_tree_owned(scene):
        return False

    if not cscene.use_light_mixer:
        tree = scene.compositing_node_group
        if tree is not None and tree.get("falcon_light_mixer", False):
            scene.compositing_node_group = None
        return True

    tree = scene.compositing_node_group
    if tree is None:
        tree = bpy.data.node_groups.get(MIXER_TREE_NAME)
        if tree is None or tree.bl_idname != 'CompositorNodeTree':
            tree = bpy.data.node_groups.new(MIXER_TREE_NAME, 'CompositorNodeTree')
        tree["falcon_light_mixer"] = True
        scene.compositing_node_group = tree
    tree.nodes.clear()
    if not tree.interface.items_tree:
        tree.interface.new_socket("Image", in_out='OUTPUT', socket_type='NodeSocketColor')

    layers = tree.nodes.new('CompositorNodeRLayers')
    layers.location = (-600, 0)
    output = tree.nodes.new('NodeGroupOutput')
    output.location = (600, 0)

    current = layers.outputs["Image"]
    x = -200
    for item in cscene.light_mixer:
        socket = layers.outputs.get("Combined_" + item.name)
        if socket is None or not socket.enabled:
            continue
        factor = [c * item.gain - 1.0 for c in item.color]
        if all(abs(f) < 1e-6 for f in factor):
            continue
        scale = tree.nodes.new('ShaderNodeVectorMath')
        scale.operation = 'MULTIPLY'
        scale.location = (x, -200)
        scale.inputs[1].default_value = factor
        tree.links.new(socket, scale.inputs[0])
        add = tree.nodes.new('ShaderNodeVectorMath')
        add.operation = 'ADD'
        add.location = (x + 150, 0)
        tree.links.new(current, add.inputs[0])
        tree.links.new(scale.outputs[0], add.inputs[1])
        current = add.outputs[0]
        x += 300
    tree.links.new(current, output.inputs[0])
    scene.render.use_compositing = True
    return True


def item_update(self, context):
    rebuild(context.scene)


class NEURAL_RENDER_OT_light_mixer_sync(Operator):
    """Add a mixer entry for every light group of the view layer and rebuild the mixer"""
    bl_idname = "neural_render.light_mixer_sync"
    bl_label = "Update Light Groups"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        scene = context.scene
        names = [lg.name for lg in context.view_layer.lightgroups]
        mixer = scene.cycles.light_mixer
        for i in reversed(range(len(mixer))):
            if mixer[i].name not in names:
                mixer.remove(i)
        for name in names:
            if mixer_item(scene, name) is None:
                item = mixer.add()
                item.name = name
        if not rebuild(scene):
            self.report({'WARNING'}, "The compositor is set up by hand: the Light Mixer leaves it alone")
        return {'FINISHED'}


class NEURAL_RENDER_RENDER_PT_light_mixer(Panel):
    bl_label = "Light Mixer"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "view_layer"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'NEURAL_RENDERING'}

    @classmethod
    def poll(cls, context):
        return context.engine in cls.COMPAT_ENGINES

    def draw_header(self, context):
        self.layout.prop(context.scene.cycles, "use_light_mixer", text="")

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        cscene = scene.cycles
        layout.active = cscene.use_light_mixer

        groups = [lg.name for lg in context.view_layer.lightgroups]
        if not groups:
            layout.label(text="Add light groups (Passes > Light Groups) to mix them", icon='INFO')
            return
        if not mixer_tree_owned(scene):
            layout.label(text="The compositor is set up by hand: the mixer is not applied", icon='ERROR')

        missing = [name for name in groups if mixer_item(scene, name) is None]
        if missing or len(cscene.light_mixer) != len(groups):
            layout.operator("neural_render.light_mixer_sync", icon='FILE_REFRESH')

        for name in groups:
            item = mixer_item(scene, name)
            if item is None:
                continue
            row = layout.row(align=True)
            row.label(text=name)
            row.prop(item, "gain", text="")
            row.prop(item, "color", text="")


classes = (
    NEURAL_RENDER_OT_light_mixer_sync,
    NEURAL_RENDER_RENDER_PT_light_mixer,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
