/* SPDX-FileCopyrightText: 2021-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Falcon Cycles-expansion: the shader ray-tracing kernel with the bidirectional techniques, loaded
 * instead of kernel_shader_raytrace.cu when the scene uses them (KERNEL_FEATURE_NR_BIDIR). */

#include "kernel/device/optix/compat.h"
#include "kernel/device/optix/globals.h"

#include "kernel/device/gpu/image.h"

#include "kernel/integrator/shade_surface.h"

extern "C" __global__ void __raygen__kernel_optix_integrator_shade_surface_raytrace()
{
  const int global_index = optixGetLaunchIndex().x;
  const int path_index = (kernel_params.path_index_array) ?
                             kernel_params.path_index_array[global_index] :
                             global_index;
  integrator_shade_surface_raytrace(nullptr, path_index, kernel_params.render_buffer);
}
