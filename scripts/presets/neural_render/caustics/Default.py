import bpy
cycles = bpy.context.scene.cycles

cycles.use_bidirectional = False
cycles.bidirectional_mode = 'AUTO'
cycles.bidirectional_photons = 1000000
cycles.use_bidirectional_extensions = False
cycles.bidirectional_photon_strength = 1.0
cycles.bidirectional_line_boost = 0.0
cycles.bidirectional_sharpness = 0.0
cycles.bidirectional_glow = 0.0
cycles.bidirectional_glow_radius = 8.0
cycles.bidirectional_dispersion = 0.0
cycles.bidirectional_max_light_bounces = 8
cycles.caustics_unidir_mode = 'SMS'
cycles.blur_glossy = 1.0
cycles.caustics_reflective = True
cycles.caustics_refractive = True
