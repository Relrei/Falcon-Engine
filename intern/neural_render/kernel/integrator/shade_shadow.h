/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/guiding.h"
#include "kernel/integrator/shade_volume.h"
#include "kernel/integrator/surface_shader.h"
#include "kernel/integrator/volume_stack.h"

#include "kernel/geom/shader_data.h"

CCL_NAMESPACE_BEGIN

enum TransparentShadowEvalResult {
  TRANSPARENT_SHADOW_EVAL_CONTINUE = 0,
  TRANSPARENT_SHADOW_EVAL_OPAQUE = 1,
  TRANSPARENT_SHADOW_EVAL_CACHE_MISS = 2,
};

#ifdef __KERNEL_GPU__
/* Assume we can pack num hits into 12 bits, so that we can also store resume hits
 * and skip volume in the 16 bit num_hits. */
#  define SHADOW_HIT_COUNT_BITS_GPU 12
#  define SHADOW_HIT_COUNT_MASK_GPU ((1 << SHADOW_HIT_COUNT_BITS_GPU) - 1)
#  define SHADOW_RESUME_BITS_GPU 3
#  define SHADOW_RESUME_MASK_GPU ((1 << SHADOW_RESUME_BITS_GPU) - 1)

/* +1 is for the extra loop iteration for the final volume segment. */
static_assert(INTEGRATOR_SHADOW_ISECT_SIZE_GPU + 1 < (1 << SHADOW_RESUME_BITS_GPU),
              "INTEGRATOR_SHADOW_ISECT_SIZE_GPU too large for resume_hit bits");
#endif

ccl_device_inline uint shadow_num_hits_get(const uint packed_num_hits)
{
#ifdef __KERNEL_GPU__
  return packed_num_hits & SHADOW_HIT_COUNT_MASK_GPU;
#else
  return packed_num_hits;
#endif
}

ccl_device_inline uint shadow_resume_hit_get(const uint packed_num_hits)
{
#ifdef __KERNEL_GPU__
  return (packed_num_hits >> SHADOW_HIT_COUNT_BITS_GPU) & SHADOW_RESUME_MASK_GPU;
#else
  (void)packed_num_hits;
  return 0;
#endif
}

ccl_device_inline bool shadow_surface_cache_miss_get(const uint packed_num_hits)
{
#ifdef __KERNEL_GPU__
  return (packed_num_hits >> (SHADOW_HIT_COUNT_BITS_GPU + SHADOW_RESUME_BITS_GPU)) & 1;
#else
  (void)packed_num_hits;
  return false;
#endif
}

ccl_device_inline uint shadow_num_hits_pack(const uint num_hits,
                                            const uint resume_hit,
                                            const bool surface_hit)
{
#ifdef __KERNEL_GPU__
  return (num_hits & SHADOW_HIT_COUNT_MASK_GPU) |
         ((resume_hit & SHADOW_RESUME_MASK_GPU) << SHADOW_HIT_COUNT_BITS_GPU) |
         ((surface_hit ? 1u : 0u) << (SHADOW_HIT_COUNT_BITS_GPU + SHADOW_RESUME_BITS_GPU));
#else
  /* Cache miss resume not supported on CPU. */
  kernel_assert(resume_hit == 0 && !surface_hit);
  (void)resume_hit;
  (void)surface_hit;
  return num_hits;
#endif
}

ccl_device_inline bool shadow_intersections_has_remaining(const uint packed_num_hits)
{
  return shadow_num_hits_get(packed_num_hits) >= INTEGRATOR_SHADOW_ISECT_SIZE;
}

#ifdef __TRANSPARENT_SHADOWS__
ccl_device_inline float fast_caustics_smin(const float a, const float b, const float k)
{
  const float kk = max(k, 1e-4f);
  const float h = clamp(0.5f + 0.5f * (b - a) / kk, 0.0f, 1.0f);
  return mix(b, a, h) - kk * h * (1.0f - h);
}

ccl_device_inline float fast_caustics_smoothstep(const float edge0,
                                                 const float edge1,
                                                 const float x)
{
  const float t = clamp((x - edge0) / max(edge1 - edge0, 1e-5f), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/* Falcon Cycles-expansion: fast analytic fake dispersion caustics for shadow rays passing through
 * refractive/glass closures. Combines a 6-wavelength spectral caustic cusp profile with a
 * distance-based convex-lens Jacobian (1 / sqrt(J^2 + eps^2) + polynomial smin cap + smoothstep
 * focal inversion) so curved glass and water cast crisp folded caustic lines that converge and
 * smoothly invert across the focal distance in both viewport and final renders (GPU and CPU). */
ccl_device_inline float3 fast_caustics_spectral_eval(const float cos_theta,
                                                     const float hit_dist,
                                                     const float3 P,
                                                     const float3 N,
                                                     const float3 N_smooth,
                                                     const float ior,
                                                     const float disp,
                                                     const float focus,
                                                     const float sharpness,
                                                     const int folds,
                                                     const float ripple,
                                                     const float shadow,
                                                     const float focal_distance,
                                                     const float lens_power,
                                                     const float lens_smooth,
                                                     const float reach)
{
  const float F_base = fresnel_dielectric_cos(cos_theta, ior);
  const float T_base = 1.0f - F_base;

  /* Gentle distance attenuation scaled by user-configurable reach so even glancing/light hits
   * stretch cleanly across long floor distances instead of dying out abruptly. */
  const float dist = max(hit_dist, 0.0f);
  const float norm_dist = dist / max(reach, 0.05f);
  const float dist_atten = 1.0f / (1.0f + 0.35f * norm_dist + 0.12f * norm_dist * norm_dist);
  const float dist_blur = norm_dist * 0.04f;

  /* Fold signature from the closure normal vs the smooth shading normal: normal maps and bump
   * turn plain concentric rings into folded light ribbons. Not against the geometric normal: with
   * Shade Smooth that difference jumps at every face edge and printed the polygons into the
   * caustics (Shade Smooth did not smooth what FFC drew). */
  const float facet_diff = len(N - N_smooth);
  const float facet_fold = clamp(facet_diff * 0.85f, 0.0f, 0.38f);
  const float ribbon_edge = 1.0f + clamp(facet_diff * 1.4f, 0.0f, 1.2f) * dist_atten;

  /* Smooth deterministic 3-wave interference for water / organic caustic webs when ripple > 0. */
  float ripple_shift = 0.0f;
  float ripple_gain = 1.0f;
  if (ripple > 0.0f) {
    const float3 q = P * 4.5f + N * 1.8f;
    const float w1 = sinf(q.x * 1.7f + q.z * 1.3f);
    const float w2 = sinf(q.y * 1.9f - q.x * 1.1f + 1.2f);
    const float w3 = sinf((q.x + q.y + q.z) * 1.4f + 2.4f);
    const float web = (w1 + w2 + w3) * (1.0f / 3.0f);
    ripple_shift = web * 0.14f * ripple;
    const float crest = powf(clamp(1.0f - fabsf(web), 0.0f, 1.0f), 2.5f);
    ripple_gain = mix(1.0f, 0.55f + 1.45f * crest, clamp(ripple, 0.0f, 1.0f));
  }

  /* Soft Fresnel base transmission with gentle grazing-angle sensitivity so light hits stretch far. */
  const float shadow_atten = clamp(1.0f - shadow, 0.0f, 1.0f);
  const float grazing_boost = powf(max(cos_theta, 0.06f), 0.55f);
  const float base_fill = 0.64f * shadow_atten * grazing_boost * (0.25f + 0.75f * T_base) *
                          (0.45f + 0.55f * dist_atten) * ripple_gain;

  /* Snell's law deflection: focal caustic band shifts and softens with hit distance. */
  const float sin_i = sqrtf(max(1.0f - cos_theta * cos_theta, 0.0f));
  const float base_width = mix(0.11f, 0.022f, clamp(focus * sharpness / 2.2f, 0.0f, 1.0f)) +
                           dist_blur;
  const float line_gain = (1.35f + 1.35f * focus * sharpness) * dist_atten * ribbon_edge;
  const int num_folds = clamp(folds, 1, 8);
  const float eps = max(lens_smooth, 0.015f);

  const float offsets[6] = {-1.0f, -0.6f, -0.2f, 0.2f, 0.6f, 1.0f};
  const float3 colors[6] = {
      make_float3(0.42f, 0.02f, 0.00f),
      make_float3(0.34f, 0.18f, 0.00f),
      make_float3(0.20f, 0.34f, 0.02f),
      make_float3(0.04f, 0.36f, 0.14f),
      make_float3(0.00f, 0.10f, 0.38f),
      make_float3(0.00f, 0.00f, 0.46f),
  };

  float3 accum = make_float3(base_fill, base_fill, base_fill);
  for (int k = 0; k < 6; k++) {
    const float w = offsets[k];
    const float ior_k = max(1.01f, ior + w * disp * 0.08f);
    const float T_k = 1.0f - fresnel_dielectric_cos(cos_theta, ior_k);
    const float sin_t = sin_i / ior_k;

    /* Distance-based convex-lens Jacobian (J = 1 - d / f_k) with SDF-style smooth reversal.
     * Focal distance is independent of surface/caustics IOR (only shifted gently by dispersion). */
    float lens_gain = 1.0f;
    float lens_shift = 0.0f;
    if (focal_distance > 0.0f && lens_power > 0.0f) {
      const float f_k = max(focal_distance * (1.0f - w * clamp(disp, 0.0f, 3.0f) * 0.06f), 0.01f);
      const float ramp = fast_caustics_smoothstep(0.0f, f_k * 0.35f, dist);
      const float J = 1.0f - (dist / f_k) * (0.45f + 0.55f * cos_theta);
      const float smooth_j = sqrtf(J * J + eps * eps);
      const float raw_gain = 1.0f / smooth_j;
      const float cap = 2.0f + 3.0f * lens_power;
      const float capped_gain = fast_caustics_smin(raw_gain, cap, 0.4f + 0.8f * eps);
      const float inv_blend = fast_caustics_smoothstep(-eps * 1.5f, eps * 1.5f, J);
      lens_gain = mix(1.0f, 1.0f + max(capped_gain - 1.0f, 0.0f) * lens_power * 0.45f, ramp);
      lens_shift = ramp * (2.0f * inv_blend - 1.0f) * tanhf(dist / f_k) * 0.12f *
                   min(lens_power, 4.0f);
    }

    /* Optical deflection parameter in [0, 1] with curvature/facet fold & focal inversion. */
    const float deflect = clamp((sin_i - sin_t) * (1.25f + 0.35f * min(norm_dist, 3.0f)) +
                                    facet_fold + ripple_shift + lens_shift,
                                0.0f,
                                1.0f);
    const float target_base = clamp(0.24f + 0.16f * (ior_k - 1.45f) + w * disp * 0.065f,
                                    0.06f,
                                    0.78f);
    float energy = 0.0f;
    for (int f = 0; f < num_folds; f++) {
      const float fold_step = float(f) * 0.12f;
      const float fold_decay = 1.0f / (1.0f + 0.65f * float(f));
      const float fold_width = base_width * (1.0f + 0.40f * float(f));
      const float delta = deflect - (target_base + fold_step);
      const float d = delta / max(fold_width, 1e-3f);
      /* Crisp overlapping cusp-edge profile (10.0 base sharpness, built-in without extra UI):
       * sharp bright rim line + smooth translucent ribbon tail. */
      const float cusp_core = expf(-10.0f * d * d) * 0.85f;
      const float cusp_line = (expf(-d * d) + cusp_core) * line_gain * fold_decay;
      const float inner_tail = (f == 0 && delta < 0.0f) ?
                                   expf(delta / max(fold_width * 3.5f, 1e-3f)) * 0.32f *
                                       dist_atten :
                                   0.0f;
      energy += cusp_line + inner_tail;
    }
    accum += colors[k] * (energy * T_k * ripple_gain * lens_gain);
  }
  return max(accum, zero_float3());
}

ccl_device_inline Spectrum fast_caustics_shadow_transparency(KernelGlobals kg,
                                                             const ccl_private ShaderData *sd,
                                                             const float hit_dist,
                                                             const KernelShader kshader)
{
  Spectrum result = surface_shader_transparency(sd);
  const float strength = kshader.fast_caustics_strength;
  if (!(strength > 0.0f)) {
    return result;
  }
  const float disp = kshader.fast_caustics_dispersion;
  const float focus = kshader.fast_caustics_focus;
  const int folds = clamp((kshader.fast_caustics_flags >> 8) & 0xff, 1, 8);
  const float ripple = kshader.fast_caustics_ripple;
  const float shadow = kshader.fast_caustics_shadow;
  const float focal_distance = kshader.fast_caustics_focal_distance;
  const float lens_power = kshader.fast_caustics_lens_power;
  const float lens_smooth = kshader.fast_caustics_lens_smooth;
  const float reach = kshader.fast_caustics_reach;
  const float ior = clamp(kshader.fast_caustics_ior, 1.01f, 10.0f);
  const bool backfacing = (sd->runtime_flag & SR_BACKFACING) != 0;

  for (int i = 0; i < sd->num_closure; i++) {
    const ccl_private ShaderClosure *sc = &sd->closure[i];
    if (!(CLOSURE_IS_GLASS(sc->type) || CLOSURE_IS_BSDF_TRANSMISSION(sc->type))) {
      continue;
    }
    const ccl_private MicrofacetBsdf *bsdf = (const ccl_private MicrofacetBsdf *)sc;
    const float rough = clamp(sqrtf(max(bsdf->alpha_x * bsdf->alpha_y, 0.0f)), 0.0f, 1.0f);
    const float sharpness = 1.0f / (1.0f + 8.0f * rough);
    const float cos_theta = clamp(fabsf(dot(sc->N, sd->wi)), 0.0f, 1.0f);

    if (backfacing) {
      /* On the exit face of a closed glass mesh, pass through cleanly with subtle secondary
       * facet-edge ribbon modulation so inner walls also contribute overlapping caustic sheets. */
      const float F = fresnel_dielectric_cos(cos_theta, ior);
      const float exit_fold = clamp(len(sc->N - sd->N) * 0.35f, 0.0f, 0.25f);
      result += sc->weight * clamp(1.0f - 0.25f * F + exit_fold, 0.35f, 1.25f);
    }
    else {
      const float3 rgb = fast_caustics_spectral_eval(cos_theta,
                                                     hit_dist,
                                                     sd->P,
                                                     sc->N,
                                                     sd->N,
                                                     ior,
                                                     disp,
                                                     focus,
                                                     sharpness,
                                                     folds,
                                                     ripple,
                                                     shadow,
                                                     focal_distance,
                                                     lens_power,
                                                     lens_smooth,
                                                     reach) *
                         strength;
      result += sc->weight * rgb_to_spectrum(rgb);
    }
  }
  return result;
}

ccl_device_inline TransparentShadowEvalResult integrate_transparent_surface_shadow(
    KernelGlobals kg, IntegratorShadowState state, const int num_hits, const int hit)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_SURFACE);

  /* Setup shader data at surface. */
  Intersection isect ccl_optional_struct_init;
  integrator_state_read_shadow_isect(state, &isect, hit);

  Ray ray ccl_optional_struct_init;
  integrator_state_read_shadow_ray(state, &ray);

  Spectrum transparency = zero_spectrum();

  if (kernel_data.integrator.use_fast_caustics) {
    ShaderData full_sd;
    shader_setup_from_ray(kg, &full_sd, &ray, &isect);

    const KernelShader kshader = kernel_data_fetch(shaders, full_sd.shader & SHADER_MASK);
    const bool mat_fast_caustics = (kshader.fast_caustics_flags & 1) != 0;

    if (!(full_sd.shader_flag & SD_HAS_ONLY_VOLUME)) {
      if (mat_fast_caustics) {
        surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW | KERNEL_FEATURE_NODE_BSDF>(
            kg, state, &full_sd, nullptr, PATH_RAY_VISIBILITY_DIFFUSE, PATH_RAY_FLAG_NONE);
      }
      else {
        surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW>(
            kg, state, &full_sd, nullptr, PATH_RAY_VISIBILITY_SHADOW, PATH_RAY_FLAG_NONE);
      }
      if (full_sd.runtime_flag & SR_CACHE_MISS) {
        INTEGRATOR_STATE_WRITE(state, shadow_path, packed_num_hits) = shadow_num_hits_pack(
            num_hits, hit, true);
        return TRANSPARENT_SHADOW_EVAL_CACHE_MISS;
      }
    }
    else {
      INTEGRATOR_STATE_WRITE(state, shadow_path, volume_bounds_bounce) += 1;
      if (INTEGRATOR_STATE(state, shadow_path, volume_bounds_bounce) > VOLUME_BOUNDS_MAX) {
        return TRANSPARENT_SHADOW_EVAL_OPAQUE;
      }
    }

#  ifdef __VOLUME__
    volume_stack_enter_exit<true>(kg, state, &full_sd);
#  endif

    if (full_sd.runtime_flag & SR_RAY_PORTAL) {
      return TRANSPARENT_SHADOW_EVAL_OPAQUE;
    }

    transparency = mat_fast_caustics ?
                       fast_caustics_shadow_transparency(kg, &full_sd, isect.t, kshader) :
                       surface_shader_transparency(&full_sd);
  }
  else {
    ShaderDataTinyStorage shadow_sd_storage;
    ccl_private ShaderData *shadow_sd = AS_SHADER_DATA(&shadow_sd_storage);

    shader_setup_from_ray(kg, shadow_sd, &ray, &isect);

    /* Evaluate shader. */
    if (!(shadow_sd->shader_flag & SD_HAS_ONLY_VOLUME)) {
      surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW>(
          kg, state, shadow_sd, nullptr, PATH_RAY_VISIBILITY_SHADOW, PATH_RAY_FLAG_NONE);
      if (shadow_sd->runtime_flag & SR_CACHE_MISS) {
        /* Store resume state at current hit. */
        INTEGRATOR_STATE_WRITE(state, shadow_path, packed_num_hits) = shadow_num_hits_pack(
            num_hits, hit, true);
        return TRANSPARENT_SHADOW_EVAL_CACHE_MISS;
      }
    }
    else {
      INTEGRATOR_STATE_WRITE(state, shadow_path, volume_bounds_bounce) += 1;
      if (INTEGRATOR_STATE(state, shadow_path, volume_bounds_bounce) > VOLUME_BOUNDS_MAX) {
        return TRANSPARENT_SHADOW_EVAL_OPAQUE;
      }
    }

#  ifdef __VOLUME__
    /* Exit/enter volume. */
    volume_stack_enter_exit<true>(kg, state, shadow_sd);
#  endif

    /* Disable transparent shadows for ray portals */
    if (shadow_sd->runtime_flag & SR_RAY_PORTAL) {
      return TRANSPARENT_SHADOW_EVAL_OPAQUE;
    }

    /* Compute transparency from closures. */
    transparency = surface_shader_transparency(shadow_sd);
  }

  const Spectrum throughput = INTEGRATOR_STATE(state, shadow_path, throughput) * transparency;

  if (is_zero(throughput)) {
    return TRANSPARENT_SHADOW_EVAL_OPAQUE;
  }

  INTEGRATOR_STATE_WRITE(state, shadow_path, throughput) = throughput;
  INTEGRATOR_STATE_WRITE(state, shadow_path, transparent_bounce) += 1;
  INTEGRATOR_STATE_WRITE(state, shadow_path, rng_offset) += PRNG_BOUNCE_NUM;

  return TRANSPARENT_SHADOW_EVAL_CONTINUE;
}

#  ifdef __VOLUME__
ccl_device_inline TransparentShadowEvalResult
integrate_transparent_volume_shadow(KernelGlobals kg,
                                    IntegratorShadowState state,
                                    const int num_hits,
                                    const int hit,
                                    const int num_recorded_hits,
                                    ccl_private Spectrum *ccl_restrict throughput)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_VOLUME);

  /* TODO: deduplicate with surface, or does it not matter for memory usage? */
  ShaderDataTinyStorage shadow_sd_storage;
  ccl_private ShaderData *shadow_sd = AS_SHADER_DATA(&shadow_sd_storage);

  /* Setup shader data. */
  Ray ray ccl_optional_struct_init;
  integrator_state_read_shadow_ray(state, &ray);
  ray.self.object = OBJECT_NONE;
  ray.self.prim = PRIM_NONE;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  /* Modify ray position and length to match current segment. */
  ray.tmin = (hit == 0) ? ray.tmin : INTEGRATOR_STATE_ARRAY(state, shadow_isect, hit - 1, t);
  ray.tmax = (hit < num_recorded_hits) ? INTEGRATOR_STATE_ARRAY(state, shadow_isect, hit, t) :
                                         ray.tmax;

  /* `object` is only needed for light tree with light linking, it is irrelevant for shadow. */
  shader_setup_from_volume(shadow_sd, &ray, OBJECT_NONE);

  if (kernel_data.integrator.volume_ray_marching) {
    const float step_size = volume_stack_step_size<true>(kg, state);
    volume_shadow_ray_marching(kg, state, &ray, shadow_sd, throughput, step_size);
  }
  else {
    volume_shadow_null_scattering(kg, state, &ray, shadow_sd, throughput);
  }

  if (is_zero(*throughput)) {
    return TRANSPARENT_SHADOW_EVAL_OPAQUE;
  }

  if (shadow_sd->runtime_flag & SR_CACHE_MISS) {
    /* Store resume state at current hit. */
    INTEGRATOR_STATE_WRITE(state, shadow_path, packed_num_hits) = shadow_num_hits_pack(
        num_hits, hit, false);
    return TRANSPARENT_SHADOW_EVAL_CACHE_MISS;
  }

  return TRANSPARENT_SHADOW_EVAL_CONTINUE;
}
#  endif

ccl_device_inline TransparentShadowEvalResult integrate_transparent_shadow(
    KernelGlobals kg, IntegratorShadowState state, const uint packed_num_hits)
{
  /* Accumulate shadow for transparent surfaces. */
  const uint num_hits = shadow_num_hits_get(packed_num_hits);
  const uint num_recorded_hits = min(num_hits, (uint)INTEGRATOR_SHADOW_ISECT_SIZE);

  /* Resume state from previous cache miss. */
  const uint resume_hit = shadow_resume_hit_get(packed_num_hits);
  const bool cache_miss_surface = shadow_surface_cache_miss_get(packed_num_hits);

  /* Plus one to account for world volume, which has no boundary to hit but casts shadows. */
  for (uint hit = resume_hit; hit < num_recorded_hits + 1; hit++) {
    /* Skip volume if resuming after volume completed but surface had cache miss. */
    const bool skip_volume = (hit == resume_hit) && cache_miss_surface;

    /* Volume shaders. */
    if (!skip_volume &&
        (hit < num_recorded_hits || !shadow_intersections_has_remaining(packed_num_hits)))
    {
#  ifdef __VOLUME__
      if (!integrator_state_shadow_volume_stack_is_empty(kg, state)) {
        Spectrum throughput = INTEGRATOR_STATE(state, shadow_path, throughput);
        const TransparentShadowEvalResult result = integrate_transparent_volume_shadow(
            kg, state, num_hits, hit, num_recorded_hits, &throughput);
        if (result != TRANSPARENT_SHADOW_EVAL_CONTINUE) {
          return result;
        }

        INTEGRATOR_STATE_WRITE(state, shadow_path, throughput) = throughput;
      }
#  endif
    }

    /* Surface shaders. */
    if (hit < num_recorded_hits) {
      const TransparentShadowEvalResult result = integrate_transparent_surface_shadow(
          kg, state, num_hits, hit);
      if (result != TRANSPARENT_SHADOW_EVAL_CONTINUE) {
        return result;
      }
    }
    /* Note we do not need to check max_transparent_bounce here, the number
     * of intersections is already limited and made opaque in the
     * INTERSECT_SHADOW kernel. */
  }

  if (shadow_intersections_has_remaining(packed_num_hits)) {
    /* There are more hits that we could not recorded due to memory usage,
     * adjust ray to intersect again from the last hit. */
    const float last_hit_t = INTEGRATOR_STATE_ARRAY(state, shadow_isect, num_recorded_hits - 1, t);
    INTEGRATOR_STATE_WRITE(state, shadow_ray, tmin) = intersection_t_offset(last_hit_t);
  }

  return TRANSPARENT_SHADOW_EVAL_CONTINUE;
}
#endif /* __TRANSPARENT_SHADOWS__ */

ccl_device void integrator_shade_shadow(KernelGlobals kg,
                                        IntegratorShadowState state,
                                        ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_SETUP);
  const uint packed_num_hits = INTEGRATOR_STATE(state, shadow_path, packed_num_hits);

#ifdef __TRANSPARENT_SHADOWS__
  /* Evaluate transparent shadows. */
  const TransparentShadowEvalResult result = integrate_transparent_shadow(
      kg, state, packed_num_hits);
  if (result == TRANSPARENT_SHADOW_EVAL_CACHE_MISS) {
    integrator_shadow_path_cache_miss(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
    return;
  }
  if (result == TRANSPARENT_SHADOW_EVAL_OPAQUE) {
    integrator_shadow_path_terminate(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
    return;
  }

  kernel_assert(result == TRANSPARENT_SHADOW_EVAL_CONTINUE);
#endif

  if (shadow_intersections_has_remaining(packed_num_hits)) {
    /* More intersections to find, continue shadow ray. */
    integrator_shadow_path_next(
        state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW);
    return;
  }

  guiding_record_direct_light(kg, state);
  film_write_direct_light(kg, state, render_buffer);
  integrator_shadow_path_terminate(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
}

CCL_NAMESPACE_END
