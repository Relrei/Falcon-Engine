# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: Apache-2.0

"""Falcon Cycles-expansion: Fake Fast Caustics (FFC) custom Principled-based shader node.

Adds `Fake Fast Caustics (FFC)` to Shift+A -> Shader in the Shader Node Editor.
Wraps a full Principled BSDF with collapsible Caustics and Convex Lens panels in a single node.
When synced by Falcon's Neural Render (blender/shader.cpp), the node's Caustics and Convex Lens
inputs directly drive the GPU/CPU 6-wavelength + distance convex-lens shadow caustics kernel.
"""

from __future__ import annotations

import bpy
from bpy.types import Operator


FFC_GROUP_NAME = "Fake Fast Caustics (FFC)"
# FFC stays, and the bidirectional caustics get a material node of their own (BD).
BD_GROUP_NAME = "Bidirectional Caustics (BD)"
BD_USAGE_INPUTS = ("BD Caustics Weight",)


def _add_socket(
    interface,
    name: str,
    socket_type: str,
    default=None,
    min_value=None,
    max_value=None,
    parent=None,
    subtype: str | None = None,
    description: str = "",
):
    sock = interface.new_socket(
        name=name,
        in_out='INPUT',
        socket_type=socket_type,
        parent=parent,
    )
    if description:
        sock.description = description
    if subtype is not None and hasattr(sock, "subtype"):
        try:
            sock.subtype = subtype
        except (TypeError, AttributeError):
            pass
    if min_value is not None and hasattr(sock, "min_value"):
        sock.min_value = min_value
    if max_value is not None and hasattr(sock, "max_value"):
        sock.max_value = max_value
    if default is not None and hasattr(sock, "default_value"):
        sock.default_value = default
    return sock


def _wire_caustics_usage_sink(ntree: bpy.types.ShaderNodeTree, n_in):
    _wire_usage_sink(ntree, n_in, FFC_USAGE_INPUTS, "Caustics Enabled", "_falcon_ffc")


FFC_USAGE_INPUTS = (
    "Caustics Strength",
    "Caustics Shadow",
    "Caustics IOR",
    "Caustics Dispersion",
    "Caustics Focus",
    "Caustics Folds",
    "Caustics Ripple",
    "Focal Distance",
    "Convex Lens",
    "Inversion Smooth",
    "Reach",
)


CAUSTICS_ITEM_NAMES = frozenset(FFC_USAGE_INPUTS + BD_USAGE_INPUTS + ("Caustics Enabled", "Bidirectional"))


def _wire_usage_sink(ntree: bpy.types.ShaderNodeTree, n_in, caustics_inputs, gate_input, prefix):
    """Connect Caustics/Convex Lens inputs to an internal AOV sink gated by Caustics Enabled.

    Blender 5.3's socket usage inference grays out group inputs that have no internal link.
    Routing them through a Mix gated by 'Caustics Enabled' into an internal Output AOV keeps
    all sockets active while enabled (and dims them when disabled) with zero render overhead
    because Cycles strips unrequested AOVs during graph simplification.
    """
    nodes = ntree.nodes
    links = ntree.links
    if nodes.get(prefix + "_aov") is not None:
        return

    prev_sock = None
    x_pos = -160
    y_pos = -420
    for idx, sname in enumerate(caustics_inputs):
        sock = n_in.outputs.get(sname)
        if sock is None:
            continue
        if prev_sock is None:
            prev_sock = sock
        else:
            m = nodes.new('ShaderNodeMath')
            m.operation = 'ADD'
            m.name = f"{prefix}_sum_{idx}"
            m.location = (x_pos + idx * 40, y_pos - idx * 30)
            links.new(prev_sock, m.inputs[0])
            links.new(sock, m.inputs[1])
            prev_sock = m.outputs[0]

    if prev_sock is None:
        return

    en_sock = n_in.outputs.get(gate_input) if gate_input else None
    if en_sock is not None:
        mix = nodes.new('ShaderNodeMix')
        mix.data_type = 'FLOAT'
        mix.name = prefix + "_gate"
        mix.location = (180, -480)
        links.new(en_sock, mix.inputs[0])
        mix.inputs[2].default_value = 0.0
        links.new(prev_sock, mix.inputs[3])
        prev_sock = mix.outputs[0]

    aov = nodes.new('ShaderNodeOutputAOV')
    aov.name = prefix + "_aov"
    aov.aov_name = prefix + "_params"
    aov.location = (380, -480)
    links.new(prev_sock, aov.inputs["Value"])


def ensure_ffc_node_group() -> bpy.types.ShaderNodeTree:
    """Create or reuse the Fake Fast Caustics (FFC) ShaderNodeTree."""
    existing = bpy.data.node_groups.get(FFC_GROUP_NAME)
    if existing is not None and existing.bl_idname == 'ShaderNodeTree':
        n_in = next((n for n in existing.nodes if n.bl_idname == 'NodeGroupInput'), None)
        if n_in is not None:
            _wire_caustics_usage_sink(existing, n_in)
        return existing

    ntree = _new_principled_group(FFC_GROUP_NAME, "falcon_ffc_node", _add_ffc_panels)
    n_in = next(n for n in ntree.nodes if n.bl_idname == 'NodeGroupInput')
    _wire_caustics_usage_sink(ntree, n_in)
    return ntree


def _add_ffc_panels(interface):
    # --- Panel: Caustics (open by default) ---
    p_caustics = interface.new_panel(
        "Caustics",
        description="Falcon Fast Caustics shadow-ray spectral caustics settings",
        default_closed=False,
    )
    _add_socket(
        interface, "Caustics Enabled", 'NodeSocketBool', default=True, parent=p_caustics,
        description="Enable Falcon Fake Fast Caustics on this material",
    )
    _add_socket(
        interface, "Caustics Strength", 'NodeSocketFloat', default=1.5, min_value=0.0, max_value=1000.0,
        parent=p_caustics, description="Brightness multiplier of Fast Caustics",
    )
    _add_socket(
        interface, "Caustics Shadow", 'NodeSocketFloat', default=0.5, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_caustics,
        description="Darkness of the glass object shadow outside the caustic lines (0 = bright, 1 = dark)",
    )
    _add_socket(
        interface, "Caustics IOR", 'NodeSocketFloat', default=1.45, min_value=1.01, max_value=10.0,
        parent=p_caustics,
        description="Independent refractive index used only for Fast Caustics (separate from surface BSDF IOR)",
    )
    _add_socket(
        interface, "Caustics Dispersion", 'NodeSocketFloat', default=0.5, min_value=0.0, max_value=20.0,
        parent=p_caustics, description="Spectral wavelength dispersion amount for rainbow caustic bands",
    )
    _add_socket(
        interface, "Caustics Focus", 'NodeSocketFloat', default=1.2, min_value=0.0, max_value=50.0,
        parent=p_caustics, description="Sharpness of the caustic lines and focal highlight",
    )
    _add_socket(
        interface, "Caustics Folds", 'NodeSocketInt', default=2, min_value=1, max_value=8,
        parent=p_caustics, description="Number of folded caustic lines (1-8)",
    )
    _add_socket(
        interface, "Caustics Ripple", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=20.0,
        parent=p_caustics, description="Smooth procedural caustic wave/web modulation for water or textured glass",
    )

    # --- Panel: Convex Lens (open by default) ---
    p_lens = interface.new_panel(
        "Convex Lens",
        description="Distance-based convex-lens Jacobian with SDF-style smooth focal inversion",
        default_closed=False,
    )
    _add_socket(
        interface, "Focal Distance", 'NodeSocketFloat', default=0.8, min_value=0.0, max_value=1000.0,
        subtype='DISTANCE', parent=p_lens,
        description="Distance where convex-lens convergence peaks and smoothly inverts (0 disables)",
    )
    _add_socket(
        interface, "Convex Lens", 'NodeSocketFloat', default=1.0, min_value=0.0, max_value=50.0,
        parent=p_lens, description="Strength of the distance-based convex-lens Jacobian boost",
    )
    _add_socket(
        interface, "Inversion Smooth", 'NodeSocketFloat', default=0.15, min_value=0.01, max_value=10.0,
        parent=p_lens, description="SDF-style smoothing width across the focal inversion boundary",
    )
    _add_socket(
        interface, "Reach", 'NodeSocketFloat', default=2.5, min_value=0.05, max_value=10000.0,
        subtype='DISTANCE', parent=p_lens,
        description="Distance falloff scale (higher values project caustics farther than physical falloff)",
    )



def _new_principled_group(name, marker, add_caustics_panels) -> bpy.types.ShaderNodeTree:
    """A node group wrapping a Principled BSDF, with the panels of add_caustics_panels() after the
    top-level sockets."""
    ntree = bpy.data.node_groups.new(name, 'ShaderNodeTree')
    ntree[marker] = True
    interface = ntree.interface

    # Output
    interface.new_socket(name="BSDF", in_out='OUTPUT', socket_type='NodeSocketShader')

    # --- Top-level Principled BSDF sockets (glass / caustic ready defaults) ---
    _add_socket(interface, "Base Color", 'NodeSocketColor', default=(1.0, 1.0, 1.0, 1.0))
    _add_socket(interface, "Metallic", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=1.0, subtype='FACTOR')
    _add_socket(interface, "Roughness", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=1.0, subtype='FACTOR')
    _add_socket(interface, "IOR", 'NodeSocketFloat', default=1.45, min_value=1.0, max_value=10.0)
    _add_socket(interface, "Alpha", 'NodeSocketFloat', default=1.0, min_value=0.0, max_value=1.0, subtype='FACTOR')
    _add_socket(interface, "Normal", 'NodeSocketVector')

    add_caustics_panels(interface)

    # --- Panel: Transmission (open by default) ---
    p_trans = interface.new_panel("Transmission", default_closed=False)
    _add_socket(
        interface, "Transmission Weight", 'NodeSocketFloat', default=1.0, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_trans,
    )
    _add_socket(
        interface, "Transmission Dispersion Scale", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_trans,
    )
    _add_socket(
        interface, "Transmission Dispersion Abbe Number", 'NodeSocketFloat', default=20.0, min_value=0.0, max_value=1000.0,
        parent=p_trans,
    )
    _add_socket(interface, "Thin Wall", 'NodeSocketBool', default=False, parent=p_trans)

    # --- Panel: Thin Film ---
    p_film = interface.new_panel("Thin Film", default_closed=True)
    _add_socket(
        interface, "Thin Film Thickness", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=100000.0,
        parent=p_film,
    )
    _add_socket(
        interface, "Thin Film IOR", 'NodeSocketFloat', default=1.33, min_value=1.0, max_value=10.0,
        parent=p_film,
    )

    # --- Panel: Subsurface ---
    p_sss = interface.new_panel("Subsurface", default_closed=True)
    _add_socket(
        interface, "Subsurface Weight", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_sss,
    )
    _add_socket(interface, "Subsurface Radius", 'NodeSocketVector', default=(1.0, 0.2, 0.1), parent=p_sss)
    _add_socket(
        interface, "Subsurface Scale", 'NodeSocketFloat', default=0.05, min_value=0.0, max_value=100.0,
        subtype='DISTANCE', parent=p_sss,
    )
    _add_socket(
        interface, "Subsurface IOR", 'NodeSocketFloat', default=1.4, min_value=1.01, max_value=3.8,
        subtype='FACTOR', parent=p_sss,
    )
    _add_socket(
        interface, "Subsurface Anisotropy", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_sss,
    )

    # --- Panel: Specular ---
    p_spec = interface.new_panel("Specular", default_closed=True)
    _add_socket(
        interface, "Specular IOR Level", 'NodeSocketFloat', default=0.5, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_spec,
    )
    _add_socket(interface, "Specular Tint", 'NodeSocketColor', default=(1.0, 1.0, 1.0, 1.0), parent=p_spec)
    _add_socket(
        interface, "Anisotropic", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_spec,
    )
    _add_socket(
        interface, "Anisotropic Rotation", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_spec,
    )
    _add_socket(interface, "Tangent", 'NodeSocketVector', parent=p_spec)

    # --- Panel: Coat ---
    p_coat = interface.new_panel("Coat", default_closed=True)
    _add_socket(
        interface, "Coat Weight", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_coat,
    )
    _add_socket(
        interface, "Coat Roughness", 'NodeSocketFloat', default=0.03, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_coat,
    )
    _add_socket(
        interface, "Coat IOR", 'NodeSocketFloat', default=1.5, min_value=1.0, max_value=4.0,
        parent=p_coat,
    )
    _add_socket(interface, "Coat Tint", 'NodeSocketColor', default=(1.0, 1.0, 1.0, 1.0), parent=p_coat)
    _add_socket(interface, "Coat Normal", 'NodeSocketVector', parent=p_coat)

    # --- Panel: Sheen ---
    p_sheen = interface.new_panel("Sheen", default_closed=True)
    _add_socket(
        interface, "Sheen Weight", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_sheen,
    )
    _add_socket(
        interface, "Sheen Roughness", 'NodeSocketFloat', default=0.5, min_value=0.0, max_value=1.0,
        subtype='FACTOR', parent=p_sheen,
    )
    _add_socket(interface, "Sheen Tint", 'NodeSocketColor', default=(1.0, 1.0, 1.0, 1.0), parent=p_sheen)

    # --- Panel: Emission ---
    p_emit = interface.new_panel("Emission", default_closed=True)
    _add_socket(interface, "Emission Color", 'NodeSocketColor', default=(1.0, 1.0, 1.0, 1.0), parent=p_emit)
    _add_socket(
        interface, "Emission Strength", 'NodeSocketFloat', default=0.0, min_value=0.0, max_value=1000000.0,
        parent=p_emit,
    )

    # --- Build internal graph: Group Input -> Principled BSDF -> Group Output ---
    nodes = ntree.nodes
    links = ntree.links

    n_in = nodes.new('NodeGroupInput')
    n_in.location = (-360, 0)

    n_bsdf = nodes.new('ShaderNodeBsdfPrincipled')
    n_bsdf.location = (0, 0)

    n_out = nodes.new('NodeGroupOutput')
    n_out.location = (340, 0)

    # The Caustics items are read by Cycles from the group node itself
    # (intern/neural_render/blender/shader.cpp), not passed on to the Principled BSDF inside.
    for out_sock in n_in.outputs:
        if out_sock.name in n_bsdf.inputs and out_sock.name not in CAUSTICS_ITEM_NAMES:
            links.new(out_sock, n_bsdf.inputs[out_sock.name])

    links.new(n_bsdf.outputs["BSDF"], n_out.inputs["BSDF"])
    return ntree


def _add_bd_panels(interface):
    p_bd = interface.new_panel(
        "Bidirectional Caustics",
        description="Falcon bidirectional caustics (light tracing and photon mapping) settings of this material",
        default_closed=False,
    )
    _add_socket(
        interface, "BD Caustics Weight", 'NodeSocketFloat', default=1.0, min_value=0.0, max_value=100.0,
        parent=p_bd,
        description="Multiplier on the bidirectional caustics of the light that passes through or reflects off "
        "this material (1 is physically correct). Used while the render's Bidirectional is on",
    )


def ensure_bd_node_group() -> bpy.types.ShaderNodeTree:
    """Create or reuse the Bidirectional Caustics (BD) ShaderNodeTree."""
    existing = bpy.data.node_groups.get(BD_GROUP_NAME)
    if existing is not None and existing.bl_idname == 'ShaderNodeTree':
        return existing
    ntree = _new_principled_group(BD_GROUP_NAME, "falcon_bd_node", _add_bd_panels)
    n_in = next(n for n in ntree.nodes if n.bl_idname == 'NodeGroupInput')
    _wire_usage_sink(ntree, n_in, BD_USAGE_INPUTS, None, "_falcon_bd")
    return ntree


# --- Principled BSDF "Bidirectional" follows the render's Caustics panel ---
# When the caustics method changes, the Principled BSDF items switch with it.
# The Principled BSDF shows its Fast Caustics items while "Bidirectional" is off and the BD items while
# it is on (source/blender/nodes/shader/nodes/node_shader_bsdf_principled.cc).

def _scene_uses_bidirectional(scene) -> bool:
    cscene = getattr(scene, "cycles", None) if scene is not None else None
    return bool(getattr(cscene, "use_bidirectional", False))


def _sync_tree(ntree, use_bidirectional: bool):
    if ntree is None:
        return
    for node in ntree.nodes:
        if node.bl_idname != 'ShaderNodeBsdfPrincipled':
            continue
        sock = node.inputs.get("Bidirectional")
        if sock is None or sock.is_linked:
            continue
        if bool(sock.default_value) != use_bidirectional:
            sock.default_value = use_bidirectional


def sync_principled_bidirectional(scene=None):
    """Set "Bidirectional" of every Principled BSDF to the render's Bidirectional setting."""
    if scene is None:
        scene = getattr(bpy.context, "scene", None)
    use = _scene_uses_bidirectional(scene)
    for mat in bpy.data.materials:
        _sync_tree(mat.node_tree, use)
    for group in bpy.data.node_groups:
        if group.bl_idname == 'ShaderNodeTree':
            _sync_tree(group, use)


# Materials used by no object never show up in depsgraph.updates, so a material made after the
# toggle (or appended) without being assigned would keep the old value. When the number of
# materials or node groups changes, everything is synced once.
_data_counts = None


def _current_data_counts():
    return (len(bpy.data.materials), len(bpy.data.node_groups))


@bpy.app.handlers.persistent
def _on_load_post(*_args):
    global _data_counts
    sync_principled_bidirectional()
    _data_counts = _current_data_counts()


@bpy.app.handlers.persistent
def _on_depsgraph_update_post(scene, depsgraph):
    global _data_counts
    counts = _current_data_counts()
    if counts != _data_counts:
        _data_counts = counts
        sync_principled_bidirectional(scene)
        return
    # Only the materials and node trees that changed (a new material or a new Principled BSDF).
    use = None
    for update in depsgraph.updates:
        id_eval = update.id
        if not isinstance(id_eval, (bpy.types.Material, bpy.types.ShaderNodeTree)):
            continue
        id_orig = id_eval.original
        if use is None:
            use = _scene_uses_bidirectional(scene)
        _sync_tree(id_orig.node_tree if isinstance(id_orig, bpy.types.Material) else id_orig, use)


class NODE_OT_add_falcon_ffc(Operator):
    """Add a Principled-based Fake Fast Caustics (FFC) shader node"""
    bl_idname = "node.add_falcon_ffc"
    bl_label = "Fake Fast Caustics (FFC)"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return (
            space is not None
            and space.type == 'NODE_EDITOR'
            and space.tree_type == 'ShaderNodeTree'
            and space.edit_tree is not None
        )

    def execute(self, context):
        tree = context.space_data.edit_tree
        for n in tree.nodes:
            n.select = False

        ffc_group = ensure_ffc_node_group()
        node = tree.nodes.new('ShaderNodeGroup')
        node.node_tree = ffc_group
        node.name = FFC_GROUP_NAME
        node.label = FFC_GROUP_NAME
        node.width = 260
        node.select = True
        tree.nodes.active = node

        if hasattr(context.space_data, "cursor_location"):
            node.location = context.space_data.cursor_location

        return {'FINISHED'}

    def invoke(self, context, event):
        result = self.execute(context)
        if result == {'FINISHED'}:
            bpy.ops.node.translate_attach_remove_on_cancel('INVOKE_DEFAULT')
        return result


class NODE_OT_add_falcon_bd(Operator):
    """Add a Principled-based Bidirectional Caustics (BD) shader node"""
    bl_idname = "node.add_falcon_bd"
    bl_label = "Bidirectional Caustics (BD)"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return NODE_OT_add_falcon_ffc.poll(context)

    def execute(self, context):
        tree = context.space_data.edit_tree
        for n in tree.nodes:
            n.select = False

        node = tree.nodes.new('ShaderNodeGroup')
        node.node_tree = ensure_bd_node_group()
        node.name = BD_GROUP_NAME
        node.label = BD_GROUP_NAME
        node.width = 260
        node.select = True
        tree.nodes.active = node

        if hasattr(context.space_data, "cursor_location"):
            node.location = context.space_data.cursor_location

        return {'FINISHED'}

    invoke = NODE_OT_add_falcon_ffc.invoke


def _draw_shader_menu(self, context):
    space = context.space_data
    if space and space.tree_type == 'ShaderNodeTree':
        self.layout.operator(NODE_OT_add_falcon_ffc.bl_idname, text="Fake Fast Caustics (FFC)")
        self.layout.operator(NODE_OT_add_falcon_bd.bl_idname, text="Bidirectional Caustics (BD)")


classes = (
    NODE_OT_add_falcon_ffc,
    NODE_OT_add_falcon_bd,
)


def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
    if hasattr(bpy.types, "NODE_MT_category_shader_shader"):
        bpy.types.NODE_MT_category_shader_shader.append(_draw_shader_menu)
    bpy.app.handlers.load_post.append(_on_load_post)
    bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph_update_post)


def unregister():
    from bpy.utils import unregister_class
    if _on_depsgraph_update_post in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_on_depsgraph_update_post)
    if _on_load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load_post)
    if hasattr(bpy.types, "NODE_MT_category_shader_shader"):
        bpy.types.NODE_MT_category_shader_shader.remove(_draw_shader_menu)
    for cls in reversed(classes):
        unregister_class(cls)
