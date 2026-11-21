# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from bl_operators.presets import AddPresetBase
from bpy.types import Operator


class AddPresetIntegrator(AddPresetBase, Operator):
    '''Add an Integrator Preset'''
    bl_idname = "render.neural_render_integrator_preset_add"
    bl_label = "Add Integrator Preset"
    preset_menu = "NEURAL_RENDER_PT_integrator_presets"

    preset_defines = [
        "cycles = bpy.context.scene.cycles"
    ]

    preset_values = [
        "cycles.max_bounces",
        "cycles.diffuse_bounces",
        "cycles.glossy_bounces",
        "cycles.transmission_bounces",
        "cycles.volume_bounces",
        "cycles.transparent_max_bounces",
        "cycles.caustics_reflective",
        "cycles.caustics_refractive",
        "cycles.blur_glossy",
        "cycles.use_fast_gi",
        "cycles.ao_bounces",
        "cycles.ao_bounces_render",
    ]

    preset_subdir = "neural_render/integrator"


class AddPresetSampling(AddPresetBase, Operator):
    '''Add a Sampling Preset'''
    bl_idname = "render.neural_render_sampling_preset_add"
    bl_label = "Add Sampling Preset"
    preset_menu = "NEURAL_RENDER_PT_sampling_presets"

    preset_defines = [
        "cycles = bpy.context.scene.cycles"
    ]

    preset_values = [
        "cycles.use_adaptive_sampling",
        "cycles.samples",
        "cycles.adaptive_threshold",
        "cycles.adaptive_min_samples",
        "cycles.time_limit",
        "cycles.use_denoising",
        "cycles.denoiser",
        "cycles.denoising_input_passes",
        "cycles.denoising_prefilter",
        "cycles.denoising_quality",
    ]

    preset_subdir = "neural_render/sampling"


class AddPresetViewportSampling(AddPresetBase, Operator):
    '''Add a Viewport Sampling Preset'''
    bl_idname = "render.neural_render_viewport_sampling_preset_add"
    bl_label = "Add Viewport Sampling Preset"
    preset_menu = "NEURAL_RENDER_PT_viewport_sampling_presets"

    preset_defines = [
        "cycles = bpy.context.scene.cycles"
    ]

    preset_values = [
        "cycles.use_preview_adaptive_sampling",
        "cycles.preview_samples",
        "cycles.preview_adaptive_threshold",
        "cycles.preview_adaptive_min_samples",
        "cycles.use_preview_denoising",
        "cycles.preview_denoiser",
        "cycles.preview_denoising_input_passes",
        "cycles.preview_denoising_prefilter",
        "cycles.preview_denoising_quality",
        "cycles.preview_denoising_start_sample",
    ]

    preset_subdir = "neural_render/viewport_sampling"


class AddPresetPerformance(AddPresetBase, Operator):
    '''Add an Performance Preset'''
    bl_idname = "render.neural_render_performance_preset_add"
    bl_label = "Add Performance Preset"
    preset_menu = "NEURAL_RENDER_PT_performance_presets"

    preset_defines = [
        "render = bpy.context.scene.render",
        "cycles = bpy.context.scene.cycles",
    ]

    preset_values = [
        "render.threads_mode",
        "render.use_persistent_data",
        "cycles.debug_use_spatial_splits",
        "cycles.debug_use_compact_bvh",
        "cycles.debug_use_hair_bvh",
        "cycles.debug_bvh_time_steps",
        "cycles.tile_size",
    ]

    preset_subdir = "neural_render/performance"


class AddPresetCaustics(AddPresetBase, Operator):
    '''Add a Caustics Preset'''
    bl_idname = "render.neural_render_caustics_preset_add"
    bl_label = "Add Caustics Preset"
    preset_menu = "NEURAL_RENDER_PT_caustics_presets"

    preset_defines = [
        "cycles = bpy.context.scene.cycles"
    ]

    preset_values = [
        "cycles.use_bidirectional",
        "cycles.bidirectional_mode",
        "cycles.bidirectional_photons",
        "cycles.use_bidirectional_extensions",
        "cycles.bidirectional_photon_strength",
        "cycles.bidirectional_line_boost",
        "cycles.bidirectional_sharpness",
        "cycles.bidirectional_glow",
        "cycles.bidirectional_glow_radius",
        "cycles.bidirectional_dispersion",
        "cycles.bidirectional_max_light_bounces",
        "cycles.caustics_unidir_mode",
        "cycles.blur_glossy",
        "cycles.caustics_reflective",
        "cycles.caustics_refractive",
    ]

    preset_subdir = "neural_render/caustics"


class AddPresetFastCaustics(AddPresetBase, Operator):
    '''Add a Material Fast Caustics Preset'''
    bl_idname = "material.neural_render_fast_caustics_preset_add"
    bl_label = "Add Fast Caustics Preset"
    preset_menu = "NEURAL_RENDER_PT_fast_caustics_presets"

    preset_defines = [
        "cmat = bpy.context.material.cycles"
    ]

    preset_values = [
        "cmat.use_fast_caustics",
        "cmat.fast_caustics_strength",
        "cmat.fast_caustics_shadow",
        "cmat.fast_caustics_ior",
        "cmat.fast_caustics_dispersion",
        "cmat.fast_caustics_focus",
        "cmat.fast_caustics_folds",
        "cmat.fast_caustics_ripple",
        "cmat.fast_caustics_focal_distance",
        "cmat.fast_caustics_lens_power",
        "cmat.fast_caustics_lens_smooth",
        "cmat.fast_caustics_reach",
    ]

    preset_subdir = "neural_render/fast_caustics"


classes = (
    AddPresetIntegrator,
    AddPresetSampling,
    AddPresetViewportSampling,
    AddPresetPerformance,
    AddPresetCaustics,
    AddPresetFastCaustics,
)


def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)


def unregister():
    from bpy.utils import unregister_class
    for cls in classes:
        unregister_class(cls)


if __name__ == "__main__":
    register()
