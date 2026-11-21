import bpy
cmat = bpy.context.material.cycles

cmat.use_fast_caustics = True
cmat.fast_caustics_strength = 1.5
cmat.fast_caustics_shadow = 0.5
cmat.fast_caustics_ior = 1.45
cmat.fast_caustics_dispersion = 0.5
cmat.fast_caustics_focus = 1.2
cmat.fast_caustics_folds = 2
cmat.fast_caustics_ripple = 0.0
cmat.fast_caustics_focal_distance = 0.8
cmat.fast_caustics_lens_power = 1.0
cmat.fast_caustics_lens_smooth = 0.15
cmat.fast_caustics_reach = 2.5
