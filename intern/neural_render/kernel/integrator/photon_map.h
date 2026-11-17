/* SPDX-FileCopyrightText: 2026 Falcon Engine
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Falcon Cycles-expansion photon mapping for caustics (LuxCore's PhotonGI caustic cache as the model; written for the Cycles kernel).
 *
 * At the start of a frame a photon pass sends light paths (kernel/integrator/init_from_light.h) and
 * stores a photon where a path that went through one or more nearly specular bounces reaches a nearly
 * diffuse surface (L S+ D). PathTraceWorkGPU sorts them into a hash grid. A camera path whose bounces
 * so far were all specular gathers the photons around its first nearly diffuse vertex: that gives the
 * caustics seen through glass or water (E S+ D S+ L), which light tracing cannot connect to the camera.
 * On the first hit, light tracing takes the caustics when it is on (sharper, unbiased); photon mapping
 * takes them there too when it is off. The camera paths leave out what the technique takes
 * (light_trace_camera_path_skips_light()).
 *
 *   L = strength * sum over the photons within r of f(wi, wo) * power / (pi r^2) / light paths
 *
 * Each photon is 3 float4: position, incoming direction (towards where the light came from), power,
 * with the geometric normal in their w. */

#pragma once

#include "kernel/closure/bsdf.h"
#include "kernel/integrator/init_from_light.h"

CCL_NAMESPACE_BEGIN

/* Gather only the photons whose surface normal is within about 37 degrees of the gathering one. */
#define NR_PHOTON_NORMAL_MIN_COS 0.8f

/* The most photons a gather looks at (photon_map_gather()). */
#define NR_PHOTON_GATHER_MAX 8192u

/* A gather where more photons than this lie within the radius narrows its radius to about this many
 * (photon_map_gather()), down to NR_PHOTON_GATHER_MIN_RADIUS of it. The frame's radius is set for
 * about as many where the photons are on average (PathTraceWorkGPU::render_photons()). */
#define NR_PHOTON_GATHER_K 64.0f
#define NR_PHOTON_GATHER_MIN_RADIUS (1.0f / 32.0f)

ccl_device_inline uint photon_map_cell_hash(const int3 c, const uint size)
{
  return (uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u) % size;
}

ccl_device_inline int3 photon_map_cell(const float3 P, const float cell_size)
{
  return make_int3(int(floorf(P.x / cell_size)), int(floorf(P.y / cell_size)), int(floorf(P.z / cell_size)));
}

/* Whether the caustics arrive here for the camera from light tracing and photon mapping: a vertex
 * whose closures are all nearly diffuse reflection (LuxCore likewise keeps its caustic cache to
 * diffuse materials). A vertex mixing nearly specular closures, or with transmission (rough glass,
 * translucency), is left to path tracing entirely: the light paths do not end there and the
 * camera path keeps its caustics (11-15 junkshop: its jars, a principled with transmission from
 * a texture, rough in places, got the caustics behind them twice, +0.7% of the image). */
ccl_device_inline bool photon_map_diffuse_vertex(const ccl_private ShaderData *sd)
{
  bool any = false;
  for (int i = 0; i < sd->num_closure; i++) {
    const ccl_private ShaderClosure *sc = &sd->closure[i];
    if (!CLOSURE_IS_BSDF(sc->type) || !(sc->sample_weight > 0.0f)) {
      continue;
    }
    if (bsdf_get_specular_roughness_squared(sc) < NR_LIGHT_TRACE_SPECULAR_ALPHA_SQUARED ||
        CLOSURE_IS_BSDF_TRANSMISSION(sc->type) || CLOSURE_IS_GLASS(sc->type) ||
        sc->type == CLOSURE_BSDF_TRANSLUCENT_ID)
    {
      return false;
    }
    any = true;
  }
  return any;
}

/* Next event estimation at a vertex of a camera path's open caustic chain (after its first nearly
 * diffuse vertex, with only nearly specular bounces since): a light sample through a nearly
 * specular closure here makes L S+ D, which light tracing or photon mapping carries. The share the
 * camera path keeps is the one of the nearly diffuse closures in the BSDF sampling pdf towards the
 * light, as the BSDF sampled bounce keeps a light it reaches through them only (11-15: without it,
 * the glass and the low roughness glossy of junkshop got their caustics twice, +1.4% of the image). */
ccl_device float photon_map_nee_kept_share(KernelGlobals kg,
                                           ccl_private ShaderData *sd,
                                           const float3 wo)
{
  float pdf_all = 0.0f;
  float pdf_rough = 0.0f;
  for (int i = 0; i < sd->num_closure; i++) {
    const ccl_private ShaderClosure *sc = &sd->closure[i];
    if (!CLOSURE_IS_BSDF(sc->type) || !(sc->sample_weight > 0.0f)) {
      continue;
    }
    float pdf = 0.0f;
    bsdf_eval(kg, sd, sc, wo, &pdf);
    const float w = pdf * sc->sample_weight;
    pdf_all += w;
    if (bsdf_get_specular_roughness_squared(sc) >= NR_LIGHT_TRACE_SPECULAR_ALPHA_SQUARED) {
      pdf_rough += w;
    }
  }
  return (pdf_all > 0.0f) ? pdf_rough / pdf_all : 1.0f;
}

/* Photon pass: store a photon at this nearly diffuse vertex of a light path. */
ccl_device_forceinline void photon_map_store(KernelGlobals kg,
                                             ConstIntegratorState state,
                                             const ccl_private ShaderData *sd)
{
  if (!photon_map_diffuse_vertex(sd) || kernel_data_array(nr_photon_counter) == nullptr) {
    return;
  }
  const Spectrum power = INTEGRATOR_STATE(state, path, throughput);
  if (!(average(power) > 0.0f) || !isfinite_safe(average(power))) {
    return;
  }
  /* A full storage keeps a random part of the photons (reservoir sampling: the n-th takes a random
   * slot with probability capacity / n), which stands for all of them by the count found
   * (PathTraceWorkGPU::render_photons()). Keeping the first ones kept the short paths and gave too
   * bright caustics (11-15: the window test with 1000 photons asked for, 2.80 against 2.51). Two
   * photons taking a slot at the same time may mix theirs, seldom enough to leave. */
  const uint capacity = uint(kernel_data.integrator.nr_photon_capacity);
  uint index = atomic_fetch_and_add_uint32(&kernel_data_array(nr_photon_counter)[0], 1);
  if (index >= capacity) {
    index = hash_uint2(index, 0x5bd1e995u) % (index + 1);
    if (index >= capacity) {
      return;
    }
  }
  const float3 rgb = spectrum_to_rgb(power);
  ccl_global float4 *photon = kernel_data_array(nr_photons) + 3 * index;
  /* The geometric normal, facing where the photon came from, goes in the w of the three. */
  photon[0] = make_float4(sd->P.x, sd->P.y, sd->P.z, sd->Ng.x);
  photon[1] = make_float4(sd->wi.x, sd->wi.y, sd->wi.z, sd->Ng.y);
  photon[2] = make_float4(rgb.x, rgb.y, rgb.z, sd->Ng.z);

  /* The light tracing guide learns from the photons too. */
  const uint guide_cell = INTEGRATOR_STATE(state, path, nr_light_guide_cell);
  if (guide_cell != NR_LIGHT_GUIDE_NONE && kernel_data_array(nr_light_guide_hist) != nullptr) {
    atomic_add_and_fetch_float(&kernel_data_array(nr_light_guide_hist)[guide_cell], average(power));
  }
}

/* Camera path: the caustic radiance at this vertex from the photons around it, through its nearly
 * diffuse closures. */
ccl_device Spectrum photon_map_gather(KernelGlobals kg, ccl_private ShaderData *sd)
{
  const float r = kernel_data.integrator.nr_photon_radius;
  const uint size = uint(kernel_data.integrator.nr_photon_grid_size);
  if (!(r > 0.0f) || size == 0 || kernel_data_array(nr_photon_cells) == nullptr) {
    return zero_spectrum();
  }
  const float r2 = r * r;
  const int3 base = photon_map_cell(sd->P, r);
  const ccl_global uint *cells = kernel_data_array(nr_photon_cells);
  const ccl_global float4 *photons = kernel_data_array(nr_photons);

  /* The neighbouring cells, each bucket once: two cells hashed to the same bucket would count its
   * photons twice. */
  uint buckets[27];
  int num_buckets = 0;
  uint total = 0;
  for (int dz = -1; dz <= 1; dz++) {
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        const uint h = photon_map_cell_hash(make_int3(base.x + dx, base.y + dy, base.z + dz), size);
        bool seen = false;
        for (int b = 0; b < num_buckets; b++) {
          seen |= (buckets[b] == h);
        }
        if (!seen) {
          buckets[num_buckets++] = h;
          total += cells[h + 1] - cells[h];
        }
      }
    }
  }
  if (total == 0) {
    return zero_spectrum();
  }

  /* At most NR_PHOTON_GATHER_MAX photons are looked at: with more (the core of a focused caustic),
   * every stride-th one from a random start, weighted by the stride. Cutting the rest off lost their
   * light (11-15: the caustic seen through the window test's glass ball, 1.74 against 2.99). */
  const uint stride = (total + NR_PHOTON_GATHER_MAX - 1) / NR_PHOTON_GATHER_MAX;
  uint next = (stride > 1) ? hash_uint3(__float_as_uint(sd->P.x),
                                        __float_as_uint(sd->P.y),
                                        __float_as_uint(sd->P.z) ^ __float_as_uint(sd->wi.x)) %
                                 stride :
                             0;
  const uint first = next;

  /* The radius of this gather: where the photons crowd (the core of a focused caustic, of a sun
   * through glass), narrowed to about NR_PHOTON_GATHER_K of the photons looked at, which keeps that
   * core sharp; the frame's radius elsewhere (11-15: the window test, RMSE 1.34 to 1.31; the sky
   * test's floor seen through its ball is no measure, its reference has fireflies there even at
   * 1024 samples). The photons counted are the ones the gather below would take, but for the
   * closures. */
  uint within = 0;
  {
    uint offset = 0;
    for (int b = 0; b < num_buckets; b++) {
      const uint start = cells[buckets[b]];
      const uint len = cells[buckets[b] + 1] - start;
      for (; next < offset + len; next += stride) {
        const uint i = start + (next - offset);
        const float4 p = photons[3 * i];
        const float3 d = make_float3(p.x, p.y, p.z) - sd->P;
        if (dot(d, d) <= r2 &&
            dot(make_float3(p.w, photons[3 * i + 1].w, photons[3 * i + 2].w), sd->Ng) >=
                NR_PHOTON_NORMAL_MIN_COS)
        {
          within++;
        }
      }
      offset += len;
    }
  }
  if (within == 0) {
    return zero_spectrum();
  }
  const float r_gather = (float(within) > NR_PHOTON_GATHER_K) ?
                             r * fmaxf(sqrtf(NR_PHOTON_GATHER_K / float(within)),
                                       NR_PHOTON_GATHER_MIN_RADIUS) :
                             r;
  const float r2_gather = r_gather * r_gather;

  next = first;
  uint offset = 0;
  Spectrum sum = zero_spectrum();
  for (int b = 0; b < num_buckets; b++) {
    const uint start = cells[buckets[b]];
    const uint len = cells[buckets[b] + 1] - start;
    for (; next < offset + len; next += stride) {
      const uint i = start + (next - offset);
      const float4 p = photons[3 * i];
      const float3 d = make_float3(p.x, p.y, p.z) - sd->P;
      if (dot(d, d) > r2_gather) {
        continue;
      }
      const float4 w = photons[3 * i + 1];
      const float4 q = photons[3 * i + 2];
      /* Only the photons of a surface facing the same way: not the ones on the other side of a
       * thin object, or of a nearby surface at an angle (11-15 junkshop: the photons under its
       * table cloth, thinner than the radius, lit its top; LuxCore keeps them apart likewise). */
      if (dot(make_float3(p.w, w.w, q.w), sd->Ng) < NR_PHOTON_NORMAL_MIN_COS) {
        continue;
      }
      const float3 wi = make_float3(w.x, w.y, w.z);
      const float cos_in = dot(wi, sd->N);
      if (!(fabsf(cos_in) > 1e-4f)) {
        continue;
      }
      Spectrum f = zero_spectrum();
      for (int c = 0; c < sd->num_closure; c++) {
        const ccl_private ShaderClosure *sc = &sd->closure[c];
        if (!CLOSURE_IS_BSDF(sc->type) ||
            bsdf_get_specular_roughness_squared(sc) < NR_LIGHT_TRACE_SPECULAR_ALPHA_SQUARED)
        {
          continue;
        }
        float pdf = 0.0f;
        f += bsdf_eval(kg, sd, sc, wi, &pdf) * sc->weight;
      }
      /* bsdf_eval() includes the cosine, which the arriving power already has. */
      sum += f * rgb_to_spectrum(make_float3(q.x, q.y, q.z)) / fabsf(cos_in);
    }
    offset += len;
  }
  sum *= float(stride);
  return sum * (kernel_data.integrator.nr_photon_inv_paths * kernel_data.integrator.nr_photon_strength /
                (M_PI_F * r2_gather));
}

CCL_NAMESPACE_END
