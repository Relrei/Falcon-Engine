/* SPDX-FileCopyrightText: 2026 Falcon Engine
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Falcon Cycles-expansion: bidirectional light tracing, the paths that start at the lights.
 *
 * The scheme follows LuxCore's hybrid back/forward path tracing (LuxCoreRender, Apache License
 * 2.0, src/slg/engines/pathtracer.cpp RenderLightSample() and ConnectToEye(),
 * src/slg/cameras/perspective.cpp GetPDF()): light paths carry the caustics L S+ D E, the
 * camera paths everything else. The code is written for the Cycles kernel, not copied.
 *
 * One light path is started per pixel and sample of the work tiles, so N light paths are
 * width * height * samples, and a light path vertex x seen in pixel j adds
 *
 *   throughput * f(x, to camera) * |cos| / (cos^3(theta_camera) * pixel_area * distance^2)
 *
 * to the pixel: averaging by the sample count as Cycles does for every pixel gives the light
 * tracing estimate (LuxCore's fluxToRadianceFactor with a per-screen normalisation of
 * pixel count / light paths).
 *
 * The light flux follows the Cycles light conventions (scene/light.cpp, kernel/light/): for point,
 * spot and area lights the radiance is strength * eval_fac, so a Lambertian emitter of area A sends
 * strength * eval_fac * pi * A; for a sun the irradiance is strength * eval_fac times the sun
 * disk's solid angle.
 * The world sends parallel rays from a disk covering the scene, in the directions its light
 * samples, with its radiance from the background shader, for its bright directions only (11-15,
 * NR_WORLD_LIGHT_PATH_MIN_FACTOR).
 * Emissive meshes send light paths from the triangles their light sampling uses (emission sampling
 * on), with their emission shader evaluated at the start point (11-15).
 * Not covered (the camera paths keep them): light node trees (only the light's colour and strength
 * are used), area light spread and spot light radius. */

#pragma once

#include "kernel/integrator/path_state.h"
#include "kernel/integrator/state_util.h"

#include "kernel/geom/object.h"
#include "kernel/light/common.h"
#include "kernel/light/spot.h"

#include "kernel/sample/mapping.h"
#include "kernel/sample/pattern.h"

CCL_NAMESPACE_BEGIN

/* A bounce counts as nearly specular when its closure has alpha_x * alpha_y below this (alpha is the
 * squared roughness, so a roughness below about 0.22). LuxCore uses a glossiness threshold of
 * 0.05. */
#define NR_LIGHT_TRACE_SPECULAR_ALPHA_SQUARED 0.0025f

ccl_device_inline bool light_trace_active(KernelGlobals kg)
{
  return (kernel_data.integrator.bidirectional_strategies & BIDIR_STRATEGY_LIGHT_TRACING) != 0;
}

/* The caustic chains are tracked, and light paths can be started, when light tracing or photon
 * mapping is on (the photon pass sends light paths too). */
ccl_device_inline bool caustic_chains_tracked(KernelGlobals kg)
{
  return (kernel_data.integrator.bidirectional_strategies &
          (BIDIR_STRATEGY_LIGHT_TRACING | BIDIR_STRATEGY_PHOTON_MAPPING)) != 0;
}

/* The world sends light paths when it is a light (importance sampling on, so it has its light in
 * the list), and not with OSL shading: the light path start evaluates the background shader with
 * SVM (kernel/integrator/light_path_start.h). */
ccl_device_inline bool light_trace_world_emitted(KernelGlobals kg)
{
  return kernel_data.background.use_mis && kernel_data.background.map_res_x > 0 &&
         kernel_data.background.map_res_y > 0 &&
         !(kernel_data.kernel_features & KERNEL_FEATURE_OSL_SHADING);
}

/* The world's light paths carry only its directions brighter than this many times its average
 * radiance (a sun, the bright part of an HDRI), the camera paths keep the rest: the caustics of a
 * wide, even sky are found well by the camera paths, and the light paths gave them more noise
 * (11-15: a sky without its sun around a glass ball, RMSE 0.048 without, 0.062 with them). The
 * split is by direction, so each direction is counted once. */
#define NR_WORLD_LIGHT_PATH_MIN_FACTOR 10.0f

ccl_device_inline bool light_trace_world_radiance_emitted(KernelGlobals kg, const Spectrum L)
{
  /* The importance map's total (scene/light.cpp device_update_background()): the average
   * radiance over the sphere, averaged over the channels as here, is total * pi / 2. */
  const float average_radiance = kernel_data_fetch(light_background_marginal_cdf,
                                                   kernel_data.background.map_res_y)
                                     .x *
                                 M_PI_2_F;
  return average(fabs(L)) > NR_WORLD_LIGHT_PATH_MIN_FACTOR * average_radiance;
}

/* Emissive meshes send light paths when their triangles are listed (scene/light.cpp
 * device_update_nr_mesh_lights()), and not with OSL shading: the light path start evaluates their
 * emission with SVM. */
ccl_device_inline bool light_trace_mesh_emitted(KernelGlobals kg)
{
  return kernel_data.integrator.nr_mesh_light_count > 0 &&
         !(kernel_data.kernel_features & KERNEL_FEATURE_OSL_SHADING);
}

/* The lights the light tracer emits from. The camera paths only leave out the caustics of
 * these. */
ccl_device_inline bool light_trace_light_supported(KernelGlobals kg, const int type)
{
  return type == LIGHT_POINT || type == LIGHT_SPOT || type == LIGHT_AREA || type == LIGHT_SUN ||
         (type == LIGHT_BACKGROUND && light_trace_world_emitted(kg)) ||
         (type == LIGHT_TRIANGLE && light_trace_mesh_emitted(kg));
}

/* A camera path that reached a light the light tracer emits from through E D S+: the light paths
 * carry that path (L S+ D E), so the camera path leaves it out. */
ccl_device_inline bool photon_mapping_active(KernelGlobals kg)
{
  return (kernel_data.integrator.bidirectional_strategies & BIDIR_STRATEGY_PHOTON_MAPPING) != 0 &&
         kernel_data.integrator.nr_photon_count > 0;
}

/* Which technique takes the caustics at the first not nearly specular vertex of a camera path:
 * light tracing on the first hit, photon mapping after a specular prefix, or on the first hit too
 * when light tracing is off. */
ccl_device_inline bool caustics_taken_at_first_hit(KernelGlobals kg)
{
  return light_trace_active(kg) || photon_mapping_active(kg);
}

ccl_device_inline bool caustics_taken(KernelGlobals kg, const uint nr_chain)
{
  if (nr_chain & NR_CHAIN_VOLUME) {
    return light_trace_active(kg);
  }
  return (nr_chain & NR_CHAIN_PREFIX_SPECULAR) ? photon_mapping_active(kg) :
                                                 caustics_taken_at_first_hit(kg);
}

/* Light tracing: the importance of the camera for a light path vertex at P, 0 when the vertex is
 * not seen (behind the camera, outside the frame, not a pinhole perspective camera). Gives the
 * pixel it lands in and the direction and distance to the camera. Importance of the pixel:
 * 1 / (cos^3 * pixel area on the image plane at distance 1), times the 1 / distance^2 of the
 * connection (LuxCore PerspectiveCamera::GetPDF()), divided by the pixel count: the light paths of
 * one sample spread over the whole frame (one per pixel), while the film divides every pixel by its
 * own sample count (LuxCore's pixel count / light paths). Used by the surface vertices
 * (kernel/integrator/shade_surface.h) and the volume ones (kernel/integrator/shade_volume.h). */
ccl_device_inline float light_trace_camera_importance(KernelGlobals kg,
                                                      const float3 P,
                                                      ccl_private uint *r_pixel,
                                                      ccl_private float3 *r_to_camera,
                                                      ccl_private float *r_distance)
{
  if (kernel_data.cam.type != CAMERA_PERSPECTIVE || kernel_data.cam.aperturesize > 0.0f) {
    return 0.0f;
  }
  const Transform cameratoworld = kernel_data.cam.cameratoworld;
  const Transform worldtocamera = kernel_data.cam.worldtocamera;
  const ProjectionTransform worldtoraster = kernel_data.cam.worldtoraster;
  const ProjectionTransform rastertocamera = kernel_data.cam.rastertocamera;

  /* In front of the camera and inside the frame. */
  const float3 P_camera = transform_point(&worldtocamera, P);
  if (P_camera.z <= 0.0f) {
    return 0.0f;
  }
  const float3 raster = transform_perspective(&worldtoraster, P);
  const int width = int(kernel_data.cam.width);
  const int height = int(kernel_data.cam.height);
  float2 raster_pixel = make_float2(raster.x, raster.y);
  if (kernel_data.integrator.pixel_jitter.x != FLT_MAX) {
    /* With DLSS the camera samples pixel x at the one point x - jitter (camera_sample(), jitter in
     * [-0.5, 0.5)), not inside [x, x + 1): the vertex goes to the pixel whose sample point is the
     * nearest, so the light paths follow the jitter and line up with the camera paths. Before
     * (11-16) they stayed half a pixel off and did not move with the jitter. */
    raster_pixel += kernel_data.integrator.pixel_jitter + make_float2(0.5f, 0.5f);
  }
  const int px = int(floorf(raster_pixel.x));
  const int py = int(floorf(raster_pixel.y));
  if (px < 0 || py < 0 || px >= width || py >= height) {
    return 0.0f;
  }

  const float3 camera_P = make_float3(cameratoworld.x.w, cameratoworld.y.w, cameratoworld.z.w);
  float distance;
  const float3 to_camera = normalize_len(camera_P - P, &distance);
  if (!(distance > 0.0f)) {
    return 0.0f;
  }

  const float cos_camera = P_camera.z / len(P_camera);
  const float3 p00 = transform_perspective(&rastertocamera,
                                           make_float3(float(px), float(py), 0.0f));
  const float3 p10 = transform_perspective(&rastertocamera,
                                           make_float3(float(px + 1), float(py), 0.0f));
  const float3 p01 = transform_perspective(&rastertocamera,
                                           make_float3(float(px), float(py + 1), 0.0f));
  const float3 a00 = p00 / p00.z;
  const float pixel_area = len(cross(p10 / p10.z - a00, p01 / p01.z - a00));
  if (!(pixel_area > 0.0f) || !(cos_camera > 0.0f)) {
    return 0.0f;
  }
  *r_pixel = uint(px + py * width);
  *r_to_camera = to_camera;
  *r_distance = distance;
  return 1.0f / (cos_camera * cos_camera * cos_camera * pixel_area * sqr(distance) *
                 float(width) * float(height));
}

/* Light tracing guide: credit the cell the light path started in with the energy that reached a
 * vertex connected to the camera (kernel/integrator/init_from_light.h). */
ccl_device_inline void light_trace_guide_credit(KernelGlobals kg,
                                                ConstIntegratorState state,
                                                const Spectrum f)
{
  const uint guide_cell = INTEGRATOR_STATE(state, path, nr_light_guide_cell);
  if (guide_cell != NR_LIGHT_GUIDE_NONE && kernel_data_array(nr_light_guide_hist) != nullptr) {
    const float energy = average(INTEGRATOR_STATE(state, path, throughput) * f);
    if (energy > 0.0f && isfinite_safe(energy)) {
      atomic_add_and_fetch_float(&kernel_data_array(nr_light_guide_hist)[guide_cell], energy);
    }
  }
}

/* A camera path that reached a light the light paths emit from through its caustic chain (first
 * not nearly specular vertex, then S+): light tracing or photon mapping carries that path, so the
 * camera path leaves it out. */
ccl_device_inline bool light_trace_camera_path_skips_light(KernelGlobals kg,
                                                          const uint32_t path_flag,
                                                          const uint nr_chain,
                                                          const int light_type)
{
  return !(path_flag & PATH_RAY_NR_LIGHT_PATH) && (path_flag & PATH_RAY_NR_CAUSTIC_CHAIN) &&
         (path_flag & PATH_RAY_NR_CAUSTIC_SPECULAR) &&
         light_trace_light_supported(kg, light_type) && caustics_taken(kg, nr_chain);
}

CCL_NAMESPACE_END
