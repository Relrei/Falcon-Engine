/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/path_state.h"
#include "kernel/integrator/surface_shader.h"

#include "kernel/film/adaptive_sampling.h"
#include "kernel/film/data_passes.h"
#include "kernel/film/denoising_passes.h"
#include "kernel/film/light_passes.h"

#include "kernel/light/sample.h"

#include "kernel/geom/motion_triangle.h"
#include "kernel/geom/triangle.h"

#include "kernel/integrator/guiding.h"
#include "kernel/integrator/init_from_light.h"
#include "kernel/integrator/light_path_start.h"
#include "kernel/integrator/photon_map.h"
#include "kernel/integrator/shadow_linking.h"
#include "kernel/integrator/subsurface.h"
#include "kernel/integrator/volume_stack.h"

#include "kernel/types.h"
#include "util/math_intersect.h"

CCL_NAMESPACE_BEGIN

/* Neural Rendering glass samples (kernel_data.integrator.nr_glass_samples, 0 = off; from v4's
 * falcon_glass_samples): during the samples every pixel gets, a camera ray whose first surface has
 * glass closures with at least a quarter of the BSDF sample weight marks its pixel to go on to the
 * glass samples. Rough glass counts too (the Junk Shop jars: 0.7-0.98 of the weight is glass). */
ccl_device_forceinline void integrate_surface_nr_glass_samples_mark(
    KernelGlobals kg,
    ConstIntegratorState state,
    const ccl_private ShaderData *sd,
    ccl_global float *ccl_restrict render_buffer)
{
  if (kernel_data.integrator.nr_glass_samples == 0 ||
      kernel_data.film.pass_adaptive_aux_buffer == PASS_UNUSED ||
      INTEGRATOR_STATE(state, path, bounce) != 0 ||
      int(INTEGRATOR_STATE(state, path, sample)) >= kernel_data.integrator.nr_glass_samples)
  {
    return;
  }
  float sum = 0.0f;
  float glass = 0.0f;
  for (int i = 0; i < sd->num_closure; i++) {
    const ccl_private ShaderClosure *c = &sd->closure[i];
    if (!CLOSURE_IS_BSDF(c->type)) {
      continue;
    }
    sum += c->sample_weight;
    if ((c->type >= CLOSURE_BSDF_MICROFACET_BECKMANN_REFRACTION_ID &&
         c->type <= CLOSURE_BSDF_THIN_GLASS_TRANSMISSION_ID) ||
        (c->type >= CLOSURE_BSDF_MICROFACET_BECKMANN_GLASS_ID &&
         c->type <= CLOSURE_BSDF_MICROFACET_MULTI_GGX_GLASS_ID))
    {
      glass += c->sample_weight;
    }
  }
  if (glass > 0.0f && glass >= 0.25f * sum) {
    film_nr_glass_samples_mark(kg, state, render_buffer);
  }
}

ccl_device_forceinline void integrate_surface_shader_setup(KernelGlobals kg,
                                                           ConstIntegratorState state,
                                                           ccl_private ShaderData *sd)
{
  Intersection isect ccl_optional_struct_init;
  integrator_state_read_isect(state, &isect);

  Ray ray ccl_optional_struct_init;
  integrator_state_read_ray(state, &ray);

  shader_setup_from_ray(kg, sd, &ray, &isect);

#ifdef __SPECTRAL__
  shader_setup_wavelength(kg, sd, state);
#endif
}

ccl_device_forceinline float3 integrate_surface_ray_offset(KernelGlobals kg,
                                                           const ccl_private ShaderData *sd,
                                                           const float3 ray_P,
                                                           const float3 ray_D)
{
  /* No ray offset needed for other primitive types. */
  if (!(sd->type & PRIMITIVE_TRIANGLE)) {
    return ray_P;
  }

  /* Self intersection tests already account for the case where a ray hits the
   * same primitive. However precision issues can still cause neighboring
   * triangles to be hit. Here we test if the ray-triangle intersection with
   * the same primitive would miss, implying that a neighboring triangle would
   * be hit instead.
   *
   * This relies on triangle intersection to be watertight, and the object inverse
   * object transform to match the one used by ray intersection exactly.
   *
   * Potential improvements:
   * - It appears this happens when either barycentric coordinates are small,
   *   or dot(sd->Ng, ray_D)  is small. Detect such cases and skip test?
   * - Instead of ray offset, can we tweak P to lie within the triangle?
   */

  /* TODO: Investigate if there are better ray offsetting algorithms for each BVH.
   * Cycles and Custom BVH triangle tests aren't numerically identical, meaning
   * this method isn't ideal for them. */

  float3 verts[3];
  if (sd->type == PRIMITIVE_TRIANGLE) {
    triangle_vertices(kg, sd->object, sd->prim, verts);
  }
  else {
    kernel_assert(sd->type == PRIMITIVE_MOTION_TRIANGLE);
    motion_triangle_vertices(kg, sd->object, sd->prim, sd->time, verts);
  }

  float3 local_ray_P = ray_P;
  float3 local_ray_D = ray_D;

  if (!(sd->object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
    const Transform itfm = object_get_inverse_transform(kg, sd);
    local_ray_P = transform_point(&itfm, local_ray_P);
    local_ray_D = transform_direction(&itfm, local_ray_D);
  }

  if (ray_triangle_intersect_self(local_ray_P, local_ray_D, verts)) {
    return ray_P;
  }
  return ray_offset(ray_P, sd->Ng);
}

ccl_device_forceinline bool integrate_surface_holdout(KernelGlobals kg,
                                                      ConstIntegratorState state,
                                                      ccl_private ShaderData *sd,
                                                      ccl_global float *ccl_restrict render_buffer)
{
  /* Write holdout transparency to render buffer and stop if fully holdout. */
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (((sd->runtime_flag & SR_HOLDOUT) || (sd->object_flag & SD_OBJECT_HOLDOUT_MASK)) &&
      (path_flag & PATH_RAY_TRANSPARENT_BACKGROUND))
  {
    const Spectrum holdout_weight = surface_shader_apply_holdout(sd);
    const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
    const float transparent = average(holdout_weight * throughput);
    film_write_holdout(kg, state, path_flag, transparent, render_buffer);
    if (isequal(holdout_weight, one_spectrum())) {
      return false;
    }
  }

  return true;
}

template<bool nr_bidir>
ccl_device_forceinline void integrate_surface_emission(KernelGlobals kg,
                                                       IntegratorState state,
                                                       const ccl_private ShaderData *sd,
                                                       ccl_global float *ccl_restrict
                                                           render_buffer)
{
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

#ifdef __LIGHT_LINKING__
  if (!(path_visibility & PATH_RAY_VISIBILITY_CAMERA) &&
      !light_link_object_match(kg, light_link_receiver_forward(kg, state), sd->object))
  {
    return;
  }
#endif

#ifdef __SHADOW_LINKING__
  /* Indirect emission of shadow-linked emissive surfaces is done via shadow rays to dedicated
   * light sources. */
  if (kernel_data.kernel_features & KERNEL_FEATURE_SHADOW_LINKING) {
    if (!(path_visibility & PATH_RAY_VISIBILITY_CAMERA) &&
        kernel_data_fetch(objects, sd->object).shadow_set_membership != LIGHT_LINK_MASK_ALL)
    {
      return;
    }
  }
#endif

  /* Falcon Cycles-expansion: the caustics of an emissive triangle the light paths start from (a side
   * its light sampling uses, kernel/integrator/light_path_start.h light_path_mesh_sample()) are
   * theirs. */
  if constexpr (nr_bidir) {
    if ((sd->type & PRIMITIVE_TRIANGLE) &&
        (sd->shader_flag & ((sd->runtime_flag & SR_BACKFACING) ? SD_MIS_BACK : SD_MIS_FRONT)) &&
        light_trace_camera_path_skips_light(
            kg, path_flag, INTEGRATOR_STATE(state, path, nr_chain), LIGHT_TRIANGLE))
    {
      return;
    }
  }

  /* Evaluate emissive closure. */
  const Spectrum L = surface_shader_emission(sd);

  const float mis_weight = light_sample_mis_weight_forward_surface(
      kg, state, path_visibility, path_flag, sd);

  guiding_record_surface_emission(kg, state, L, mis_weight);
  film_write_surface_emission(
      kg, state, L, mis_weight, render_buffer, object_lightgroup(kg, sd->object));
}

ccl_device int integrate_surface_ray_portal(KernelGlobals kg,
                                            IntegratorState state,
                                            ccl_private ShaderData *sd,
                                            const ccl_private ShaderClosure *sc)
{
  const ccl_private RayPortalClosure *pc = (const ccl_private RayPortalClosure *)sc;

  float sum_sample_weight = 0.0f;
  for (int i = 0; i < sd->num_closure; i++) {
    const ccl_private ShaderClosure *sc = &sd->closure[i];

    if (CLOSURE_IS_BSDF_OR_BSSRDF(sc->type)) {
      sum_sample_weight += sc->sample_weight;
    }
  }
  if (sum_sample_weight <= 0.0f) {
    return LABEL_NONE;
  }

  if (len_squared(sd->P - pc->P) > 1e-9f) {
    /* if the ray origin is changed, unset the current object,
     * so we can potentially hit the same polygon again */
    INTEGRATOR_STATE_WRITE(state, isect, object) = OBJECT_NONE;
    INTEGRATOR_STATE_WRITE(state, ray, P) = pc->P;
  }
  else {
    INTEGRATOR_STATE_WRITE(state, ray, P) = integrate_surface_ray_offset(kg, sd, pc->P, pc->D);
  }
  INTEGRATOR_STATE_WRITE(state, ray, D) = pc->D;
  INTEGRATOR_STATE_WRITE(state, ray, tmin) = 0.0f;
  INTEGRATOR_STATE_WRITE(state, ray, tmax) = FLT_MAX;
#ifdef __RAY_DIFFERENTIALS__
  INTEGRATOR_STATE_WRITE(state, ray, dP) = differential_make_compact(sd->dP);
#endif

  const float pick_pdf = pc->sample_weight / sum_sample_weight;
  INTEGRATOR_STATE_WRITE(state, path, throughput) *= pc->weight / pick_pdf;

  const int label = LABEL_TRANSMIT | LABEL_RAY_PORTAL;
  path_state_next(kg, state, label, sd->runtime_flag);

  return label;
}

/* Branch off a shadow path and initialize common part of it.
 * THe common is between the surface shading and configuration of a special shadow ray for the
 * shadow linking. */
ccl_device_inline IntegratorShadowState
integrate_direct_light_shadow_init_common(KernelGlobals kg,
                                          IntegratorState state,
                                          const ccl_private Ray *ccl_restrict ray,
                                          const Spectrum bsdf_spectrum,
                                          const int light_group,
                                          const int mnee_vertex_count,
                                          const bool constant_light_shader)
{
  const DeviceKernel next_kernel = (constant_light_shader) ?
                                       DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW :
                                       DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE;

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state;
#ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    /* Reuse shadow path that was already allocated by intersect_mnee. */
    shadow_state = integrator_state_get_mnee_shadow_state(state);
    integrator_shadow_path_next(
        shadow_state, DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING, next_kernel);
  }
  else
#endif
  {
    shadow_state = integrator_shadow_path_init(kg, state, next_kernel, false);
  }

#ifdef __VOLUME__
  /* Copy volume stack and enter/exit volume. */
  integrator_state_copy_volume_stack_to_shadow(kg, shadow_state, state);
#endif

  /* Write shadow ray and associated state to global memory. */
  integrator_state_write_shadow_ray(shadow_state, ray);
  integrator_state_write_shadow_ray_self(shadow_state, ray);

  /* Copy state from main path to shadow path. */
  const Spectrum unlit_throughput = INTEGRATOR_STATE(state, path, throughput);
  const Spectrum throughput = unlit_throughput * bsdf_spectrum;

  if (!(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_TREE)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bsdf_eval_average) = average(bsdf_spectrum);
  }

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, render_pixel_index) = INTEGRATOR_STATE(
      state, path, render_pixel_index);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_offset) = INTEGRATOR_STATE(
      state, path, rng_offset);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_pixel) = INTEGRATOR_STATE(
      state, path, rng_pixel);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, sample) = INTEGRATOR_STATE(
      state, path, sample);

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transparent_bounce) = INTEGRATOR_STATE(
      state, path, transparent_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, volume_bounds_bounce) = INTEGRATOR_STATE(
      state, path, volume_bounds_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, glossy_bounce) = INTEGRATOR_STATE(
      state, path, glossy_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, throughput) = throughput;

  if ((kernel_data.kernel_features & KERNEL_FEATURE_NODE_PORTAL)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, portal_bounce) = INTEGRATOR_STATE(
        state, path, portal_bounce);
  }

#ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transmission_bounce) =
        INTEGRATOR_STATE(state, path, transmission_bounce) + mnee_vertex_count - 1;
    INTEGRATOR_STATE_WRITE(shadow_state,
                           shadow_path,
                           diffuse_bounce) = INTEGRATOR_STATE(state, path, diffuse_bounce) + 1;
    INTEGRATOR_STATE_WRITE(shadow_state,
                           shadow_path,
                           bounce) = INTEGRATOR_STATE(state, path, bounce) + mnee_vertex_count;
  }
  else
#endif
  {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transmission_bounce) = INTEGRATOR_STATE(
        state, path, transmission_bounce);
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, diffuse_bounce) = INTEGRATOR_STATE(
        state, path, diffuse_bounce);
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bounce) = INTEGRATOR_STATE(
        state, path, bounce);
  }

  /* Write Light-group, +1 as light-group is int but we need to encode into a uint8_t. */
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, lightgroup) = light_group + 1;

#if defined(__PATH_GUIDING__)
  if ((kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, unlit_throughput) = unlit_throughput;
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, path_segment) = INTEGRATOR_STATE(
        state, guiding, path_segment);
    INTEGRATOR_STATE(shadow_state, shadow_path, guiding_light_linking_mis_weight) = 0.0f;
  }
#endif

  return shadow_state;
}

/* Path tracing: sample point on light and evaluate light shader, then
 * queue shadow ray to be traced. */
template<uint64_t node_feature_mask, bool nr_bidir>
#if defined(__KERNEL_HIP__)
/* Inlining the function makes gfx1102 crash rendering principled_bsdf_bevel_emission_137420.blend
 * using SDK 7.2.1. */
ccl_device_noinline
#elif defined(__KERNEL_GPU__)
ccl_device_forceinline
#else
/* MSVC has very long compilation time (x20) if we force inline this function */
ccl_device
#endif
    ShaderEvalResult integrate_surface_direct_light(KernelGlobals kg,
                                                    IntegratorState state,
                                                    ccl_private ShaderData *sd,
                                                    const ccl_private RNGState *rng_state)
{
  /* Test if there is a light or BSDF that needs direct light. */
  if (!(kernel_data.integrator.use_direct_light && (sd->runtime_flag & SR_BSDF_HAS_EVAL))) {
    return SHADER_EVAL_EMPTY;
  }

  LightSample ls ccl_optional_struct_init;
  int mnee_vertex_count = 0;  // NOLINT

#ifdef __MNEE__
  if ((kernel_data.kernel_features & KERNEL_FEATURE_MNEE) &&
      (INTEGRATOR_STATE(state, path, mnee) & PATH_MNEE_SAMPLED))
  {
    /* MNEE already sampled a light and caustics casters. */
    integrator_state_read_mnee(state, &ls, &mnee_vertex_count);
  }
  else
#endif
  {
    /* Sample position on a light. */
    const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
    const uint bounce = INTEGRATOR_STATE(state, path, bounce);
    const float3 rand_light = path_state_rng_3D(kg, rng_state, PRNG_LIGHT);

    if (!light_sample_from_position(kg,
                                    rand_light,
                                    sd->time,
                                    sd->P,
                                    sd->N,
                                    light_link_receiver_nee(kg, sd),
                                    sd->runtime_flag,
                                    bounce,
                                    path_flag,
                                    &ls))
    {
      return SHADER_EVAL_EMPTY;
    }
  }

  kernel_assert(ls.pdf != 0.0f);

  const bool is_transmission = dot(ls.D, sd->N) < 0.0f;

  if (ls.prim != PRIM_NONE && ls.prim == sd->prim && ls.object == sd->object) {
    /* Skip self intersection if light direction lies in the same hemisphere as the geometric
     * normal. */
    if (dot(ls.D, is_transmission ? -sd->Ng : sd->Ng) > 0.0f) {
      return SHADER_EVAL_EMPTY;
    }
  }

#ifdef __MNEE__
  /* On a caustic caster, a caustic light's contribution is delivered to receivers by
   * MNEE and does not need to be computed again here. */
  if (kernel_data.kernel_features & KERNEL_FEATURE_MNEE) {
    if (mnee_vertex_count == 0 && is_transmission &&
        (sd->object_flag & SD_OBJECT_CAUSTICS_CASTER) && ls.type != LIGHT_TRIANGLE &&
        kernel_data_fetch(lights, ls.prim).use_caustics)
    {
      return SHADER_EVAL_EMPTY;
    }
  }
#endif

  /* Evaluate constant part of light shader, rest will optionally be done in another kernel. */
  Spectrum light_shader_eval ccl_optional_struct_init;
  const bool is_constant_light_shader = light_sample_shader_eval_nee_constant(
      kg, ls.shader, ls.prim, ls.type != LIGHT_TRIANGLE, light_shader_eval);

  /* Evaluate BSDF. */
  BsdfEval bsdf_eval ccl_optional_struct_init;
  float avg_roughness_squared = 0.0f;
  const float bsdf_pdf = surface_shader_bsdf_eval(
      kg, state, sd, ls.D, &bsdf_eval, ls.shader, avg_roughness_squared);

  Ray ray ccl_optional_struct_init;

#ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    light_shader_eval *= integrator_state_read_mnee_throughput(state);
    bsdf_eval_mul(&bsdf_eval, light_shader_eval);

    if (bsdf_eval_is_zero(&bsdf_eval)) {
      return SHADER_EVAL_EMPTY;
    }

    integrator_state_read_mnee_ray(state, &ls, &ray);
  }
  else
#endif /* __MNEE__ */
  {
    const float mis_weight = light_sample_mis_weight_nee(kg, ls.pdf, bsdf_pdf);
    bsdf_eval_mul(&bsdf_eval, light_shader_eval * ls.eval_fac / ls.pdf * mis_weight);

    /* Falcon Cycles-expansion: in an open caustic chain, leave out the share of the nearly
     * specular closures (kernel/integrator/photon_map.h). */
    if constexpr (nr_bidir) {
      const uint32_t chain_flag = INTEGRATOR_STATE(state, path, flag);
      if ((chain_flag & PATH_RAY_NR_CAUSTIC_CHAIN) && !(chain_flag & PATH_RAY_NR_LIGHT_PATH) &&
          !(sd->shader_flag & SD_HAS_TRANSPARENT_SHADOW) && light_trace_light_supported(kg, ls.type) &&
          caustics_taken(kg, INTEGRATOR_STATE(state, path, nr_chain)) &&
          (ls.type != LIGHT_BACKGROUND ||
           light_trace_world_radiance_emitted(
               kg, light_path_world_radiance(kg, state, sd->P, ls.D))))
      {
        const float kept = photon_map_nee_kept_share(kg, sd, ls.D);
        if (!(kept > 0.0f)) {
          return SHADER_EVAL_EMPTY;
        }
        bsdf_eval_mul(&bsdf_eval, kept);
      }
    }

    /* Path termination for constant light shader. */
    if (is_constant_light_shader && !(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_TREE)) {
      const float terminate = path_state_rng_light_termination(kg, rng_state);
      if (light_sample_terminate(kg, &bsdf_eval, terminate)) {
        return SHADER_EVAL_EMPTY;
      }
    }
    /* For non-constant light shader, probabilistic termination happens in
     * SHADE_LIGHT_NEE when the full contribution is known. */
    else if (bsdf_eval_is_zero(&bsdf_eval)) {
      return SHADER_EVAL_EMPTY;
    }

    /* Create shadow ray. */
    light_sample_to_surface_shadow_ray(kg, sd, &ls, &ray);

#ifdef __RAY_DIFFERENTIALS__
    /* Widen ray differences, with same logic as forward sampling to ensure
     * both MIS strategies converge to the same result. */
    ray.dD = bsdf_widen_dD(kg, INTEGRATOR_STATE(state, ray, dD), avg_roughness_squared);
#endif
  }

  if (ray.self.object != OBJECT_NONE) {
    ray.P = integrate_surface_ray_offset(kg, sd, ray.P, ray.D);
  }

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrate_direct_light_shadow_init_common(
      kg,
      state,
      &ray,
      bsdf_eval_sum(&bsdf_eval),
      ls.group,
      mnee_vertex_count,
      is_constant_light_shader);

  if (is_transmission) {
#ifdef __VOLUME__
    volume_stack_enter_exit<true>(kg, shadow_state, sd);
#endif
  }

  uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag);

  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    PackedSpectrum pass_diffuse_weight;
    PackedSpectrum pass_glossy_weight;

    if (shadow_flag & PATH_RAY_ANY_PASS) {
      /* Indirect bounce, use weights from earlier surface or volume bounce. */
      pass_diffuse_weight = INTEGRATOR_STATE(state, path, pass_diffuse_weight);
      pass_glossy_weight = INTEGRATOR_STATE(state, path, pass_glossy_weight);
    }
    else {
      /* Direct light, use BSDFs at this bounce. */
      shadow_flag |= PATH_RAY_SURFACE_PASS;
      pass_diffuse_weight = PackedSpectrum(bsdf_eval_pass_diffuse_weight(&bsdf_eval));
      pass_glossy_weight = PackedSpectrum(bsdf_eval_pass_glossy_weight(&bsdf_eval));
    }

    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_diffuse_weight) = pass_diffuse_weight;
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_glossy_weight) = pass_glossy_weight;
  }

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = INTEGRATOR_STATE(
      state, path, visibility);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;

  return SHADER_EVAL_OK;
}

/* Falcon Cycles-expansion light tracing (kernel/integrator/init_from_light.h): keep track of the
 * caustic chains. The bounce number is the one before path_state_next(). False: a light path that
 * ends here. */
ccl_device_forceinline bool integrate_surface_caustic_chain(KernelGlobals kg,
                                                           IntegratorState state,
                                                           const ccl_private ShaderData *sd,
                                                           const int label,
                                                           const float2 bsdf_sampled_roughness)
{
  const bool nearly_specular = (label & LABEL_SINGULAR) ||
                               ((label & (LABEL_GLOSSY | LABEL_TRANSMIT)) &&
                                bsdf_sampled_roughness.x * bsdf_sampled_roughness.y <
                                    NR_LIGHT_TRACE_SPECULAR_ALPHA_SQUARED);
  /* A surface the shadow rays see through (the "Is Shadow Ray -> Transparent" glass of bulbs and
   * jars): the next event estimation already brings the light through it, and its shader may
   * show other rays something else again ("Is Reflection Ray" too in junkshop), so the light
   * paths do not go on from it and a camera path that bounces on it is left to path tracing
   * entirely (junkshop "Cristal": the photons through it gave its light twice, +1.4% of
   * the image, and the ones gathered behind it +0.7%). */
  const bool shadow_transparent = (sd->shader_flag & SD_HAS_TRANSPARENT_SHADOW) != 0;
  uint32_t flag = INTEGRATOR_STATE(state, path, flag);
  if (flag & PATH_RAY_NR_LIGHT_PATH) {
    /* A light path is only of use while it stays specular (L S+, then the diffuse vertex that
     * connects to the camera). */
    if ((!nearly_specular || shadow_transparent) &&
        !(kernel_data.integrator.bidirectional_pad1 & 1))
    {
      return false;
    }
    /* Per-material weight of the bidirectional caustics (Principled BSDF / "Bidirectional
     * Caustics (BD)" node, "BD Caustics Weight"). On the specular bounces of the light paths, so it reaches light tracing and the
     * stored photons alike. Front faces only: through a glass ball the light enters and leaves,
     * and weighting both gave the square of the weight (3 -> 9 times). */
    if (nearly_specular && !(sd->runtime_flag & SR_BACKFACING)) {
      const float bd_weight = kernel_data_fetch(shaders, (sd->shader & SHADER_MASK))
                                  .bd_caustics_weight;
      if (bd_weight != 1.0f) {
        if (!(bd_weight > 0.0f)) {
          return false;
        }
        INTEGRATOR_STATE_WRITE(state, path, throughput) *= bd_weight;
      }
    }
    flag |= PATH_RAY_NR_CAUSTIC_SPECULAR;
  }
  else if (!(INTEGRATOR_STATE(state, path, nr_chain) & NR_CHAIN_EVER_DIFFUSE)) {
    /* Camera path, all bounces so far specular: the first bounce that is not opens a chain (its
     * caustics come from light tracing on the first hit, from photon mapping after a specular
     * prefix). Specular ones keep the prefix going. */
    if (shadow_transparent || (!nearly_specular && !photon_map_diffuse_vertex(sd))) {
      flag &= ~(PATH_RAY_NR_CAUSTIC_CHAIN | PATH_RAY_NR_CAUSTIC_SPECULAR);
      INTEGRATOR_STATE_WRITE(state, path, nr_chain) = uint8_t(NR_CHAIN_EVER_DIFFUSE);
    }
    else if (!nearly_specular) {
      flag = (flag | PATH_RAY_NR_CAUSTIC_CHAIN) & ~PATH_RAY_NR_CAUSTIC_SPECULAR;
      INTEGRATOR_STATE_WRITE(state, path, nr_chain) = uint8_t(
          NR_CHAIN_EVER_DIFFUSE |
          ((INTEGRATOR_STATE(state, path, bounce) > 0) ? NR_CHAIN_PREFIX_SPECULAR : 0));
    }
    else {
      flag &= ~(PATH_RAY_NR_CAUSTIC_CHAIN | PATH_RAY_NR_CAUSTIC_SPECULAR);
    }
  }
  else if (flag & PATH_RAY_NR_CAUSTIC_CHAIN) {
    flag = (nearly_specular && !shadow_transparent) ?
               (flag | PATH_RAY_NR_CAUSTIC_SPECULAR) :
               (flag & ~(PATH_RAY_NR_CAUSTIC_CHAIN | PATH_RAY_NR_CAUSTIC_SPECULAR));
  }
  INTEGRATOR_STATE_WRITE(state, path, flag) = flag;
  return true;
}

/* Path tracing: bounce off or through surface with new direction. */
template<bool nr_bidir>
ccl_device_forceinline int integrate_surface_bsdf_bssrdf_bounce(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private ShaderData *sd,
    const ccl_private RNGState *rng_state)
{
  /* Sample BSDF or BSSRDF. */
  if (!(sd->runtime_flag & (SR_BSDF | SR_BSSRDF))) {
    return LABEL_NONE;
  }

  float3 rand_bsdf = path_state_rng_3D(kg, rng_state, PRNG_SURFACE_BSDF);
  const ccl_private ShaderClosure *sc = surface_shader_bsdf_bssrdf_pick(sd, &rand_bsdf);

#ifdef __SUBSURFACE__
  /* BSSRDF closure, we schedule subsurface intersection kernel. */
  if (CLOSURE_IS_BSSRDF(sc->type)) {
    return subsurface_bounce(kg, state, sd, sc);
  }
#endif
  if (CLOSURE_IS_RAY_PORTAL(sc->type)) {
    return integrate_surface_ray_portal(kg, state, sd, sc);
  }

  /* BSDF closure, sample direction. */
  float bsdf_pdf = 0.0f;
  float unguided_bsdf_pdf = 0.0f;
  BsdfEval bsdf_eval ccl_optional_struct_init;
  float3 bsdf_wo ccl_optional_struct_init;
  int label;

  float2 bsdf_sampled_roughness = make_float2(1.0f, 1.0f);
  float bsdf_eta = 1.0f;
  float mis_pdf = 1.0f;

  float bsdf_avg_roughness_squared = 0.0f;

#if defined(__PATH_GUIDING__) && PATH_GUIDING_LEVEL >= 4
  if (kernel_data.integrator.use_surface_guiding &&
      (kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING))
  {
    label = surface_shader_bsdf_guided_sample_closure(kg,
                                                      state,
                                                      sd,
                                                      sc,
                                                      rand_bsdf,
                                                      &bsdf_eval,
                                                      &bsdf_wo,
                                                      &bsdf_pdf,
                                                      &mis_pdf,
                                                      &unguided_bsdf_pdf,
                                                      &bsdf_sampled_roughness,
                                                      &bsdf_eta,
                                                      rng_state,
                                                      bsdf_avg_roughness_squared);

    if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval)) {
      return LABEL_NONE;
    }

    INTEGRATOR_STATE_WRITE(state, path, unguided_throughput) *= bsdf_pdf / unguided_bsdf_pdf;
  }
  else
#endif
  {
    label = surface_shader_bsdf_sample_closure(kg,
                                               sd,
                                               sc,
                                               rand_bsdf,
                                               &bsdf_eval,
                                               &bsdf_wo,
                                               &bsdf_pdf,
                                               &bsdf_sampled_roughness,
                                               &bsdf_eta,
                                               bsdf_avg_roughness_squared);

    if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval)) {
      return LABEL_NONE;
    }
    mis_pdf = bsdf_pdf;
    unguided_bsdf_pdf = bsdf_pdf;
  }

  if (label & LABEL_TRANSPARENT) {
    /* Only need to modify start distance for transparent. */
    INTEGRATOR_STATE_WRITE(state, ray, tmin) = intersection_t_offset(sd->ray_length);
  }
  else {
    /* Setup ray with changed origin and direction. */
    const float3 D = normalize(bsdf_wo);
    INTEGRATOR_STATE_WRITE(state, ray, P) = integrate_surface_ray_offset(kg, sd, sd->P, D);
    INTEGRATOR_STATE_WRITE(state, ray, D) = D;
    INTEGRATOR_STATE_WRITE(state, ray, tmin) = 0.0f;
    INTEGRATOR_STATE_WRITE(state, ray, tmax) = FLT_MAX;
#ifdef __RAY_DIFFERENTIALS__
    INTEGRATOR_STATE_WRITE(state, ray, dP) = differential_make_compact(sd->dP);

    /* Widen ray differences, with same logic as NEE sampling to ensure
     * both MIS strategies converge to the same result. */
    const float dD = bsdf_widen_dD(
        kg, INTEGRATOR_STATE(state, ray, dD), bsdf_avg_roughness_squared);
    INTEGRATOR_STATE_WRITE(state, ray, dD) = dD;
#endif
  }

  /* Update throughput. */
  const Spectrum bsdf_weight = bsdf_eval_sum(&bsdf_eval) / bsdf_pdf;
  INTEGRATOR_STATE_WRITE(state, path, throughput) *= bsdf_weight;

  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    if (INTEGRATOR_STATE(state, path, bounce) == 0) {
      INTEGRATOR_STATE_WRITE(state, path, pass_diffuse_weight) = bsdf_eval_pass_diffuse_weight(
          &bsdf_eval);
      INTEGRATOR_STATE_WRITE(state, path, pass_glossy_weight) = bsdf_eval_pass_glossy_weight(
          &bsdf_eval);
    }
  }

  /* Update path state */
  if (!(label & LABEL_TRANSPARENT)) {
    const float min_ray_pdf = INTEGRATOR_STATE(state, path, min_ray_pdf);
    INTEGRATOR_STATE_WRITE(state, path, mis_ray_pdf) = mis_pdf;
    INTEGRATOR_STATE_WRITE(state, path, mis_origin_n) = sd->N;
    INTEGRATOR_STATE_WRITE(state, path, min_ray_pdf) = fminf(unguided_bsdf_pdf, min_ray_pdf);

#ifdef __LIGHT_LINKING__
    if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_LINKING) {
      INTEGRATOR_STATE_WRITE(state, path, mis_ray_object) = sd->object;
    }
#endif
  }

  /* Falcon Cycles-expansion light tracing: the caustic chains (integrate_surface_caustic_chain()). */
  if constexpr (nr_bidir) {
    if (caustic_chains_tracked(kg) && !(label & LABEL_TRANSPARENT)) {
      if (!integrate_surface_caustic_chain(kg, state, sd, label, bsdf_sampled_roughness)) {
        return LABEL_NONE;
      }
    }
  }

  path_state_next(kg, state, label, sd->runtime_flag);

  guiding_record_surface_bounce(kg,
                                state,
                                bsdf_weight,
                                bsdf_pdf,
                                sd->N,
                                normalize(bsdf_wo),
                                bsdf_sampled_roughness,
                                bsdf_eta);

  return label;
}

#ifdef __VOLUME__
ccl_device_forceinline int integrate_surface_volume_only_bounce(IntegratorState state,
                                                                ccl_private ShaderData *sd)
{
  if (!path_state_volume_next(state)) {
    return LABEL_NONE;
  }

  /* Only modify start distance. */
  INTEGRATOR_STATE_WRITE(state, ray, tmin) = intersection_t_offset(sd->ray_length);

  return LABEL_TRANSMIT | LABEL_TRANSPARENT;
}
#endif

ccl_device_forceinline bool integrate_surface_terminate(IntegratorState state,
                                                        const uint32_t path_flag)
{
  const float continuation_probability = (path_flag & PATH_RAY_TERMINATE_ON_NEXT_SURFACE) ?
                                             0.0f :
                                             float(INTEGRATOR_STATE(
                                                 state, path, continuation_probability));
  if (continuation_probability == 0.0f) {
    return true;
  }
  if (continuation_probability != 1.0f) {
    INTEGRATOR_STATE_WRITE(state, path, throughput) /= continuation_probability;
  }

  return false;
}

#if defined(__AO__)
ccl_device_forceinline void integrate_surface_ao(KernelGlobals kg,
                                                 IntegratorState state,
                                                 const ccl_private ShaderData *ccl_restrict sd,
                                                 const ccl_private RNGState *ccl_restrict
                                                     rng_state)
{
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (!(kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) &&
      !(path_visibility & PATH_RAY_VISIBILITY_CAMERA))
  {
    return;
  }

  /* Skip AO for paths that were split off for shadow catchers to avoid double-counting. */
  if (path_flag & PATH_RAY_SHADOW_CATCHER_PASS) {
    return;
  }

  const float2 rand_bsdf = path_state_rng_2D(kg, rng_state, PRNG_SURFACE_BSDF);

  float3 ao_N;
  const Spectrum ao_weight = surface_shader_ao(
      sd, kernel_data.integrator.ao_additive_factor, &ao_N);

  float3 ao_D;
  float ao_pdf;
  sample_cos_hemisphere(ao_N, rand_bsdf, &ao_D, &ao_pdf);

  bool skip_self = true;

  Ray ray ccl_optional_struct_init;
  ray.P = shadow_ray_offset(kg, sd, ao_D, &skip_self);
  ray.D = ao_D;
  if (skip_self) {
    ray.P = integrate_surface_ray_offset(kg, sd, ray.P, ray.D);
  }
  ray.tmin = 0.0f;
  ray.tmax = kernel_data.integrator.ao_bounces_distance;
  ray.time = sd->time;
  ray.self.object = (skip_self) ? sd->object : OBJECT_NONE;
  ray.self.prim = (skip_self) ? sd->prim : PRIM_NONE;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  ray.dP = differential_zero_compact();
  ray.dD = differential_zero_compact();

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrator_shadow_path_init(
      kg, state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW, true);

#  ifdef __VOLUME__
  /* Copy volume stack and enter/exit volume. */
  integrator_state_copy_volume_stack_to_shadow(kg, shadow_state, state);
#  endif

  /* Write shadow ray and associated state to global memory. */
  integrator_state_write_shadow_ray(shadow_state, &ray);
  integrator_state_write_shadow_ray_self(shadow_state, &ray);

  /* Copy state from main path to shadow path. */
  const uint16_t bounce = INTEGRATOR_STATE(state, path, bounce);
  const uint16_t transparent_bounce = INTEGRATOR_STATE(state, path, transparent_bounce);
  const uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag) | PATH_RAY_SHADOW_FOR_AO;
  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput) * surface_shader_alpha(sd);

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, render_pixel_index) = INTEGRATOR_STATE(
      state, path, render_pixel_index);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_offset) = INTEGRATOR_STATE(
      state, path, rng_offset);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_pixel) = INTEGRATOR_STATE(
      state, path, rng_pixel);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, sample) = INTEGRATOR_STATE(
      state, path, sample);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = INTEGRATOR_STATE(
      state, path, visibility);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bounce) = bounce;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transparent_bounce) = transparent_bounce;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, volume_bounds_bounce) = INTEGRATOR_STATE(
      state, path, volume_bounds_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, throughput) = throughput;

  if (kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, unshadowed_throughput) = ao_weight;
  }
}
#endif /* defined(__AO__) */

/* Falcon Cycles-expansion light tracing: connect a vertex of a light path to the camera
 * (kernel/integrator/init_from_light.h). Only the nearly diffuse closures take part: the light
 * path so far was L S+, so the connected path is L S+ D E, the one the camera paths leave out.
 * The connection is a shadow ray towards the camera that adds to the pixel it lands in. Pinhole
 * perspective cameras only. */
ccl_device_forceinline void integrate_surface_light_trace_connect(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private ShaderData *sd,
    ccl_global float *ccl_restrict render_buffer)
{
  uint pixel;
  float3 to_camera;
  float distance;
  const float importance = light_trace_camera_importance(kg, sd->P, &pixel, &to_camera, &distance);
  if (!(importance > 0.0f)) {
    return;
  }

  /* BSDF of the nearly diffuse closures towards the camera (including the cosine). */
  Spectrum f = zero_spectrum();
  for (int i = 0; i < sd->num_closure; i++) {
    const ccl_private ShaderClosure *sc = &sd->closure[i];
    if (!CLOSURE_IS_BSDF(sc->type) ||
        bsdf_get_specular_roughness_squared(sc) < NR_LIGHT_TRACE_SPECULAR_ALPHA_SQUARED)
    {
      continue;
    }
    float pdf = 0.0f;
    const Spectrum eval = bsdf_eval(kg, sd, sc, to_camera, &pdf);
    f += eval * sc->weight;
  }
  if (is_zero(f)) {
    return;
  }

  Ray ray ccl_optional_struct_init;
  ray.P = integrate_surface_ray_offset(kg, sd, sd->P, to_camera);
  ray.D = to_camera;
  ray.tmin = 0.0f;
  ray.tmax = distance * (1.0f - 1e-4f);
  ray.time = sd->time;
  ray.self.object = sd->object;
  ray.self.prim = sd->prim;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
#ifdef __RAY_DIFFERENTIALS__
  ray.dP = differential_make_compact(sd->dP);
  ray.dD = differential_zero_compact();
#endif

  light_trace_guide_credit(kg, state, f);

  /* Caustic Strength scales the caustics of light tracing (a vertex after a specular chain) like
   * those of photon mapping. */
  const float strength = (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_NR_CAUSTIC_SPECULAR) ?
                             kernel_data.integrator.nr_photon_strength :
                             1.0f;
  /* Caustic Spread: the caustics land in the pixel of a point moved along the
   * surface, Gaussian with sigma (spread - 1) times the photon radius like the photon gathers
   * (photon_map.h); without photons, (spread - 1) * 2 pixels. The light is kept, the time the same. */
  const float spread = kernel_data.integrator.nr_caustic_spread;
  if (spread > 1.0f && (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_NR_CAUSTIC_SPECULAR)) {
    const uint h = hash_uint3(__float_as_uint(sd->P.x),
                              __float_as_uint(sd->P.y),
                              __float_as_uint(sd->P.z) ^ INTEGRATOR_STATE(state, path, rng_pixel));
    const float u1 = (float(h & 0xffffu) + 0.5f) * (1.0f / 65536.0f);
    const float u2 = float(h >> 16) * (1.0f / 65536.0f);
    const float g = sqrtf(-2.0f * logf(u1));  /* (light paths write to scattered pixels anyway) */
    const float c = cosf(M_2PI_F * u2), s = sinf(M_2PI_F * u2);
    const float r = kernel_data.integrator.nr_photon_radius;
    if (r > 0.0f) {
      float3 T, B;
      make_orthonormals(sd->Ng, &T, &B);
      uint moved_pixel;
      float3 moved_dir;
      float moved_distance;
      if (!(light_trace_camera_importance(kg,
                                          sd->P + (spread - 1.0f) * r * g * (c * T + s * B),
                                          &moved_pixel,
                                          &moved_dir,
                                          &moved_distance) > 0.0f))
      {
        return;
      }
      pixel = moved_pixel;
    }
    else {
      const int width = int(kernel_data.cam.width);
      const int height = int(kernel_data.cam.height);
      const float rad = (spread - 1.0f) * 2.0f * g;
      const int px = int(pixel % uint(width)) + int(floorf(rad * c + 0.5f));
      const int py = int(pixel / uint(width)) + int(floorf(rad * s + 0.5f));
      if (px < 0 || py < 0 || px >= width || py >= height) {
        return;
      }
      pixel = uint(px + py * width);
    }
  }
  const int slice_y_start = kernel_data.integrator.nr_lt_slice_y_start;
  const int slice_y_end = kernel_data.integrator.nr_lt_slice_y_end;
  if (slice_y_end > slice_y_start) {
    const int width = int(kernel_data.cam.width);
    const int px = int(pixel % uint(width));
    const int py = int(pixel / uint(width));
    if (py < slice_y_start || py >= slice_y_end) {
      return;
    }
    pixel = uint(px + (py - slice_y_start) * width);
  }
  const IntegratorShadowState shadow_state = integrate_direct_light_shadow_init_common(
      kg, state, &ray, f * (importance * strength), LIGHTGROUP_NONE, 0, true);
  /* Lands in the pixel the vertex is seen in. The render buffer is the whole frame (checked by
   * PathTrace before the light paths are rendered). */
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, render_pixel_index) = pixel;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = PATH_RAY_VISIBILITY_CAMERA;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = INTEGRATOR_STATE(state, path, flag) &
                                                            ~PATH_RAY_ANY_PASS;
}

/* Falcon Cycles-expansion light tracing: a vertex of a light path. It writes nothing there, it
 * connects to the camera once a specular bounce brought it here, then goes on while it stays
 * specular (the bounce ends it otherwise). */
ccl_device_forceinline int integrate_surface_light_path_vertex(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private ShaderData *sd,
    ccl_global float *ccl_restrict render_buffer,
    const uint32_t path_flag)
{
  if (integrate_surface_terminate(state, path_flag)) {
    return LABEL_NONE;
  }
  if (sd->shader_flag & SD_HAS_TRANSPARENT_SHADOW) {
    /* Nothing on a surface the shadow rays see through (see the bounce below). */
  }
  else if (kernel_data.integrator.bidirectional_pad1 & NR_BIDIR_PHOTON_PASS_FLAG) {
    /* Photon pass of photon mapping: store, do not connect. */
    if (path_flag & PATH_RAY_NR_CAUSTIC_SPECULAR) {
      photon_map_store(kg, state, sd);
    }
  }
  else if (((path_flag & PATH_RAY_NR_CAUSTIC_SPECULAR) && photon_map_diffuse_vertex(sd)) ||
           (kernel_data.integrator.bidirectional_pad1 & 1))
  {
    integrate_surface_light_trace_connect(kg, state, sd, render_buffer);
  }
  /* Light Bounces of the Caustics panel (photon mapping bounces): a
   * light path (photon pass and light tracing) stores or connects here, then stops after this many
   * bounces. It was read nowhere before, the paths ending only by the Light Paths bounces. */
  if (int(INTEGRATOR_STATE(state, path, bounce)) >=
      kernel_data.integrator.bidirectional_max_light_bounces)
  {
    return LABEL_NONE;
  }
  /* Caustics Override Dispersion: the photons split into red, green and blue through glass. */
  if ((kernel_data.integrator.bidirectional_pad1 & NR_BIDIR_PHOTON_PASS_FLAG) &&
      kernel_data.integrator.nr_caustic_dispersion > 0.0f)
  {
    photon_map_disperse(kg, state, sd);
  }
  RNGState rng_state;
  path_state_rng_load(state, &rng_state);
  const int label = integrate_surface_bsdf_bssrdf_bounce<true>(kg, state, sd, &rng_state);
  if (label & LABEL_SUBSURFACE_SCATTER) {
    return LABEL_NONE;
  }
#  ifdef __VOLUME__
  if (label & LABEL_TRANSMIT) {
    volume_stack_enter_exit<false>(kg, state, sd);
  }
#  endif
  return label;
}

/* Falcon Cycles-expansion photon mapping (kernel/integrator/photon_map.h): the caustics gathered
 * here, into the image. */
ccl_device_forceinline void integrate_surface_photon_gather(
    KernelGlobals kg,
    ConstIntegratorState state,
    ccl_private ShaderData *sd,
    ccl_global float *ccl_restrict render_buffer)
{
  const Spectrum caustics = photon_map_gather(kg, sd, INTEGRATOR_STATE(state, path, sample));
  if (is_zero(caustics)) {
    return;
  }
  film_write_surface_emission(kg, state, caustics, 1.0f, render_buffer, LIGHTGROUP_NONE);
  /* Automatic bidirectional: what the gathers of the first sample add to the image
   * (PathTraceWorkGPU::update_photon_auto()). Only the first sample, so the atomics do not slow
   * the rest of the frame. */
  if ((kernel_data.integrator.bidirectional_pad1 & NR_BIDIR_AUTO_FLAG) &&
      INTEGRATOR_STATE(state, path, sample) == 0)
  {
    atomic_add_and_fetch_float((ccl_global float *)&kernel_data_array(nr_photon_counter)[1],
                               average(INTEGRATOR_STATE(state, path, throughput) * caustics));
  }
}

template<uint64_t node_feature_mask, bool nr_bidir>
ccl_device int integrate_surface(KernelGlobals kg,
                                 IntegratorState state,
                                 ccl_global float *ccl_restrict render_buffer)

{
  PROFILING_INIT_FOR_SHADER(kg, PROFILING_SHADE_SURFACE_SETUP);

  /* Setup shader data. */
  ShaderData sd;
  integrate_surface_shader_setup(kg, state, &sd);
  PROFILING_SHADER(sd.object, sd.shader);

  int continue_path_label = 0;

  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  /* Skip most work for volume bounding surface. */
#ifdef __VOLUME__
  if (!(sd.shader_flag & SD_HAS_ONLY_VOLUME)) {
#endif
#ifdef __SUBSURFACE__
    /* Can skip shader evaluation for BSSRDF exit point without bump mapping. */
    if (!(path_flag & PATH_RAY_SUBSURFACE) || ((sd.shader_flag & SD_HAS_BSSRDF_BUMP)))
#endif
    {
      /* Evaluate shader. */
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_EVAL);
      surface_shader_eval<node_feature_mask>(
          kg, state, &sd, render_buffer, path_visibility, path_flag);
    }

    if (sd.runtime_flag & SR_CACHE_MISS) {
      return LABEL_CACHE_MISS;
    }

#ifdef __SPECTRAL__
    if (sd.runtime_flag & SR_BSDF_HAS_DISPERSION) {
      update_path_throughput_for_dispersion(kg, state, sd.rand_wavelength);
    }
#endif

    /* After shader evaluation, in case of texture cache miss. */
    guiding_record_surface_segment(kg, state, &sd);

#ifdef __SUBSURFACE__
    if (path_flag & PATH_RAY_SUBSURFACE) {
      /* When coming from inside subsurface scattering, setup a diffuse
       * closure to perform lighting at the exit point. */
      subsurface_shader_data_setup(kg, &sd);
      INTEGRATOR_STATE_WRITE(state, path, flag) &= ~PATH_RAY_SUBSURFACE;
    }
    else
#endif
    {
      /* Filter closures. */
      surface_shader_prepare_closures<nr_bidir>(kg, state, &sd, path_visibility);

      /* Falcon Cycles-expansion light tracing (integrate_surface_light_path_vertex()). */
      if constexpr (nr_bidir) {
        if (path_flag & PATH_RAY_NR_LIGHT_PATH) {
          return integrate_surface_light_path_vertex(kg, state, &sd, render_buffer, path_flag);
        }

        integrate_surface_nr_glass_samples_mark(kg, state, &sd, render_buffer);

        /* Falcon Cycles-expansion photon mapping (kernel/integrator/photon_map.h): the caustics at the
         * first not nearly specular vertex, after a specular prefix (seen through glass), or on the
         * first hit when light tracing is off. */
        if (photon_mapping_active(kg) && !(sd.shader_flag & SD_HAS_TRANSPARENT_SHADOW) &&
            photon_map_diffuse_vertex(&sd) &&
            !(INTEGRATOR_STATE(state, path, nr_chain) & NR_CHAIN_EVER_DIFFUSE) &&
            (INTEGRATOR_STATE(state, path, bounce) > 0 || !light_trace_active(kg)) &&
            /* NR_LT_DEBUG & 32 / 64 (diagnostics): gather only on the first hit / only after it. */
            !((kernel_data.integrator.bidirectional_pad1 & 32) &&
              INTEGRATOR_STATE(state, path, bounce) > 0) &&
            !((kernel_data.integrator.bidirectional_pad1 & 64) &&
              INTEGRATOR_STATE(state, path, bounce) == 0))
        {
          integrate_surface_photon_gather(kg, state, &sd, render_buffer);
        }
      }

      /* Evaluate holdout. */
      if (!integrate_surface_holdout(kg, state, &sd, render_buffer)) {
        return LABEL_NONE;
      }

      /* Write emission. */
      if (sd.runtime_flag & SR_EMISSION) {
        integrate_surface_emission<nr_bidir>(kg, state, &sd, render_buffer);
      }

      /* Perform path termination. Most paths have already been terminated in
       * the intersect_closest kernel, this is just for emission and for dividing
       * throughput by the probability at the right moment.
       *
       * Also ensure we don't do it twice for SSS at both the entry and exit point. */
      if (integrate_surface_terminate(state, path_flag)) {
        return LABEL_NONE;
      }

      /* Write render passes. */
#ifdef __PASSES__
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_PASSES);
      film_write_data_passes(kg, state, path_flag, &sd, render_buffer);
#endif

#ifdef __DENOISING_FEATURES__
      film_write_denoising_features_surface(kg, state, path_flag, &sd, render_buffer);
#endif
    }

    /* Load random number state. */
    RNGState rng_state;
    path_state_rng_load(state, &rng_state);

#if defined(__PATH_GUIDING__) && PATH_GUIDING_LEVEL >= 4
    if (kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING) {
      surface_shader_prepare_guiding(kg, state, &sd, &rng_state);
      guiding_write_debug_passes(kg, state, &sd, render_buffer);
    }
#endif
    /* Direct light. */
    PROFILING_EVENT(PROFILING_SHADE_SURFACE_DIRECT_LIGHT);
    const ShaderEvalResult result = integrate_surface_direct_light<node_feature_mask, nr_bidir>(
        kg, state, &sd, &rng_state);
    if (result == SHADER_EVAL_CACHE_MISS) {
      return LABEL_CACHE_MISS;
    }

#if defined(__AO__)
    /* Ambient occlusion pass. */
    if (kernel_data.kernel_features & KERNEL_FEATURE_AO) {
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_AO);
      integrate_surface_ao(kg, state, &sd, &rng_state);
    }
#endif

    PROFILING_EVENT(PROFILING_SHADE_SURFACE_INDIRECT_LIGHT);
    continue_path_label = integrate_surface_bsdf_bssrdf_bounce<nr_bidir>(kg, state, &sd, &rng_state);
#ifdef __VOLUME__
  }
  else {
    if (integrate_surface_terminate(state, path_flag)) {
      return LABEL_NONE;
    }

#  ifdef __DENOISING_FEATURES__
    film_write_denoising_features_surface_volume(kg, state, &sd, render_buffer);
#  endif

    PROFILING_EVENT(PROFILING_SHADE_SURFACE_INDIRECT_LIGHT);
    continue_path_label = integrate_surface_volume_only_bounce(state, &sd);
  }

  if (continue_path_label & LABEL_TRANSMIT) {
    /* Enter/Exit volume. */
    volume_stack_enter_exit<false>(kg, state, &sd);
  }
#endif

  return continue_path_label;
}

template<DeviceKernel current_kernel>
ccl_device_forceinline void integrator_shade_surface_next_kernel(IntegratorState state)
{
  if (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_SUBSURFACE) {
    integrator_path_next(state, current_kernel, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE);
  }
  else {
    kernel_assert(INTEGRATOR_STATE(state, ray, tmax) != 0.0f);
    integrator_path_next(state, current_kernel, DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
  }
}

template<uint64_t node_feature_mask = KERNEL_FEATURE_NODE_MASK_SURFACE &
                                      ~KERNEL_FEATURE_NODE_RAYTRACE,
         DeviceKernel current_kernel = DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE>
ccl_device_forceinline void integrator_shade_surface(KernelGlobals kg,
                                                     IntegratorState state,
                                                     ccl_global float *ccl_restrict render_buffer)
{
  /* Falcon Cycles-expansion: the bidirectional techniques only in the kernels other than the plain
   * one (kernel/integrator/state_flow.h integrator_shade_surface_kernel()). */
  constexpr bool nr_bidir = (current_kernel != DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE)
#ifdef __NR_SHADER_RAYTRACE_WITHOUT_BIDIR__
                           /* The OptiX shader ray-tracing module for scenes without them
                            * (KERNEL_FEATURE_NR_BIDIR, kernel/device/optix/kernel_shader_raytrace.cu). */
                           && (current_kernel != DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE)
#endif
      ;
  const int continue_path_label = integrate_surface<node_feature_mask, nr_bidir>(
      kg, state, render_buffer);
  if (continue_path_label == LABEL_CACHE_MISS) {
    integrator_path_cache_miss_sorted(state, current_kernel);
    return;
  }

#ifdef __MNEE__
  /* Cleanup MNEE flag and shadow path if it was not reused for shadow trace. */
  if ((kernel_data.kernel_features & KERNEL_FEATURE_MNEE) &&
      (INTEGRATOR_STATE(state, path, mnee) & PATH_MNEE_SAMPLED))
  {
    INTEGRATOR_STATE_WRITE(state, path, mnee) &= ~PATH_MNEE_SAMPLED;

    const IntegratorShadowState shadow_state = integrator_state_get_mnee_shadow_state(state);
    if (INTEGRATOR_STATE(shadow_state, shadow_path, queued_kernel) ==
        DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING)
    {
      integrator_shadow_path_terminate(shadow_state,
                                       DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING);
    }
  }
#endif

  if (continue_path_label == LABEL_NONE) {
    integrator_path_terminate(kg, state, render_buffer, current_kernel);
    return;
  }

#ifdef __SHADOW_LINKING__
  /* No need to cast shadow linking rays at a transparent bounce: the lights will be accumulated
   * via the main path in this case. BSSRDF bounces continue with intersect_subsurface. */
  if ((continue_path_label & (LABEL_TRANSPARENT | LABEL_SUBSURFACE_SCATTER)) == 0) {
    if (shadow_linking_schedule_intersection_kernel<current_kernel>(kg, state)) {
      return;
    }
  }
#endif

  integrator_shade_surface_next_kernel<current_kernel>(state);
}

/* Falcon Cycles-expansion: shade_surface with the bidirectional techniques
 * (integrator_shade_surface_kernel()). */
ccl_device_forceinline void integrator_shade_surface_bidir(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  integrator_shade_surface<KERNEL_FEATURE_NODE_MASK_SURFACE & ~KERNEL_FEATURE_NODE_RAYTRACE,
                           DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_BIDIR>(
      kg, state, render_buffer);
}

ccl_device_forceinline void integrator_shade_surface_raytrace(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  integrator_shade_surface<KERNEL_FEATURE_NODE_MASK_SURFACE,
                           DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE>(
      kg, state, render_buffer);
}

CCL_NAMESPACE_END
