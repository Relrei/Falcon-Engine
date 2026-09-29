/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/closure/bsdf.h"

#include "kernel/film/write.h"

CCL_NAMESPACE_BEGIN

/* Squared GGX roughness (alpha_x * alpha_y) under which a glass / refraction closure counts as
 * smooth for the glass through-guides. Same value as BSDF_ROUGHNESS_SQ_THRESH (removed from svm in
 * Blender 5.2, kept for the dispersion gate in falcon_dispersion.h). */
#ifndef FALCON_GLASS_SMOOTH_ROUGHNESS_SQ
#  define FALCON_GLASS_SMOOTH_ROUGHNESS_SQ 2e-10f
#endif

/* Squared GGX roughness (alpha_x * alpha_y) up to which a glass closure counts as smooth, by the
 * level in bits 5..7 of the denoising options (FALCON_DLSS_GLASS_ROUGH). The levels are given as
 * the roughness of the shader node (alpha = roughness^2). */
ccl_device_forceinline float falcon_glass_smooth_roughness_sq(const int options_flag)
{
  switch ((options_flag >> DENOISING_PASS_GLASS_SMOOTH_SHIFT) & 7) {
    case 1:
      return 6.25e-6f; /* roughness 0.05 */
    case 2:
      return 1e-4f; /* roughness 0.1 */
    case 3:
      return 1.6e-3f; /* roughness 0.2 */
    default:
      return FALCON_GLASS_SMOOTH_ROUGHNESS_SQ;
  }
}

/* Diffuse albedo the glass matte guides describe the glass surface with (the same value the RR
 * guide suggests for pixels with nothing to tell, such as sky). */
#ifndef FALCON_GLASS_MATTE_ALBEDO
#  define FALCON_GLASS_MATTE_ALBEDO 0.5f
#endif

#ifdef __DENOISING_FEATURES__
ccl_device_forceinline float denoising_depth_compute(KernelGlobals kg,
                                                     IntegratorState state,
                                                     const ccl_private ShaderData *sd,
                                                     const Spectrum denoising_feature_throughput,
                                                     const bool follow_reflections)
{
  float depth;
  const float d = sd->ray_length - INTEGRATOR_STATE(state, ray, tmin);
  if (follow_reflections) {
    /* Write the ray length minus tmin. */
    depth = d;
  }
  else {
    /* Write the camera z depth. */
    const float3 prev_P = sd->P + sd->wi * d;
    const float prev_depth = camera_z_depth(kg, prev_P);
    const float new_depth = camera_z_depth(kg, sd->P);
    depth = new_depth - prev_depth;
  }

  return ensure_finite(depth * average(denoising_feature_throughput));
}

/* Distance from the primary surface to what its specular lobe hit, for DLSS-RR
 * (Integration Guide 3.4.9). RR reconstructs the specular motion vector from it,
 * so reflections move with what they reflect rather than with the surface that
 * carries them -- without it, highlights on polished metal smear.
 *
 * Mirrors NVIDIA's own path tracer (vk_denoise_dlssrr): the distance is the
 * length of the first ray leaving the primary surface, when that bounce was
 * glossy. A ray that escapes to the environment counts as a very distant hit,
 * not as no hit -- a car reflects mostly sky.
 *
 * Only the samples whose primary bounce was glossy say anything, so their count
 * rides along in the second component and the mean is taken over those alone. */
ccl_device_forceinline void film_write_denoising_specular_hit_distance(
    KernelGlobals kg, IntegratorState state, const float hit_distance,
    ccl_global float *ccl_restrict render_buffer)
{
  if (kernel_data.film.pass_denoising_specular_hit_distance == PASS_UNUSED) {
    return;
  }

  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  if (path_flag & PATH_RAY_SHADOW_CATCHER_PASS) {
    return;
  }

  /* Falcon glass through-guides: the pixel's guides describe what is seen through the glass, and
   * only some of the samples reflect. Their hit distance would flicker between 0 and a distance
   * from frame to frame, and RR builds the reflection motion from it, so glass pixels get none
   * (RR Integration Guide 3.4.1: keep depth, normal and specular hit distance consistent). */
  if (path_flag & PATH_RAY_GLASS_SEEN) {
    return;
  }

  /* One bounce in, and that bounce was glossy: the ray that just ended started
   * on the primary surface and landed in the reflection. */
  if (INTEGRATOR_STATE(state, path, bounce) != 1 ||
      INTEGRATOR_STATE(state, path, glossy_bounce) != 1)
  {
    return;
  }

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  film_write_pass_float3(buffer + kernel_data.film.pass_denoising_specular_hit_distance,
                         make_float3(ensure_finite(hit_distance), 1.0f, 0.0f));
}

/* `force_first`: write every guide as for a first surface hit even though this is not the first
 * bounce. Only the glass through-guides ask for it (film_write_denoising_glass_resolve). */
ccl_device_forceinline void film_write_denoising_features_surface(KernelGlobals kg,
                                                                  IntegratorState state,
                                                                  const ccl_private ShaderData *sd,
                                                                  ccl_global float *ccl_restrict
                                                                      render_buffer,
                                                                  const bool force_first)
{
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  if (!(path_flag & PATH_RAY_DENOISING_FEATURES)) {
    return;
  }

  /* Don't write denoising passes for paths that were split off for shadow catchers
   * to avoid double-counting. */
  if (path_flag & PATH_RAY_SHADOW_CATCHER_PASS) {
    return;
  }

  const bool use_albedo_roughness_weighting = (kernel_data.film.denoising_pass_options_flag &
                                               DENOISING_PASS_USE_ALBEDO_ROUGHNESS_WEIGHTING) != 0;

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  float3 normal = zero_float3();
  Spectrum diffuse_albedo = zero_spectrum();
  Spectrum specular_albedo = zero_spectrum();
  Spectrum transparent_albedo = zero_spectrum();
  float specular_roughness = 0.0f;
  float sum_weight = 0.0f;
  float sum_nonspecular_weight = 0.0f;
  /* Weight of the smooth (delta) glass / refraction closures, for the glass through-guides. */
  float smooth_glass_weight = 0.0f;
  const float glass_smooth_roughness_sq = falcon_glass_smooth_roughness_sq(
      kernel_data.film.denoising_pass_options_flag);
  bool has_transmission = false;

  for (int i = 0; i < sd->num_closure; i++) {
    const ccl_private ShaderClosure *sc = &sd->closure[i];

    if (!CLOSURE_IS_BSDF_OR_BSSRDF(sc->type)) {
      continue;
    }

    /* Transparency always passes through. */
    if (CLOSURE_IS_BSDF_TRANSPARENT(sc->type)) {
      transparent_albedo += sc->weight;
      continue;
    }

    if (CLOSURE_IS_BSDF_TRANSMISSION(sc->type) || CLOSURE_IS_GLASS(sc->type) ||
        CLOSURE_IS_REFRACTION(sc->type))
    {
      has_transmission = true;
    }

    const Spectrum closure_albedo = bsdf_albedo(kg, sd, sc, true, true);
    const float closure_weight = average(closure_albedo);

    /* All closures contribute to the normal feature, but only diffuse-like ones to the albedo. */
    /* If far-field hair, use fiber tangent as feature instead of normal. */
    normal += (sc->type == CLOSURE_BSDF_HAIR_HUANG_ID ? safe_normalize(sd->dPdu) : sc->N) *
              closure_weight;

    /* bsdf_get_specular_roughness_squared returns GGX alpha squared (alpha_x*alpha_y). Use sqrtf
     * to get GGX alpha. */
    const float roughness = sqrtf(bsdf_get_specular_roughness_squared(sc));

    if ((CLOSURE_IS_GLASS(sc->type) || CLOSURE_IS_REFRACTION(sc->type)) &&
        bsdf_get_specular_roughness_squared(sc) <= glass_smooth_roughness_sq)
    {
      smooth_glass_weight += closure_weight;
    }

    /* Transition smoothly from specular to diffuse between 0.0 and 0.15 roughness. */
    const float diffuse_weight = (sc->type == CLOSURE_BSDF_HAIR_HUANG_ID) ?
                                     1.0f :
                                     smoothstep(0.0f, 0.15f, roughness);

    if (use_albedo_roughness_weighting) {
      diffuse_albedo += closure_albedo * diffuse_weight;
      specular_albedo += closure_albedo * (1.0f - diffuse_weight);
    }
    else if (CLOSURE_IS_BSDF_DIFFUSE(sc->type) || CLOSURE_IS_BSSRDF(sc->type)) {
      diffuse_albedo += closure_albedo;
    }
    else if (CLOSURE_IS_BSDF_GLOSSY(sc->type) || CLOSURE_IS_GLASS(sc->type)) {
      specular_albedo += closure_albedo;
    }
    /* Apply sqrtf again to convert GGX alpha to perceptual roughness. */
    specular_roughness += sqrtf(roughness) * closure_weight;

    sum_weight += closure_weight;
    sum_nonspecular_weight += closure_weight * diffuse_weight;
  }

  /* Fraction of non-transparent closures, for smooth blending at transparent surfaces. */
  const float transparent_weight = average(transparent_albedo);
  const float total_weight = sum_weight + transparent_weight;

  /* Blend between writing features at this bounce vs. deferring to the next bounce based
   * on the proportion of diffuse closures. Smoothly transition between 0.0 and 0.5 diffuse
   * fraction. */
  float feature_weight = 0.0f;
  if (sum_weight > 0.0f) {
    normal /= sum_weight;
    specular_roughness /= sum_weight;

    feature_weight = smoothstep(0.0f, 0.5f, sum_nonspecular_weight / sum_weight);
  }

  /* Whether to defer features to the next bounce for individual passes. */
  const bool follow_reflections = (kernel_data.film.denoising_pass_options_flag &
                                   DENOISING_PASS_FOLLOW_REFLECTIONS) != 0;
  if (!follow_reflections) {
    feature_weight = 1.0f;
  }

  const Spectrum denoising_feature_throughput = INTEGRATOR_STATE(
      state, path, denoising_feature_throughput);
  const bool is_first_bounce = INTEGRATOR_STATE(state, path, bounce) == 0;

  /* Falcon glass through-guides.
   *
   * Clear glass gets the glass surface's own guides otherwise: roughness 0, specular albedo ~1,
   * no diffuse albedo, the glass's normal, depth and motion. RR reads that as a mirror, while what
   * the pixel shows is mostly the background refracted through it, so the background detail is
   * treated as noise and smoothed. RR Integration Guide 3.4.1: where the see-through background
   * dominates the guides should describe it, and the reflection-dominated part (Fresnel) the
   * glass surface; it must not be noisy either (3.5).
   *
   * The lobe is only known after the BSDF is sampled, so the guides of a smooth glass hit are held
   * back here (PATH_RAY_GLASS_PENDING) and film_write_denoising_glass_resolve() decides: a sample
   * that refracts carries on and the next surface writes for it (PATH_RAY_GLASS_THROUGH), one
   * that reflects writes the glass surface now. Lobes are chosen in proportion to Fresnel, so
   * the pixel average is the Fresnel blend the guide asks for. A second smooth glass behind
   * the first (the far wall of a bottle) is held back the same way.
   *
   * Not with following reflections on (that already defers the guides its own way), and not for
   * mixtures that are mostly something else than smooth glass. */
  const bool glass_through_enabled = (kernel_data.film.denoising_pass_options_flag &
                                      DENOISING_PASS_GLASS_THROUGH) != 0 &&
                                     !follow_reflections;
  const bool glass_through_now = glass_through_enabled && (path_flag & PATH_RAY_GLASS_THROUGH) != 0;

  if (glass_through_now && sum_weight == 0.0f && transparent_weight > 1e-4f) {
    /* An alpha hole behind the glass: keep waiting for the surface behind it. */
    INTEGRATOR_STATE_WRITE(state, path, denoising_feature_throughput) *= transparent_albedo;
    return;
  }

  /* Smooth glass (and nothing much else) is what this surface is. */
  const bool glass_dominant = sum_weight > 0.0f && transparent_weight < 1e-4f &&
                              smooth_glass_weight >= 0.5f * sum_weight;

  if (glass_through_enabled && !force_first && glass_dominant &&
      ((is_first_bounce && !(path_flag & (PATH_RAY_GLASS_SEEN | PATH_RAY_PSR))) ||
       glass_through_now))
  {
    INTEGRATOR_STATE_WRITE(state, path, flag) = (path_flag & ~PATH_RAY_GLASS_THROUGH) |
                                                PATH_RAY_GLASS_PENDING;
    return;
  }

  /* Every guide is written as for a first surface hit at the surface behind smooth glass, and at
   * a glass surface whose lobe has been resolved. */
  const bool write_as_first = is_first_bounce || glass_through_now || force_first;

  /* Falcon glass matte guides. RR Integration Guide 3.5: where the guides of a refracting surface
   * cannot be made noise-free, constant values let RR converge to a smooth result. The glass
   * surface's own guides (a mirror to RR: roughness 0, specular albedo ~1, no diffuse albedo) make
   * it keep the noise of what is seen through the glass as if it were a sharp reflection, so
   * describe the glass as a plain rough diffuse surface instead. Applies wherever the glass
   * surface's own guides are written (a first hit, or a resolved reflecting sample); the surface
   * behind the glass keeps its own. Its specular hit distance goes with it (a matte surface has
   * no reflection to track). */
  const bool glass_matte_enabled = (kernel_data.film.denoising_pass_options_flag &
                                    DENOISING_PASS_GLASS_MATTE) != 0 &&
                                   !follow_reflections;
  if (glass_matte_enabled && glass_dominant && !glass_through_now &&
      (is_first_bounce || force_first))
  {
    diffuse_albedo = one_spectrum() * FALCON_GLASS_MATTE_ALBEDO;
    specular_albedo = zero_spectrum();
    specular_roughness = 1.0f;
    INTEGRATOR_STATE_WRITE(state, path, flag) |= PATH_RAY_GLASS_SEEN;
  }

  /* Primary surface replacement.
   *
   * A mirror has no detail of its own: what the denoiser sees at that pixel belongs to whatever is
   * reflected, and that reflection slides across the mirror as the camera moves. Describing the
   * mirror surface (its depth, its normal, its motion) tells the denoiser the image is pinned to
   * the mirror, which is why highlights on polished metal smear. Instead, hold the guides back at
   * the mirror and write them for the virtual image -- the reflected hit mirrored back through the
   * reflector's plane. Depth, normal and motion then all agree with where the reflection appears.
   *
   * Restricted to delta (perfectly sharp) reflectors: a rough one reflects a lobe, not an image,
   * so the deferred hit would be a different surface for every sample. Refractive surfaces are
   * left alone as well -- their first-surface guides are what keeps glass free of speckle. */
  const bool psr_enabled = (kernel_data.film.denoising_pass_options_flag & DENOISING_PASS_PSR) != 0;
  const bool psr_pending = (path_flag & PATH_RAY_PSR) != 0;

  if (psr_enabled && is_first_bounce && !psr_pending && !has_transmission &&
      transparent_weight < 1e-4f && sum_weight > 0.0f && (sd->flag & SD_BSDF) &&
      !(sd->flag & SD_BSDF_HAS_EVAL))
  {
    const float3 mirror_n = safe_normalize(normal);

    if (!is_zero(mirror_n)) {
      INTEGRATOR_STATE_WRITE(state, path, psr_mirror_n) = mirror_n;
      INTEGRATOR_STATE_WRITE(state, path, psr_mirror_d) = dot(sd->P, mirror_n);
      INTEGRATOR_STATE_WRITE(state, path, flag) |= PATH_RAY_PSR;

      /* The reflection is seen through the mirror's tint, so the albedo guides carry it. */
      if (reduce_max(fabs(specular_albedo)) > 1e-4f) {
        INTEGRATOR_STATE_WRITE(state, path, denoising_feature_throughput) *= specular_albedo;
      }

      /* Write nothing for the mirror itself. */
      return;
    }
  }

  if (psr_pending) {
    /* Purely transparent hit in the reflection: it is a hole, not the virtual image. Keep waiting
     * for what is behind it, the way the non-replaced path does. */
    if (sum_weight == 0.0f && transparent_weight > 1e-4f) {
      INTEGRATOR_STATE_WRITE(state, path, denoising_feature_throughput) *= transparent_albedo;
      return;
    }

    const float3 mirror_n = INTEGRATOR_STATE(state, path, psr_mirror_n);
    const float mirror_d = INTEGRATOR_STATE(state, path, psr_mirror_d);
    const float3 virtual_P = sd->P - 2.0f * mirror_n * (dot(sd->P, mirror_n) - mirror_d);

    /* Geometry (depth, normal, roughness, motion) describes where the virtual image is, so it is
     * written straight; only the albedos carry the mirror's throughput. */
    if (kernel_data.film.pass_denoising_depth != PASS_UNUSED) {
      film_write_pass_float(buffer + kernel_data.film.pass_denoising_depth,
                            ensure_finite(camera_z_depth(kg, virtual_P)));
    }

    if (kernel_data.film.pass_denoising_normal != PASS_UNUSED && sum_weight > 0.0f) {
      /* Householder reflection: the virtual image's surface faces the mirrored way. */
      const float3 virtual_normal = normal - 2.0f * mirror_n * dot(normal, mirror_n);
      const Transform worldtocamera = kernel_data.cam.worldtocamera;
      film_write_pass_float3(
          buffer + kernel_data.film.pass_denoising_normal,
          ensure_finite(transform_direction(&worldtocamera, safe_normalize(virtual_normal))));
    }

    if (kernel_data.film.pass_denoising_albedo != PASS_UNUSED) {
      film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_albedo,
                               ensure_finite(diffuse_albedo * denoising_feature_throughput));
    }

    if (kernel_data.film.pass_denoising_specular_albedo != PASS_UNUSED) {
      film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_specular_albedo,
                               ensure_finite(specular_albedo * denoising_feature_throughput));
    }

    if (kernel_data.film.pass_denoising_roughness != PASS_UNUSED) {
      film_write_pass_float(buffer + kernel_data.film.pass_denoising_roughness,
                            ensure_finite(specular_roughness));
    }

    if (kernel_data.film.pass_denoising_backward_motion != PASS_UNUSED) {
      const float3 backward_motion = primitive_motion_vector_backward_depth_delta_psr(
          kg, sd, mirror_n, mirror_d);
      film_write_pass_float3(buffer + kernel_data.film.pass_denoising_backward_motion,
                             backward_motion);
    }

    /* One replacement per path: a mirror seen in a mirror keeps the first virtual image rather
     * than chasing the chain. */
    INTEGRATOR_STATE_WRITE(state, path, flag) &= ~(PATH_RAY_PSR | PATH_RAY_DENOISING_FEATURES);
    return;
  }

  if (kernel_data.film.pass_denoising_depth != PASS_UNUSED &&
      (write_as_first || follow_reflections))
  {
    /* Behind smooth glass the depth is the surface's own camera depth (the guide throughput only
     * tints the albedos, as in the primary surface replacement above); the running ray-length
     * difference is for first hits and followed reflections. */
    const float denoising_depth = (write_as_first && !is_first_bounce) ?
                                      ensure_finite(camera_z_depth(kg, sd->P)) :
                                      denoising_depth_compute(kg,
                                                              state,
                                                              sd,
                                                              denoising_feature_throughput,
                                                              follow_reflections);
    film_write_pass_float(buffer + kernel_data.film.pass_denoising_depth, denoising_depth);
  }

  if (kernel_data.film.pass_denoising_normal != PASS_UNUSED && feature_weight > 0.0f &&
      (write_as_first || follow_reflections))
  {
    /* Transform normal into camera space. */
    const Transform worldtocamera = kernel_data.cam.worldtocamera;
    float3 denoising_normal = transform_direction(&worldtocamera, normal);
    const float opaque_fraction = (total_weight > 0.0f) ? (sum_weight / total_weight) : 1.0f;

    denoising_normal = ensure_finite(denoising_normal * opaque_fraction * feature_weight *
                                     average(denoising_feature_throughput));
    film_write_pass_float3(buffer + kernel_data.film.pass_denoising_normal, denoising_normal);
  }

  if (kernel_data.film.pass_denoising_albedo != PASS_UNUSED && feature_weight > 0.0f &&
      (write_as_first || follow_reflections))
  {
    const Spectrum denoising_albedo = ensure_finite(diffuse_albedo * feature_weight *
                                                    denoising_feature_throughput);
    film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_albedo, denoising_albedo);
  }

  if (write_as_first) {
    if (kernel_data.film.pass_denoising_roughness != PASS_UNUSED) {
      const float denoising_roughness = ensure_finite(specular_roughness *
                                                      average(denoising_feature_throughput));
      film_write_pass_float(buffer + kernel_data.film.pass_denoising_roughness,
                            denoising_roughness);
    }

    if (kernel_data.film.pass_denoising_specular_albedo != PASS_UNUSED) {
      const Spectrum denoising_specular_albedo = ensure_finite(specular_albedo *
                                                               denoising_feature_throughput);
      film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_specular_albedo,
                               denoising_specular_albedo);
    }

    if (kernel_data.film.pass_denoising_backward_motion != PASS_UNUSED) {
      const float3 backward_motion = primitive_motion_vector_backward_depth_delta(kg, sd);
      film_write_pass_float3(buffer + kernel_data.film.pass_denoising_backward_motion,
                             backward_motion);
    }
  }

  if (glass_through_now) {
    /* The surface behind the glass has written; one replacement per path. */
    INTEGRATOR_STATE_WRITE(state, path, flag) &= ~PATH_RAY_GLASS_THROUGH;
  }

  /* Portion deferred to the next bounce. Specularity uses the feature weight, transparent
   * always passes through. */
  const Spectrum deferred_albedo = specular_albedo * (1.0f - feature_weight) + transparent_albedo;

  if (reduce_max(fabs(deferred_albedo)) > 1e-4f) {
    INTEGRATOR_STATE_WRITE(state, path, denoising_feature_throughput) *= deferred_albedo;
  }
  else {
    INTEGRATOR_STATE_WRITE(state, path, flag) &= ~PATH_RAY_DENOISING_FEATURES;
  }
}

/* Falcon glass through-guides: called right after the BSDF lobe of a smooth glass hit has been
 * sampled (and before the path state advances, so the bounce count is still that of the glass).
 * A refracting sample carries on to the surface behind, which writes for it; anything else writes
 * the glass surface's own guides now. */
ccl_device_forceinline void film_write_denoising_glass_resolve(
    KernelGlobals kg,
    IntegratorState state,
    const ccl_private ShaderData *sd,
    const ccl_private ShaderClosure *sc,
    const int label,
    ccl_global float *ccl_restrict render_buffer)
{
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  if (!(path_flag & PATH_RAY_GLASS_PENDING)) {
    return;
  }

  const bool through = (label & LABEL_TRANSMIT) && !(label & LABEL_TRANSPARENT);
  INTEGRATOR_STATE_WRITE(state, path, flag) = (path_flag & ~PATH_RAY_GLASS_PENDING) |
                                              PATH_RAY_GLASS_SEEN |
                                              (through ? PATH_RAY_GLASS_THROUGH : 0u);

  if (through) {
    /* The glass's colour tints what is seen through it. The closure's weight also carries its
     * share of a mixture (glass next to a diffuse layer), and that share is already the
     * probability with which this closure was picked, so it is divided out: for a lone glass
     * closure the share is 1 and the tint is its colour. */
    float sample_weight_sum = 0.0f;
    for (int i = 0; i < sd->num_closure; i++) {
      if (CLOSURE_IS_BSDF_OR_BSSRDF(sd->closure[i].type)) {
        sample_weight_sum += sd->closure[i].sample_weight;
      }
    }
    const float share = (sample_weight_sum > 0.0f) ? sc->sample_weight / sample_weight_sum : 1.0f;
    if (share > 1e-4f) {
      INTEGRATOR_STATE_WRITE(state, path, denoising_feature_throughput) *= sc->weight *
                                                                          (1.0f / share);
    }
  }
  else {
    film_write_denoising_features_surface(kg, state, sd, render_buffer, true);
  }
}

ccl_device_forceinline void film_write_denoising_features_surface_volume(
    KernelGlobals kg,
    IntegratorState state,
    const ccl_private ShaderData *sd,
    ccl_global float *ccl_restrict render_buffer)
{
  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  const bool follow_reflections = (kernel_data.film.denoising_pass_options_flag &
                                   DENOISING_PASS_FOLLOW_REFLECTIONS) != 0;
  const bool is_first_bounce = INTEGRATOR_STATE(state, path, bounce) == 0;

  if (kernel_data.film.pass_denoising_depth != PASS_UNUSED &&
      (is_first_bounce || follow_reflections))
  {
    const Spectrum denoising_feature_throughput = INTEGRATOR_STATE(
        state, path, denoising_feature_throughput);

    const float denoising_depth = denoising_depth_compute(
        kg, state, sd, denoising_feature_throughput, follow_reflections);
    film_write_pass_float(buffer + kernel_data.film.pass_denoising_depth, denoising_depth);
  }
}

ccl_device_forceinline void film_write_denoising_features_volume(KernelGlobals kg,
                                                                 IntegratorState state,
                                                                 const Spectrum albedo,
                                                                 const bool scatter,
                                                                 ccl_global float *ccl_restrict
                                                                     render_buffer)
{
  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);
  const Spectrum denoising_feature_throughput = INTEGRATOR_STATE(
      state, path, denoising_feature_throughput);

  if (scatter && kernel_data.film.pass_denoising_normal != PASS_UNUSED) {
    /* Assume scatter is sufficiently diffuse to stop writing denoising features. */
    INTEGRATOR_STATE_WRITE(state, path, flag) &= ~PATH_RAY_DENOISING_FEATURES;

    /* Write view direction as normal. */
    const float3 denoising_normal = make_float3(0.0f, 0.0f, -1.0f);
    film_write_pass_float3(buffer + kernel_data.film.pass_denoising_normal, denoising_normal);
  }

  if (kernel_data.film.pass_denoising_albedo != PASS_UNUSED) {
    /* Write albedo. */
    const Spectrum denoising_albedo = ensure_finite(denoising_feature_throughput * albedo);
    film_write_pass_spectrum(buffer + kernel_data.film.pass_denoising_albedo, denoising_albedo);
  }
}

ccl_device_forceinline void film_write_denoising_features_background(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
  if (!(path_flag & PATH_RAY_DENOISING_FEATURES)) {
    return;
  }

  /* A mirror reflecting the environment: the virtual image sits effectively at infinity. Without
   * this the held-back depth is never written and the mirror reads as sitting on the camera. */
  if ((path_flag & PATH_RAY_PSR) && INTEGRATOR_STATE(state, path, bounce) == 1) {
    ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

    if (kernel_data.film.pass_denoising_depth != PASS_UNUSED) {
      film_write_pass_float(buffer + kernel_data.film.pass_denoising_depth,
                            kernel_data.film.specular_hit_distance_far);
    }

    INTEGRATOR_STATE_WRITE(state, path, flag) &= ~(PATH_RAY_PSR | PATH_RAY_DENOISING_FEATURES);
    return;
  }

  /* Falcon glass through-guides: a sample that refracted through smooth glass and left the scene
   * sees the same sky a primary ray would, so it writes the same far depth. */
  if (path_flag & PATH_RAY_GLASS_THROUGH) {
    ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

    if (kernel_data.film.pass_denoising_depth != PASS_UNUSED) {
      film_overwrite_pass_float(buffer + kernel_data.film.pass_denoising_depth, FLT_MAX);
    }

    /* The sky's albedo is written after this, and needs the flag; the path ends here. */
    INTEGRATOR_STATE_WRITE(state, path, flag) &= ~PATH_RAY_GLASS_THROUGH;
    return;
  }

  /* Do not write default background denoising data for secondary paths. */
  if (INTEGRATOR_STATE(state, path, bounce) != 0) {
    return;
  }

  ccl_global float *buffer = film_pass_pixel_render_buffer(kg, state, render_buffer);

  if (kernel_data.film.pass_denoising_depth != PASS_UNUSED) {
    film_overwrite_pass_float(buffer + kernel_data.film.pass_denoising_depth, FLT_MAX);
  }

  /* 'pass_denoising_albedo' is written by 'film_write_emission_or_background_pass' */
}
#endif /* __DENOISING_FEATURES__ */

CCL_NAMESPACE_END
