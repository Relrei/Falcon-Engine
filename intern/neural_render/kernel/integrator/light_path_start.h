/* SPDX-FileCopyrightText: 2026 Falcon Engine
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Falcon Cycles-expansion: the start of a light path (the scheme is in
 * kernel/integrator/init_from_light.h). Apart from it as the world's light paths evaluate the
 * background shader, and kernel/integrator/surface_shader.h includes init_from_light.h. */

#pragma once

#include "kernel/integrator/init_from_light.h"
#include "kernel/integrator/surface_shader.h"

#include "kernel/light/background.h"
#include "kernel/light/triangle.h"

#include "kernel/geom/shader_data.h"

CCL_NAMESPACE_BEGIN

/* A direction towards the world, as its light samples them (kernel/light/background.h
 * background_light_sample()) without the portals, which belong to a point in the scene: the sun
 * of a sky texture and the importance map, with MIS between the two; uniform without either. */
ccl_device float3 light_path_world_direction(KernelGlobals kg,
                                             float2 rand,
                                             ccl_private float *pdf)
{
  float sun_pdf = kernel_data.background.sun_weight;
  float map_pdf = (kernel_data.background.map_res_x > 0 && kernel_data.background.map_res_y > 0) ?
                      kernel_data.background.map_weight :
                      0.0f;
  const float total = sun_pdf + map_pdf;
  if (!(total > 0.0f)) {
    *pdf = 1.0f / M_4PI_F;
    return sample_uniform_sphere(rand);
  }
  sun_pdf /= total;
  map_pdf /= total;
  float3 D;
  if (rand.x < sun_pdf) {
    rand.x /= sun_pdf;
    D = background_sun_sample(kg, rand, pdf);
    *pdf *= sun_pdf;
    if (map_pdf > 0.0f) {
      *pdf += map_pdf * background_map_pdf(kg, D);
    }
  }
  else {
    rand.x = (rand.x - sun_pdf) / map_pdf;
    D = background_map_sample(kg, rand, pdf);
    *pdf *= map_pdf;
    if (sun_pdf > 0.0f) {
      *pdf += sun_pdf * background_sun_pdf(kg, D);
    }
  }
  return D;
}

/* Sample a ray leaving the light: origin, direction and the flux it carries divided by the
 * probability of the origin and direction (not by the light pick). */
ccl_device bool light_trace_emit(KernelGlobals kg,
                                 const ccl_global KernelLight *klight,
                                 const float2 rand_pos,
                                 const float2 rand_dir,
                                 ccl_private float3 *r_P,
                                 ccl_private float3 *r_D,
                                 ccl_private Spectrum *r_flux)
{
  const Spectrum strength = rgb_to_spectrum(
      make_float3(klight->strength[0], klight->strength[1], klight->strength[2]));
  if (is_zero(strength)) {
    return false;
  }

  switch (klight->type) {
    case LIGHT_POINT: {
      const float radius = klight->spot.radius;
      if (radius > 0.0f) {
        /* A Lambertian sphere has the same radiant intensity in every direction: a uniform
         * direction, then a uniform point on the sphere's disk seen from that direction (projected
         * area pi r^2), moved onto the sphere's surface. The flux is radiance * pi * area, as with
         * a point on the sphere and a cosine-weighted direction, but the direction alone decides
         * where the path goes, which the light tracing guide learns (rand_dir). */
        const float3 D = sample_uniform_sphere(rand_dir);
        float3 T, B;
        make_orthonormals(D, &T, &B);
        const float2 d = sample_uniform_disk(rand_pos) * radius;
        const float h = safe_sqrtf(sqr(radius) - dot(d, d));
        *r_D = D;
        *r_P = klight->co + T * d.x + B * d.y + D * h;
        *r_flux = strength * klight->spot.eval_fac * M_PI_F * (4.0f * M_PI_F * sqr(radius));
      }
      else {
        /* Radiant intensity strength * eval_fac in every direction. */
        *r_P = klight->co;
        *r_D = sample_uniform_sphere(rand_dir);
        *r_flux = strength * klight->spot.eval_fac * (4.0f * M_PI_F);
      }
      return true;
    }
    case LIGHT_SPOT: {
      /* As a point light, only in the cone, with the spot falloff. The paths start at the centre
       * (the radius only scales the intensity below). */
      const float one_minus_cos = 1.0f - klight->spot.cos_half_spot_angle;
      if (one_minus_cos <= 0.0f) {
        return false;
      }
      float cos_theta;
      float pdf_dir;
      const float3 D = sample_uniform_cone(
          klight->spot.dir, one_minus_cos, rand_dir, &cos_theta, &pdf_dir);
      const float attenuation = spot_light_attenuation(&klight->spot,
                                                       spot_light_to_local(kg, klight, D));
      if (attenuation <= 0.0f || pdf_dir <= 0.0f) {
        return false;
      }
      *r_P = klight->co;
      *r_D = D;
      /* Radiant intensity: with a radius, eval_fac is the radiance of the sphere (it holds 1 / area,
       * see PointLight::copy_to_kernel), so times the projected area pi r^2, as the point light
       * above does; without one it is already the intensity. Leaving the radius out made a spot of
       * radius r 1 / (pi r^2) times too bright (800 times at 2 cm). */
      const float radius = klight->spot.radius;
      const float intensity_fac = (radius > 0.0f) ? klight->spot.eval_fac * M_PI_F * sqr(radius) :
                                                    klight->spot.eval_fac;
      *r_flux = strength * intensity_fac * attenuation / pdf_dir;
      return true;
    }
    case LIGHT_AREA: {
      /* One-sided Lambertian rectangle or ellipse facing area.dir. */
      const float3 ru = klight->area.axis_u * (klight->area.len_u * 0.5f);
      const float3 rv = klight->area.axis_v * (klight->area.len_v * 0.5f);
      const bool is_ellipse = klight->area.invarea < 0.0f;
      const float area = is_ellipse ? M_PI_4_F * klight->area.len_u * klight->area.len_v :
                                      klight->area.len_u * klight->area.len_v;
      if (area <= 0.0f) {
        return false;
      }
      *r_P = klight->co + (is_ellipse ? ellipse_sample(ru, rv, rand_pos) :
                                        rectangle_sample(ru, rv, rand_pos));
      float pdf_dir;
      sample_cos_hemisphere(klight->area.dir, rand_dir, r_D, &pdf_dir);
      /* Radiance strength * |invarea| / pi, flux radiance * pi * area. */
      *r_flux = strength * fabsf(klight->area.invarea) * area;
      return true;
    }
    case LIGHT_SUN: {
      /* Parallel rays (inside the sun's cone) from a disk that covers the scene's bounding
       * sphere. Irradiance strength * eval_fac over the disk area. */
      const float radius = kernel_data.integrator.bidirectional_scene_radius;
      if (radius <= 0.0f) {
        return false;
      }
      float cos_theta;
      float pdf_dir;
      /* klight->co is the direction the light travels (scene/light.cpp: minus the object Z
       * axis; kernel/light/sun.h samples Ng around it and gives D = -Ng towards the sun). */
      const float3 D = sample_uniform_cone(
          klight->co, klight->sun.one_minus_cosangle, rand_dir, &cos_theta, &pdf_dir);
      float3 T, B;
      make_orthonormals(D, &T, &B);
      const float2 disk = sample_uniform_disk(rand_pos) * radius;
      const float3 center = make_float3(kernel_data.integrator.bidirectional_scene_center_x,
                                        kernel_data.integrator.bidirectional_scene_center_y,
                                        kernel_data.integrator.bidirectional_scene_center_z);
      *r_P = center - D * radius + T * disk.x + B * disk.y;
      *r_D = D;
      /* eval_fac is the radiance of the sun disk: the irradiance is radiance times the disk's solid
       * angle 2 pi (1 - cos(half angle)); a sun of angle 0 has eval_fac 1 as the irradiance. */
      const float one_minus_cos = klight->sun.one_minus_cosangle;
      const float irradiance_fac = (one_minus_cos > 0.0f) ?
                                       klight->sun.eval_fac * M_2PI_F * one_minus_cos :
                                       klight->sun.eval_fac;
      *r_flux = strength * irradiance_fac * (M_PI_F * sqr(radius));
      return true;
    }
    case LIGHT_BACKGROUND: {
      /* Parallel rays from a disk that covers the scene's bounding sphere, as for a sun, in the
       * directions the world's light samples (light_path_world_direction()). The flux of a direction is
       * radiance * disk area / pdf: the caller multiplies in the radiance, which takes the
       * background shader (light_path_world_radiance()). */
      const float radius = kernel_data.integrator.bidirectional_scene_radius;
      if (radius <= 0.0f) {
        return false;
      }
      float pdf_dir;
      const float3 to_world = light_path_world_direction(kg, rand_dir, &pdf_dir);
      if (!(pdf_dir > 0.0f)) {
        return false;
      }
      const float3 D = -to_world;
      float3 T, B;
      make_orthonormals(D, &T, &B);
      const float2 disk = sample_uniform_disk(rand_pos) * radius;
      const float3 center = make_float3(kernel_data.integrator.bidirectional_scene_center_x,
                                        kernel_data.integrator.bidirectional_scene_center_y,
                                        kernel_data.integrator.bidirectional_scene_center_z);
      *r_P = center - D * radius + T * disk.x + B * disk.y;
      *r_D = D;
      *r_flux = make_spectrum(M_PI_F * sqr(radius) / pdf_dir);
      return true;
    }
    default:
      return false;
  }
}

/* The world's radiance arriving along -to_world, from the background shader as a ray leaving the
 * scene towards to_world sees it (shade_background.h integrator_eval_background_shader()). Not a
 * camera ray: a world that shows the camera something else than it lights with gives its light. */
ccl_device Spectrum light_path_world_radiance(KernelGlobals kg,
                                              IntegratorState state,
                                              const float3 P,
                                              const float3 to_world)
{
  const int shader = kernel_data.background.surface_shader;
  Spectrum L = zero_spectrum();
  if (surface_shader_constant_emission(kg, shader, &L)) {
    return L;
  }
  ShaderDataTinyStorage emission_sd_storage;
  ccl_private ShaderData *emission_sd = AS_SHADER_DATA(&emission_sd_storage);
  shader_setup_from_background(kg, emission_sd, P, to_world, 0.0f, 0.5f);
  surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_BACKGROUND>(
      kg,
      state,
      emission_sd,
      nullptr,
      PATH_RAY_VISIBILITY_DIFFUSE | PATH_RAY_VISIBILITY_GLOSSY | PATH_RAY_VISIBILITY_TRANSMIT,
      PATH_RAY_NR_LIGHT_PATH | PATH_RAY_EMISSION);
  if (emission_sd->runtime_flag & SR_CACHE_MISS) {
    /* A texture not loaded yet: this path is not sent (the later samples have it). */
    return zero_spectrum();
  }
  return surface_shader_background(emission_sd);
}

/* A start point on the emissive triangles (scene/light.cpp device_update_nr_mesh_lights()): a
 * triangle picked with `pick` in proportion to its area and emission estimate, a uniform point on
 * it, an emitting side among the ones its light sampling uses (SD_MIS_FRONT, SD_MIS_BACK), and a
 * cosine weighted direction from that side. The flux without the radiance, which takes the emission
 * shader (light_path_mesh_radiance()): pi * area * sides / the triangle's probability. */
ccl_device bool light_path_mesh_sample(KernelGlobals kg,
                                       const float pick,
                                       const float2 rand_pos,
                                       float2 rand_dir,
                                       ccl_private float3 *r_P,
                                       ccl_private float3 *r_D,
                                       ccl_private float3 *r_Ng,
                                       ccl_private Spectrum *r_flux,
                                       ccl_private int *r_object,
                                       ccl_private int *r_prim,
                                       ccl_private float2 *r_uv,
                                       ccl_private int *r_shader)
{
  const int count = kernel_data.integrator.nr_mesh_light_count;
  int lo = 0;
  int hi = count - 1;
  while (lo < hi) {
    const int mid = (lo + hi) >> 1;
    if (kernel_data_fetch(nr_mesh_light_cdf, mid + 1) > pick) {
      hi = mid;
    }
    else {
      lo = mid + 1;
    }
  }
  const float p_tri = kernel_data_fetch(nr_mesh_light_cdf, lo + 1) -
                      kernel_data_fetch(nr_mesh_light_cdf, lo);
  if (!(p_tri > 0.0f)) {
    return false;
  }
  const int prim = int(kernel_data_fetch(nr_mesh_light_prims, 2 * lo));
  const int object = int(kernel_data_fetch(nr_mesh_light_prims, 2 * lo + 1));

  float3 V[3];
  triangle_world_space_vertices(kg, object, prim, 0.5f, V);
  float3 N0 = cross(V[1] - V[0], V[2] - V[0]);
  if (kernel_data_fetch(object_flag, object) & SD_OBJECT_NEGATIVE_SCALE) {
    N0 = -N0;
  }
  float len;
  const float3 Ng = safe_normalize_len(N0, &len);
  const float area = 0.5f * len;
  if (!(area > 0.0f)) {
    return false;
  }

  const int shader = kernel_data_fetch(tri_shader, prim);
  const int shader_flag = kernel_data_fetch(shaders, shader & SHADER_MASK).flags;
  const bool front = (shader_flag & SD_MIS_FRONT) != 0;
  const bool back = (shader_flag & SD_MIS_BACK) != 0;
  if (!front && !back) {
    return false;
  }
  bool use_back = back;
  if (front && back) {
    use_back = rand_dir.x >= 0.5f;
    rand_dir.x = use_back ? (rand_dir.x - 0.5f) * 2.0f : rand_dir.x * 2.0f;
    rand_dir.x = min(rand_dir.x, 0.99999994f);
  }

  /* Eric Heitz's "A Low-Distortion Map Between Triangle and Square", as kernel/light/triangle.h. */
  float u = rand_pos.x;
  float v = rand_pos.y;
  if (v > u) {
    u *= 0.5f;
    v -= u;
  }
  else {
    v *= 0.5f;
    u -= v;
  }
  *r_P = (1.0f - u - v) * V[0] + u * V[1] + v * V[2];

  float pdf_dir;
  sample_cos_hemisphere(use_back ? -Ng : Ng, rand_dir, r_D, &pdf_dir);
  if (!(pdf_dir > 0.0f)) {
    return false;
  }
  *r_Ng = Ng;
  *r_flux = make_spectrum(M_PI_F * area * float(int(front) + int(back)) / p_tri);
  *r_object = object;
  *r_prim = prim;
  *r_uv = make_float2(u, v);
  *r_shader = shader;
  return true;
}

/* The emission leaving a point of an emissive triangle along D, from its shader as a ray arriving
 * there from D's way sees it (kernel/integrator/shade_surface.h integrate_surface_emission()). */
ccl_device Spectrum light_path_mesh_radiance(KernelGlobals kg,
                                             IntegratorState state,
                                             const float3 P,
                                             const float3 Ng,
                                             const float3 D,
                                             const int shader,
                                             const int object,
                                             const int prim,
                                             const float2 uv)
{
  Spectrum L = zero_spectrum();
  if (surface_shader_constant_emission(kg, shader, &L)) {
    return L;
  }
  ShaderDataTinyStorage emission_sd_storage;
  ccl_private ShaderData *emission_sd = AS_SHADER_DATA(&emission_sd_storage);
  shader_setup_from_sample(
      kg, emission_sd, P, Ng, D, shader, object, prim, uv.x, uv.y, 0.0f, 0.5f, false, false);
  surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_LIGHT>(
      kg,
      state,
      emission_sd,
      nullptr,
      PATH_RAY_VISIBILITY_DIFFUSE | PATH_RAY_VISIBILITY_GLOSSY | PATH_RAY_VISIBILITY_TRANSMIT,
      PATH_RAY_NR_LIGHT_PATH | PATH_RAY_EMISSION);
  if (emission_sd->runtime_flag & SR_CACHE_MISS) {
    return zero_spectrum();
  }
  return surface_shader_emission(emission_sd);
}

/* Light tracing guide (NR_LIGHT_GUIDE_*, kernel/types.h): warp the 2D random numbers that place
 * the light path with the light's learnt table. Half the paths keep the uniform numbers (a
 * defensive mixture: caustics not found yet keep being found), so the density in the unit square is
 * 0.5 + 0.5 * cells * p(cell), and the flux is divided by it. Returns the density and the cell. */
ccl_device float light_trace_guide_warp(KernelGlobals kg,
                                        const int light_index,
                                        const float pick_guided,
                                        ccl_private float2 *u,
                                        ccl_private uint *r_cell)
{
  *r_cell = NR_LIGHT_GUIDE_NONE;
  /* NR_LT_DEBUG & 128 (diagnostics, 11-16): no guide, the light paths start uniformly. */
  if (light_index >= NR_LIGHT_GUIDE_MAX_LIGHTS ||
      kernel_data_array(nr_light_guide_cdf) == nullptr ||
      (kernel_data.integrator.bidirectional_pad1 & 128))
  {
    return 1.0f;
  }
  const int base = light_index * NR_LIGHT_GUIDE_CELLS;
  const ccl_global float *cdf = kernel_data_array(nr_light_guide_cdf) + base;
  const bool learnt = cdf[NR_LIGHT_GUIDE_CELLS - 1] > 0.0f;

  if (learnt && pick_guided >= 0.5f) {
    /* Pick a cell with u->x on the cumulative distribution, reuse its remainder inside the cell. */
    int lo = 0;
    int hi = NR_LIGHT_GUIDE_CELLS - 1;
    while (lo < hi) {
      const int mid = (lo + hi) >> 1;
      if (cdf[mid] > u->x) {
        hi = mid;
      }
      else {
        lo = mid + 1;
      }
    }
    const float c0 = (lo > 0) ? cdf[lo - 1] : 0.0f;
    const float p = cdf[lo] - c0;
    const float fx = (p > 0.0f) ? clamp((u->x - c0) / p, 0.0f, 0.99999994f) : 0.5f;
    const int cx = lo % NR_LIGHT_GUIDE_RES;
    const int cy = lo / NR_LIGHT_GUIDE_RES;
    *u = make_float2((float(cx) + fx) / NR_LIGHT_GUIDE_RES, (float(cy) + u->y) / NR_LIGHT_GUIDE_RES);
  }

  const int cx = min(int(u->x * NR_LIGHT_GUIDE_RES), NR_LIGHT_GUIDE_RES - 1);
  const int cy = min(int(u->y * NR_LIGHT_GUIDE_RES), NR_LIGHT_GUIDE_RES - 1);
  const int cell = cy * NR_LIGHT_GUIDE_RES + cx;
  *r_cell = uint(base + cell);
  if (!learnt) {
    return 1.0f;
  }
  const float p_cell = cdf[cell] - ((cell > 0) ? cdf[cell - 1] : 0.0f);
  return 0.5f + 0.5f * float(NR_LIGHT_GUIDE_CELLS) * p_cell;
}

/* The light path's first ray. Leave the light's own surface: the lamps are intersected without a
 * self check for light rays, and a hit on the emitting lamp ends the path
 * (integrator_shade_light()). */
ccl_device_inline void light_path_init_ray(IntegratorState state, const float3 P, const float3 D)
{
  Ray ray ccl_optional_struct_init;
  ray.P = P + D * (1e-5f * (1.0f + reduce_max(fabs(P))));
  ray.D = D;
  ray.tmin = 0.0f;
  ray.tmax = FLT_MAX;
  ray.time = 0.5f;
#ifdef __RAY_DIFFERENTIALS__
  ray.dP = differential_zero_compact();
  ray.dD = differential_zero_compact();
#endif
  integrator_state_write_ray(state, &ray);
}

/* Not a camera ray: no camera visibility, no denoising features, no passes. Not a shadow ray
 * either: surface_shader_eval() gives a shadow ray no closures. The chain starts empty, which
 * counts as all specular. */
ccl_device_inline void light_path_init_flags(IntegratorState state, const uint guide_cell)
{
  INTEGRATOR_STATE_WRITE(state, path, visibility) = PATH_RAY_VISIBILITY_DIFFUSE |
                                                    PATH_RAY_VISIBILITY_GLOSSY |
                                                    PATH_RAY_VISIBILITY_TRANSMIT;
  INTEGRATOR_STATE_WRITE(state, path, flag) = PATH_RAY_NR_LIGHT_PATH | PATH_RAY_NR_CAUSTIC_CHAIN |
                                              PATH_RAY_MIS_SKIP;
  INTEGRATOR_STATE_WRITE(state, path, nr_light_guide_cell) = guide_cell;
}

/* A light path may start inside a volume (a spot light in fog): with volumes in the scene it first
 * finds the volumes around its start, as camera paths inside a volume do (11-16, light tracing
 * through volumes, kernel/integrator/shade_volume.h integrate_light_path_volume()). */
ccl_device_inline DeviceKernel light_path_first_kernel(KernelGlobals kg)
{
  return (kernel_data.kernel_features & KERNEL_FEATURE_VOLUME) ?
             DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK :
             DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST;
}

ccl_device bool integrator_init_from_light(KernelGlobals kg,
                                           IntegratorState state,
                                           const ccl_global KernelWorkTile *ccl_restrict tile,
                                           ccl_global float *render_buffer,
                                           const int x,
                                           const int y,
                                           const int scheduled_sample)
{
  PROFILING_INIT(kg, PROFILING_RAY_SETUP);
  (void)render_buffer;

  /* Basic buffer access (the render pixel index is only used for the random numbers, the light
   * path writes through its camera connections). The sample is not counted in the film. */
  path_state_init(state, tile, x, y);

  /* The lights, then one slot for all the emissive triangles. */
  const int num_lights = kernel_data.integrator.num_lights;
  const int num_slots = num_lights + (light_trace_mesh_emitted(kg) ? 1 : 0);
  if (!caustic_chains_tracked(kg) || num_slots == 0) {
    return false;
  }

  const int sample = scheduled_sample + tile->sample_offset;
  /* Other random numbers than the camera path of the same pixel and sample. */
  const uint rng_pixel = hash_uint2(path_rng_pixel_init(kg, sample, x, y), 0x4c544e52u);

  const float2 rand_a = path_rng_2D(kg, rng_pixel, sample, PRNG_FILTER);
  const float3 rand_b = path_rng_3D(kg, rng_pixel, sample, PRNG_LENS_TIME);

  /* Pick a light uniformly; the rest of the first number places the point on it. Lights of a type
   * the light tracer does not handle give nothing, which keeps the estimate of the others right
   * with the pick probability 1 / num_slots. */
  const float pick = rand_a.x * num_slots;
  const int light_index = min(int(pick), num_slots - 1);
  float2 rand_pos = make_float2(pick - float(light_index), rand_a.y);
  float2 rand_dir = make_float2(rand_b.y, rand_b.z);

  float3 P;
  float3 D;
  Spectrum flux;
  if (light_index == num_lights) {
    /* The emissive triangles, without the guide: rand_b.x picks the triangle. */
    float3 Ng;
    int object;
    int prim;
    float2 uv;
    int shader;
    if (!light_path_mesh_sample(
            kg, rand_b.x, rand_pos, rand_dir, &P, &D, &Ng, &flux, &object, &prim, &uv, &shader))
    {
      return false;
    }
    flux *= float(num_slots);
    light_path_init_ray(state, P, D);
    path_state_init_integrator(kg, state, sample, rng_pixel, flux);
    /* After the state's initialisation: the shader's Light Path node reads it. */
    const Spectrum L = light_path_mesh_radiance(kg, state, P, Ng, D, shader, object, prim, uv);
    if (is_zero(L) || !isfinite_safe(average(L))) {
      return false;
    }
    INTEGRATOR_STATE_WRITE(state, path, throughput) = flux * L;
    light_path_init_flags(state, NR_LIGHT_GUIDE_NONE);
    /* Leave the triangle the path starts on. */
    INTEGRATOR_STATE_WRITE(state, isect, object) = object;
    INTEGRATOR_STATE_WRITE(state, isect, prim) = prim;
    integrator_path_init(state, light_path_first_kernel(kg));
    return true;
  }

  const ccl_global KernelLight *klight = &kernel_data_fetch(lights, light_index);
  if (!light_trace_light_supported(kg, klight->type)) {
    return false;
  }

  /* Guide the numbers that decide where the path goes: the position on the disk of a sun or the
   * world (their directions come from elsewhere), the direction for the other lights. rand_b.x picks
   * guided or uniform. */
  const bool guide_position = (klight->type == LIGHT_SUN || klight->type == LIGHT_BACKGROUND);
  uint guide_cell;
  const float guide_pdf = light_trace_guide_warp(
      kg, light_index, rand_b.x, guide_position ? &rand_pos : &rand_dir, &guide_cell);

  if (!light_trace_emit(kg, klight, rand_pos, rand_dir, &P, &D, &flux)) {
    return false;
  }
  flux *= float(num_slots) / guide_pdf;

  light_path_init_ray(state, P, D);
  path_state_init_integrator(kg, state, sample, rng_pixel, flux);

  if (klight->type == LIGHT_BACKGROUND) {
    /* After the state's initialisation: the shader's Light Path node reads it. */
    const Spectrum L = light_path_world_radiance(kg, state, P, -D);
    if (!light_trace_world_radiance_emitted(kg, L) || !isfinite_safe(average(L))) {
      return false;
    }
    INTEGRATOR_STATE_WRITE(state, path, throughput) = flux * L;
  }

  light_path_init_flags(state, guide_cell);
  /* The light itself is not an object to skip: a light ray leaves its surface. */
  INTEGRATOR_STATE_WRITE(state, isect, object) = OBJECT_NONE;
  INTEGRATOR_STATE_WRITE(state, isect, prim) = PRIM_NONE;

  integrator_path_init(state, light_path_first_kernel(kg));
  return true;
}

CCL_NAMESPACE_END
