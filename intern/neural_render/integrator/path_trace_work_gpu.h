/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/state.h"

#include "device/graphics_interop.h"
#include "device/memory.h"
#include "device/queue.h"

#include "integrator/path_trace_work.h"
#include "integrator/work_tile_scheduler.h"

#include "util/vector.h"

CCL_NAMESPACE_BEGIN

struct KernelWorkTile;

/* Implementation of PathTraceWork which schedules work to the device in tiles which are sized
 * to match device queue's number of path states.
 * This implementation suits best devices which have a lot of integrator states, such as GPU. */
class PathTraceWorkGPU : public PathTraceWork {
 public:
  PathTraceWorkGPU(Device *device,
                   Film *film,
                   DeviceScene *device_scene,
                   const bool *cancel_requested_flag);

  void alloc_work_memory() override;
  void init_execution() override;

  void render_samples(RenderStatistics &statistics,
                      const int start_sample,
                      const int samples_num,
                      const int sample_offset) override;

  bool render_photons(const int sample_offset, const int progressive_pass = 0) override;
  void update_photon_auto(const int start_sample, const int num_samples) override;

  bool render_light_paths(RenderStatistics &statistics,
                          const int start_sample,
                          const int samples_num,
                          const int sample_offset) override;

  void copy_to_display(PathTraceDisplay *display,
                       PassMode pass_mode,
                       const int num_samples) override;
  void destroy_gpu_resources(PathTraceDisplay *display) override;

  bool copy_render_buffers_from_device() override;
  bool copy_render_buffers_to_device() override;
  bool zero_render_buffers() override;

  int adaptive_sampling_converge_filter_count_active(const float threshold, bool reset) override;
  void cryptomatte_postproces() override;
  void denoise_volume_guiding_buffers() override;

 protected:
  void alloc_integrator_soa();
  void alloc_integrator_queue();
  void alloc_integrator_sorting();
  void alloc_integrator_path_split();

  /* Returns DEVICE_KERNEL_NUM if there are no scheduled kernels. */
  DeviceKernel get_most_queued_kernel() const;

  void enqueue_reset();

  bool enqueue_work_tiles(bool &finished);
  void enqueue_work_tiles(DeviceKernel kernel,
                          const KernelWorkTile work_tiles[],
                          const int num_work_tiles,
                          const int num_active_paths,
                          const int num_predicted_splits);

  bool enqueue_path_iteration();
  void enqueue_path_iteration(DeviceKernel kernel, const int num_paths_limit = INT_MAX);

  bool update_queue_counter_and_cache();

  void compute_queued_paths(DeviceKernel kernel, DeviceKernel queued_kernel);
  void compute_sorted_queued_paths(DeviceKernel queued_kernel, const int num_paths_limit);

  void compact_main_paths(const int num_active_paths);
  void compact_shadow_paths(const bool force = false);
  void compact_paths(const int num_active_paths,
                     const int max_active_path_index,
                     DeviceKernel terminated_paths_kernel,
                     DeviceKernel compact_paths_kernel,
                     DeviceKernel compact_kernel);

  int num_active_main_paths_paths();

  /* Check whether graphics interop can be used for the PathTraceDisplay update. */
  bool should_use_graphics_interop(PathTraceDisplay *display);

  /* Naive implementation of the `copy_to_display()` which performs film conversion on the
   * device, then copies pixels to the host and pushes them to the `display`. */
  void copy_to_display_naive(PathTraceDisplay *display, PassMode pass_mode, const int num_samples);

  /* Implementation of `copy_to_display()` which uses driver's OpenGL/GPU interoperability
   * functionality, avoiding copy of pixels to the host. */
  bool copy_to_display_interop(PathTraceDisplay *display,
                               PassMode pass_mode,
                               const int num_samples);

  /* Synchronously run film conversion kernel and store display result in the given destination. */
  void get_render_tile_film_pixels(const PassAccessor::Destination &destination,
                                   PassMode pass_mode,
                                   int num_samples);

  int adaptive_sampling_convergence_check_count_active(const float threshold, bool reset);
  void enqueue_adaptive_sampling_filter_x();
  void enqueue_adaptive_sampling_filter_y();

  bool has_shadow_catcher() const;

  /* Kernel that starts the paths of the work tiles instead of the camera (or bake) one: the light
   * paths of render_light_paths(). DEVICE_KERNEL_NUM = none. */
  DeviceKernel init_kernel_override_ = DEVICE_KERNEL_NUM;

  /* Light tracing guide (kernel/data_arrays.h nr_light_guide_*): the energy of the caustic
   * connections per light and cell since the first sample of the frame, read back after each pass of
   * light paths and turned into the tables of the next pass. */
  void update_light_guide(const int start_sample);
  vector<double> light_guide_energy_;
  /* Automatic bidirectional (NR_BIDIR_AUTO_FLAG): light tracing was stopped for the rest of this
   * frame because its first sample added too little (the kernel data has it off until the next
   * frame starts, render_samples()). */
  bool light_trace_auto_stopped_ = false;
  /* The same for photon mapping (update_photon_auto(); back on in the next frame's photon pass). */
  bool photon_auto_stopped_ = false;
  /* The energy of the whole image in the render buffer (the combined pass). */
  double combined_energy();

  /* Count how many currently scheduled paths can still split. */
  int shadow_catcher_count_possible_splits();

  /* Kernel properties. */
  bool kernel_uses_sorting(DeviceKernel kernel);
  bool kernel_creates_shadow_paths(DeviceKernel kernel);
  bool kernel_creates_ao_paths(DeviceKernel kernel);
  bool kernel_is_shadow_path(DeviceKernel kernel);
  int kernel_max_active_main_path_index(DeviceKernel kernel);

  /* Integrator queue. */
  unique_ptr<DeviceQueue> queue_;

  /* Scheduler which gives work to path tracing threads. */
  WorkTileScheduler work_tile_scheduler_;

  /* Integrate state for paths. */
  IntegratorStateGPU integrator_state_gpu_;
  /* SoA arrays for integrator state. */
  vector<unique_ptr<device_memory>> integrator_state_soa_;
  uint64_t integrator_state_soa_kernel_features_;
  int integrator_state_soa_volume_stack_size_ = 0;
  /* Keep track of number of queued kernels. */
  device_vector<IntegratorQueueCounter> integrator_queue_counter_;
  /* Shader sorting. */
  device_vector<int> integrator_shader_sort_counter_;
  device_vector<int> integrator_shader_raytrace_sort_counter_;
  /* Falcon Cycles-expansion: of DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_BIDIR. */
  device_vector<int> integrator_shader_bidir_sort_counter_;
  device_vector<int> integrator_shader_sort_prefix_sum_;
  device_vector<int> integrator_shader_sort_partition_key_offsets_;
  /* Path split. */
  device_vector<int> integrator_next_main_path_index_;
  device_vector<int> integrator_next_shadow_path_index_;

  /* Temporary buffer to get an array of queued path for a particular kernel. */
  device_vector<int> queued_paths_;
  device_vector<int> num_queued_paths_;

  /* Temporary buffer for passing work tiles to kernel. */
  device_vector<KernelWorkTile> work_tiles_;

  /* Temporary buffer used by the copy_to_display() whenever graphics interoperability is not
   * available. Is allocated on-demand. */
  device_vector<half4> display_rgba_half_;

  unique_ptr<DeviceGraphicsInterop> device_graphics_interop_;

  /* Cached result of device->should_use_graphics_interop(). */
  bool interop_use_checked_ = false;
  bool interop_use_ = false;

  /* Number of partitions to sort state indices into prior to material sort. */
  int num_sort_partitions_;

  /* Maximum number of concurrent integrator states. */
  int max_num_paths_;

  /* Minimum number of paths which keeps the device bust. If the actual number of paths falls below
   * this value more work will be scheduled. */
  int min_num_active_main_paths_;

  /* Maximum path index, effective number of paths used may be smaller than
   * the size of the integrator_state_ buffer so can avoid iterating over the
   * full buffer. */
  int max_active_main_path_index_;
};

CCL_NAMESPACE_END
