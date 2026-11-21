/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/types.h"

#include "device/denoise.h" /* For the parameters and type enum. */
#include "graph/node.h"
#include "integrator/adaptive_sampling.h"
#include "integrator/bidirectional.h"
#include "integrator/guiding.h"

CCL_NAMESPACE_BEGIN

class Device;
class DeviceScene;
class Scene;

struct HaltonSequence {
  HaltonSequence()
  {
    reset();
  }

  void reset()
  {
    a2 = 0;
    b2 = 1;
    a3 = 0;
    b3 = 1;
  }
  float2 next();

  int a2, b2;
  int a3, b3;
};

class Integrator : public Node {
 public:
  NODE_DECLARE

  NODE_SOCKET_API(int, min_bounce)
  NODE_SOCKET_API(int, max_bounce)

  NODE_SOCKET_API(int, max_diffuse_bounce)
  NODE_SOCKET_API(int, max_glossy_bounce)
  NODE_SOCKET_API(int, max_transmission_bounce)
  NODE_SOCKET_API(int, max_volume_bounce)

#ifdef WITH_CYCLES_DEBUG
  NODE_SOCKET_API(DirectLightSamplingType, direct_light_sampling_type)
#endif

  NODE_SOCKET_API(int, transparent_min_bounce)
  NODE_SOCKET_API(int, transparent_max_bounce)

  NODE_SOCKET_API(int, ao_bounces)
  NODE_SOCKET_API(float, ao_factor)
  NODE_SOCKET_API(float, ao_distance)
  NODE_SOCKET_API(float, ao_additive_factor)

  NODE_SOCKET_API(bool, volume_ray_marching)
  NODE_SOCKET_API(int, volume_max_steps)
  NODE_SOCKET_API(float, volume_step_rate)

  NODE_SOCKET_API(bool, use_guiding);
  NODE_SOCKET_API(bool, deterministic_guiding);
  NODE_SOCKET_API(bool, use_surface_guiding);
  NODE_SOCKET_API(float, surface_guiding_probability);
  NODE_SOCKET_API(bool, use_volume_guiding);
  NODE_SOCKET_API(float, volume_guiding_probability);
  NODE_SOCKET_API(int, guiding_training_samples);
  NODE_SOCKET_API(bool, use_guiding_direct_light);
  NODE_SOCKET_API(bool, use_guiding_mis_weights);
  NODE_SOCKET_API(GuidingDistributionType, guiding_distribution_type);
  NODE_SOCKET_API(GuidingDirectionalSamplingType, guiding_directional_sampling_type);
  NODE_SOCKET_API(float, guiding_roughness_threshold);

  /* Falcon Cycles-expansion: bidirectional light transport (integrator/bidirectional.h). */
  NODE_SOCKET_API(bool, use_bidirectional)
  /* Automatic: the techniques are chosen by Falcon (all that have kernels), and light tracing stops
   * for the rest of a frame when its first sample adds next to nothing (PathTraceWorkGPU). */
  NODE_SOCKET_API(bool, use_bidirectional_auto)
  /* Photon mapping: light paths of the photon pass (and the most photons kept), strength. */
  NODE_SOCKET_API(int, bidirectional_photons)
  NODE_SOCKET_API(float, bidirectional_photon_strength)
  NODE_SOCKET_API(float, bidirectional_caustic_spread)
  /* Caustics Override: fold lines brighter (Line Boost) and narrower (Sharpness), and a soft glow
   * of Glow times the light, Glow Radius photon radii wide (kernel/integrator/photon_map.h). */
  NODE_SOCKET_API(float, bidirectional_line_boost)
  NODE_SOCKET_API(float, bidirectional_sharpness)
  NODE_SOCKET_API(float, bidirectional_glow)
  NODE_SOCKET_API(float, bidirectional_glow_radius)
  NODE_SOCKET_API(float, bidirectional_dispersion)
  NODE_SOCKET_API(bool, use_bidirectional_light_tracing)
  NODE_SOCKET_API(bool, use_bidirectional_photon_mapping)
  NODE_SOCKET_API(bool, use_bidirectional_manifold)
  NODE_SOCKET_API(int, bidirectional_max_light_bounces)
  NODE_SOCKET_API(bool, use_bidirectional_photon_cache)
  NODE_SOCKET_API(bool, use_fast_caustics)
  NODE_SOCKET_API(float, fast_caustics_strength)
  NODE_SOCKET_API(float, fast_caustics_dispersion)
  NODE_SOCKET_API(float, fast_caustics_focus)
  NODE_SOCKET_API(int, fast_caustics_folds)
  NODE_SOCKET_API(float, fast_caustics_ripple)
  NODE_SOCKET_API(bool, use_sms_caustics)
  NODE_SOCKET_API(int, sms_max_iterations)
  NODE_SOCKET_API(float, sms_cone_angle)
  NODE_SOCKET_API(float, sms_direct_ratio)
  NODE_SOCKET_API(int, mnee_max_iterations)
  NODE_SOCKET_API(float, mnee_solver_threshold)

  NODE_SOCKET_API(bool, caustics_reflective)
  NODE_SOCKET_API(bool, caustics_refractive)
  NODE_SOCKET_API(float, filter_glossy)

  NODE_SOCKET_API(bool, use_direct_light);
  NODE_SOCKET_API(bool, use_indirect_light);
  NODE_SOCKET_API(bool, use_diffuse);
  NODE_SOCKET_API(bool, use_glossy);
  NODE_SOCKET_API(bool, use_transmission);
  NODE_SOCKET_API(bool, use_emission);

  NODE_SOCKET_API(int, seed)

  NODE_SOCKET_API(float, sample_clamp_direct)
  NODE_SOCKET_API(float, sample_clamp_indirect)
  NODE_SOCKET_API(bool, motion_blur)

  /* Maximum number of samples, beyond which we are likely to run into
   * precision issues for sampling patterns. */
  static const int MAX_SAMPLES = (1 << 24);

  NODE_SOCKET_API(int, aa_samples)

  NODE_SOCKET_API(bool, use_sample_subset)
  NODE_SOCKET_API(int, sample_subset_offset)
  NODE_SOCKET_API(int, sample_subset_length)

  NODE_SOCKET_API(bool, use_light_tree)
  NODE_SOCKET_API(float, light_sampling_threshold)

  NODE_SOCKET_API(bool, use_adaptive_sampling)
  NODE_SOCKET_API(int, adaptive_min_samples)
  NODE_SOCKET_API(float, adaptive_threshold)

  NODE_SOCKET_API(SamplingPattern, sampling_pattern)
  NODE_SOCKET_API(float, scrambling_distance)

  NODE_SOCKET_API(bool, use_pixel_jitter);
  /* Neural Rendering: final render keeps the pixel filter instead of one jitter for all samples. */
  /* Neural Rendering glass samples: 0 = off, otherwise the samples every pixel gets before the
   * pixels that saw no glass stop (the glass pixels go on to the render's samples). */
  NODE_SOCKET_API(int, nr_glass_samples);
  NODE_SOCKET_API(bool, use_custom_pixel_jitter_sample);
  NODE_SOCKET_API_ARRAY(array<float>, custom_pixel_jitter_sample);
  HaltonSequence pixel_jitter_state;
  int pixel_jitter_frame = 0;

  NODE_SOCKET_API(bool, use_denoise);
  NODE_SOCKET_API(DenoiserType, denoiser_type);
  NODE_SOCKET_API(int, denoise_start_sample);
  NODE_SOCKET_API(DenoiserPassMask, denoiser_passes);
  NODE_SOCKET_API(DenoiserPrefilter, denoiser_prefilter);
  NODE_SOCKET_API(bool, denoise_use_gpu);
  NODE_SOCKET_API(DenoiserQuality, denoiser_quality);
  NODE_SOCKET_API(float, denoiser_upscale_factor);
  NODE_SOCKET_API(int, denoiser_dlss_preset);

  enum : uint32_t {
    AO_PASS_MODIFIED = (1 << 0),
    OBJECT_MANAGER = (1 << 1),

    /* tag everything in the manager for an update */
    UPDATE_ALL = ~0u,

    UPDATE_NONE = 0u,
  };

  bool shadow_catcher_needs_recalc_ = true;
  bool scene_has_shadow_catcher_ = false;

  Integrator();
  ~Integrator() override;

  void device_update(Device *device, DeviceScene *dscene, Scene *scene);
  /* Falcon: only the next pixel jitter and the seed it scrambles (Scene::update_camera_resolution()). */
  void device_update_pixel_jitter(DeviceScene *dscene);
  void device_free(Device *device, DeviceScene *dscene, bool force_free = false);

  void tag_update(Scene *scene, const uint32_t flag);

  uint64_t get_kernel_features() const;

  AdaptiveSampling get_adaptive_sampling() const;
  DenoiseParams get_denoise_params() const;
  GuidingParams get_guiding_params(const Device *device) const;
  BidirectionalParams get_bidirectional_params() const;

  bool is_modified() const;
  void clear_modified();
};

CCL_NAMESPACE_END
