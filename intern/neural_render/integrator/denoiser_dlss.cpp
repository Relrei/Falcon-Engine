/* SPDX-FileCopyrightText: 2025 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_DLSS

#  include "integrator/denoiser_dlss.h"
#  include "integrator/pass_accessor_gpu.h"

#  include "device/cuda/device_impl.h"

#  include "scene/film.h"

#  include "util/path.h"
#  include "util/string.h"

#  include <cstdlib>
#  ifndef _WIN32
#    include <cstdio>
#    include <dirent.h>
#    include <fcntl.h>
#    include <functional>
#    include <sys/stat.h>
#    include <unistd.h>
#  endif

#  define NVSDK_NGX_HEADER_ONLY
#  include <nvsdk_ngx.h>
#  include <nvsdk_ngx_defs.h>
#  include <nvsdk_ngx_defs_dlssd.h>

CCL_NAMESPACE_BEGIN

/* Application ID for Blender from NVIDIA. */
static const int NGX_APPLICATION_ID = 100334311;

/* Falcon v5: folders holding a DLSS runtime (`libnvidia-ngx-dlssd.so.*`) shared by the Falcon
 * engines, from `FALCON_DLSS_RUNTIME_PATH` (separated like PATH). They are searched before the
 * executable directory, so a driver whose bundled runtime is too old (615 asks for 620+) still
 * finds one. Unset leaves the upstream behavior unchanged. */
#  ifndef _WIN32
/* Falcon: NGX does not load a runtime whose real path has non-ASCII characters: it skips such a
 * search folder, and it follows symbolic links to the real path first. So a runtime in e.g. a
 * Japanese folder, or Falcon unpacked into one, was reported as unsupported and DLSS could not be
 * chosen (2026-11-17, the same in the v0.6 demo). Switching the locale does not help. The runtime
 * files of such a folder are copied into an ASCII-only cache folder of this user's, which is
 * searched instead. A copy is refreshed when the size or the modification time differ. */
static bool dlss_path_is_ascii(const string &path)
{
  for (const char c : path) {
    if ((unsigned char)c >= 0x80) {
      return false;
    }
  }
  return true;
}

/* A folder only this user can write to. */
static bool dlss_private_dir(const string &dir)
{
  mkdir(dir.c_str(), 0700);
  struct stat st;
  return lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
         !(st.st_mode & (S_IWGRP | S_IWOTH));
}

static string dlss_cache_root()
{
  const char *xdg = getenv("XDG_CACHE_HOME");
  const char *home = getenv("HOME");
  string base;
  if (xdg && xdg[0] == '/' && dlss_path_is_ascii(xdg)) {
    base = xdg;
  }
  else if (home && home[0] == '/' && dlss_path_is_ascii(home)) {
    base = string(home) + "/.cache";
    mkdir(base.c_str(), 0700);
  }
  if (!base.empty() && dlss_private_dir(base + "/falcon-dlss")) {
    return base + "/falcon-dlss";
  }
  const char *tmp = getenv("TMPDIR");
  base = (tmp && tmp[0] == '/' && dlss_path_is_ascii(tmp)) ? string(tmp) : string("/tmp");
  const string dir = base + "/falcon-dlss-" + std::to_string(getuid());
  return dlss_private_dir(dir) ? dir : string();
}

static bool dlss_copy_file(const string &from, const string &to, const struct stat &from_st)
{
  const string part = to + ".part";
  const int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    return false;
  }
  const int out = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
  bool ok = out >= 0;
  char buffer[1 << 16];
  while (ok) {
    const ssize_t n = read(in, buffer, sizeof(buffer));
    if (n == 0) {
      break;
    }
    ok = n > 0 && write(out, buffer, n) == n;
  }
  close(in);
  if (out >= 0) {
    const struct timespec times[2] = {from_st.st_atim, from_st.st_mtim};
    ok = ok && futimens(out, times) == 0;
    ok = (close(out) == 0) && ok;
  }
  ok = ok && rename(part.c_str(), to.c_str()) == 0;
  if (!ok) {
    unlink(part.c_str());
  }
  return ok;
}

static string dlss_ascii_path(const string &path)
{
  if (dlss_path_is_ascii(path)) {
    return path;
  }
  const string root = dlss_cache_root();
  if (root.empty()) {
    return path;
  }
  char name[32];
  snprintf(name, sizeof(name), "%016llx", (unsigned long long)std::hash<string>()(path));
  const string dir = root + "/" + name;
  if (!dlss_private_dir(dir)) {
    return path;
  }
  DIR *const folder = opendir(path.c_str());
  if (folder == nullptr) {
    return path;
  }
  int copies = 0;
  while (const struct dirent *entry = readdir(folder)) {
    const string file = entry->d_name;
    if (!string_startswith(file, "libnvidia-ngx-")) {
      continue;
    }
    struct stat from_st, to_st;
    const string from = path + "/" + file;
    const string to = dir + "/" + file;
    if (stat(from.c_str(), &from_st) != 0 || !S_ISREG(from_st.st_mode)) {
      continue;
    }
    const bool fresh = stat(to.c_str(), &to_st) == 0 && to_st.st_size == from_st.st_size &&
                       to_st.st_mtim.tv_sec == from_st.st_mtim.tv_sec &&
                       to_st.st_mtim.tv_nsec == from_st.st_mtim.tv_nsec;
    if (fresh || dlss_copy_file(from, to, from_st)) {
      copies++;
    }
  }
  closedir(folder);
  return copies ? dir : path;
}
#  endif

static vector<wstring> dlss_search_paths()
{
  vector<wstring> paths;
  if (const char *env = getenv("FALCON_DLSS_RUNTIME_PATH")) {
#  ifdef _WIN32
    const char separator = ';';
#  else
    const char separator = ':';
#  endif
    vector<string> tokens;
    string_split(tokens, env, string(1, separator), true);
    for (const string &token : tokens) {
#  ifdef _WIN32
      paths.push_back(string_to_wstring(token));
#  else
      paths.push_back(string_to_wstring(dlss_ascii_path(token)));
#  endif
    }
  }
#  ifdef _WIN32
  paths.push_back(string_to_wstring(path_get()));
#  else
  paths.push_back(string_to_wstring(dlss_ascii_path(path_get())));
#  endif
  return paths;
}

static PassType get_actual_display_pass_type(const BufferParams &buffer_params)
{
  return buffer_params.get_actual_display_pass(PASS_COMBINED, PassMode::DENOISED)->type;
}

void DLSSDenoiser::CUDATexture::init(Device *device, int width, int height, int num_components)
{
  CUDA_ARRAY_DESCRIPTOR desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.Format = CU_AD_FORMAT_FLOAT;
  desc.NumChannels = num_components;

  cuda_device_assert(device, cuArrayCreate((CUarray *)&array, &desc));

  CUDA_TEXTURE_DESC tex_desc = {};
  tex_desc.addressMode[0] = CU_TR_ADDRESS_MODE_CLAMP;
  tex_desc.addressMode[1] = CU_TR_ADDRESS_MODE_CLAMP;
  tex_desc.addressMode[2] = CU_TR_ADDRESS_MODE_CLAMP;
  tex_desc.flags = CU_TRSF_NORMALIZED_COORDINATES;

  CUDA_RESOURCE_DESC res_desc = {};
  res_desc.resType = CU_RESOURCE_TYPE_ARRAY;
  res_desc.res.array.hArray = (CUarray)array;

  cuda_device_assert(
      device, cuTexObjectCreate((CUtexObject *)&texture_handle, &res_desc, &tex_desc, nullptr));
  cuda_device_assert(device, cuSurfObjectCreate((CUsurfObject *)&surface_handle, &res_desc));
}
void DLSSDenoiser::CUDATexture::destroy()
{
  cuSurfObjectDestroy((CUsurfObject)surface_handle);
  surface_handle = 0;
  cuTexObjectDestroy((CUtexObject)texture_handle);
  texture_handle = 0;

  cuArrayDestroy((CUarray)array);
  array = 0;
}

DLSSDenoiser::DLSSDenoiser(Device *denoiser_device, const DenoiseParams &params)
    : DenoiserGPU(denoiser_device, params)
{
  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  /* Paths to optionally search for features, in addition to the driver: the Falcon runtime
   * folders, then the executable directory. */
  const vector<wstring> search_paths = dlss_search_paths();
  /* Path to write NGX logs to. */
  const wstring app_data_path = string_to_wstring(path_cache_get());

  vector<const wchar_t *> app_paths;
  for (const wstring &path : search_paths) {
    app_paths.push_back(path.c_str());
  }

  NVSDK_NGX_FeatureCommonInfo feature_info = {};
  feature_info.PathListInfo.Path = app_paths.data();
  feature_info.PathListInfo.Length = (unsigned int)app_paths.size();
  feature_info.LoggingInfo.LoggingCallback =
      [](const char *message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature) {
        switch (loggingLevel) {
          case NVSDK_NGX_LOGGING_LEVEL_OFF:
          case NVSDK_NGX_LOGGING_LEVEL_NUM:
            assert(false);
            break;
          case NVSDK_NGX_LOGGING_LEVEL_ON:
            LOG_INFO << message;
            break;
          case NVSDK_NGX_LOGGING_LEVEL_VERBOSE:
            LOG_INFO << message;
            break;
        }
      };
  feature_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

  /* Create a fixed handle for this particular CUDA context and queue combination, since NGX uses
   * it for lookup internally. */
  ngx_device_ = new NVSDK_NGX_CUDADevice{
      cuda_device->cuContext, static_cast<CUDADeviceQueue *>(denoiser_queue_.get())->stream()};

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA_Init1(
      NGX_APPLICATION_ID, app_data_path.c_str(), ngx_device_, &feature_info);

  if (result == NVSDK_NGX_Result_FAIL_FeatureNotSupported) {
    delete ngx_device_;
    ngx_device_ = nullptr;
    set_error("Failed to load NGX driver");
  }
  else if (NVSDK_NGX_FAILED(result)) {
    set_error("Failed to initialize NGX driver");
  }
}

DLSSDenoiser::~DLSSDenoiser()
{
  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  tex_color_.destroy();
  tex_depth_.destroy();
  tex_diffuse_albedo_.destroy();
  tex_specular_albedo_.destroy();
  tex_normal_roughness_.destroy();
  tex_motion_.destroy();
  tex_specular_motion_.destroy();
  tex_output_.destroy();

  if (ngx_device_ == nullptr) {
    return;
  }

  if (handle_ != nullptr) {
    NVSDK_NGX_CUDA_ReleaseFeature(handle_);
  }

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA_Shutdown1(ngx_device_);

  if (NVSDK_NGX_FAILED(result)) {
    set_error("Failed to shutdown NGX driver");
  }

  delete ngx_device_;
}

bool DLSSDenoiser::is_device_supported(const DeviceInfo &device)
{
  if (device.type != DEVICE_CUDA && device.type != DEVICE_OPTIX) {
    return false;
  }

  /* 'NVSDK_NGX_CUDA_GetFeatureRequirements' is an expensive call, so cache the result (since
   * 'is_device_supported' is called a lot). */
  static nr::map<int, NVSDK_NGX_Feature_Support_Result> supported_cache;
  if (const auto it = supported_cache.find(device.num); it != supported_cache.end()) {
    return it->second == NVSDK_NGX_FeatureSupportResult_Supported;
  }

  /* Path to write NGX logs to. */
  const wstring app_data_path = string_to_wstring(path_cache_get());

  CUdevice cuDevice = 0;
  cuDeviceGet(&cuDevice, device.num);

  /* Query driver whether the DLSS-RR feature is supported on this particular device. */
  NVSDK_NGX_FeatureDiscoveryInfo discovery_info = {};
  discovery_info.SDKVersion = NVSDK_NGX_Version_API;
  discovery_info.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
  discovery_info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
  discovery_info.Identifier.v.ApplicationId = NGX_APPLICATION_ID;
  discovery_info.ApplicationDataPath = app_data_path.c_str();

  /* Falcon v5: without `FeatureInfo` the discovery only looks next to the executable and in the
   * driver, so a runtime in a Falcon runtime folder would be reported as unsupported. */
  const vector<wstring> search_paths = dlss_search_paths();
  vector<const wchar_t *> search_path_ptrs;
  for (const wstring &path : search_paths) {
    search_path_ptrs.push_back(path.c_str());
  }
  NVSDK_NGX_FeatureCommonInfo common_info = {};
  common_info.PathListInfo.Path = search_path_ptrs.data();
  common_info.PathListInfo.Length = (unsigned int)search_path_ptrs.size();
  discovery_info.FeatureInfo = &common_info;

  NVSDK_NGX_FeatureRequirement requirement = {NVSDK_NGX_FeatureSupportResult_Supported};

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA_GetFeatureRequirements(
      cuDevice, &discovery_info, &requirement);

  if (NVSDK_NGX_SUCCEED(result)) {
    supported_cache.emplace(device.num, requirement.FeatureSupported);
    return requirement.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
  }
  else {
    return false;
  }
}

bool DLSSDenoiser::denoise_create_if_needed(DenoiseContext &context)
{
  const bool recreate_denoiser = last_width_ != context.denoised_buffer_params.width ||
                                 last_height_ != context.denoised_buffer_params.height ||
                                 last_upscale_factor_ != context.denoise_params.upscale_factor;
  if (handle_ != nullptr && !recreate_denoiser) {
    return true;
  }

  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  if (handle_ != nullptr) {
    denoiser_queue_->synchronize();

    NVSDK_NGX_CUDA_ReleaseFeature(handle_);
    handle_ = nullptr;
  }

  tex_color_.destroy();
  tex_depth_.destroy();
  tex_diffuse_albedo_.destroy();
  tex_specular_albedo_.destroy();
  tex_normal_roughness_.destroy();
  tex_motion_.destroy();
  tex_specular_motion_.destroy();
  tex_output_.destroy();

  /* Feature creation fails below these dimensions.
   * Avoid hard error that stops rendering and only disable denoising for very small viewports. */
  if (context.denoised_buffer_params.width < 32 || context.denoised_buffer_params.height < 32) {
    last_width_ = 0;
    last_height_ = 0;
    return false;
  }

  NVSDK_NGX_Parameter *params = nullptr;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_CUDA_AllocateParameters(&params))) {
    return false;
  }

  /* Input params (with resolution divider applied). */
  params->Set(NVSDK_NGX_Parameter_Width, context.buffer_params.width);
  params->Set(NVSDK_NGX_Parameter_Height, context.buffer_params.height);
  /* Output params. */
  params->Set(NVSDK_NGX_Parameter_OutWidth, context.denoised_buffer_params.width);
  params->Set(NVSDK_NGX_Parameter_OutHeight, context.denoised_buffer_params.height);

  /* Usually the DLSS quality mode is set first and then DLSS should be queried for the optimal
   * upscale factor to go along with it. In Cycles the upscale factor is already determined before
   * the denoiser is initialized though, so we do it backwards and instead try to find a
   * reasonable quality mode to match the denoiser settings here. */
  NVSDK_NGX_PerfQuality_Value perf_quality_value = NVSDK_NGX_PerfQuality_Value_DLAA;
  if (context.denoise_params.upscale_factor > 1) {
    switch (context.denoise_params.quality) {
      default:
      case DENOISER_QUALITY_HIGH:
        perf_quality_value = NVSDK_NGX_PerfQuality_Value_MaxQuality;
        break;
      case DENOISER_QUALITY_BALANCED:
        perf_quality_value = NVSDK_NGX_PerfQuality_Value_Balanced;
        break;
      case DENOISER_QUALITY_FAST:
        perf_quality_value = NVSDK_NGX_PerfQuality_Value_MaxPerf;
        if (context.denoise_params.upscale_factor >= 3) {
          perf_quality_value = NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        }
        break;
    }
  }
  params->Set(NVSDK_NGX_Parameter_PerfQualityValue, perf_quality_value);

  params->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
  params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
              NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                  NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling);
  params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
  params->Set(NVSDK_NGX_Parameter_Use_HW_Depth, NVSDK_NGX_DLSS_Depth_Type_Linear);
  /* Normals and roughness are packed into one texture in 'denoise_filter_guiding_preprocess'. */
  params->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_DLSS_Roughness_Mode_Packed);

  /* Free internal resources immediately on release instead of doing delayed destruction. This is
   * safe to do since the denoiser queue is synchronized before release above. */
  params->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA_CreateFeature1(
      ngx_device_, NVSDK_NGX_Feature_RayReconstruction, params, &handle_);

  NVSDK_NGX_CUDA_DestroyParameters(params);

  if (NVSDK_NGX_FAILED(result)) {
    set_error("Failed to create DLSS instance");
    return false;
  }

  /* DLSS requires inputs and outputs in separate CUDA textures/surfaces, while Cycles stores them
   * in an interleaved buffer, so need to create these temporary textures and convert between the
   * two storage variants (in 'denoise_filter_color_preprocess',
   * 'denoise_filter_guiding_preprocess' and 'denoise_filter_color_postprocess'). */
  tex_color_.init(cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_depth_.init(cuda_device, context.buffer_params.width, context.buffer_params.height, 1);
  tex_diffuse_albedo_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_specular_albedo_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_normal_roughness_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 4);
  tex_motion_.init(cuda_device, context.buffer_params.width, context.buffer_params.height, 2);
  tex_specular_motion_.init(
      cuda_device, context.buffer_params.width, context.buffer_params.height, 2);

  tex_output_.init(
      cuda_device, context.denoised_buffer_params.width, context.denoised_buffer_params.height, 4);

  last_width_ = context.denoised_buffer_params.width;
  last_height_ = context.denoised_buffer_params.height;
  last_upscale_factor_ = context.denoise_params.upscale_factor;
  /* Neural Rendering: a new feature has no history to keep. */
  pending_reset_ = true;

  return !cuda_device->have_error();
}

bool DLSSDenoiser::denoise_configure_if_needed(DenoiseContext & /*context*/)
{
  return true;
}

bool DLSSDenoiser::denoise_filter_color_preprocess(const DenoiseContext &context,
                                                   const DenoisePass &pass)
{
  if (pass.type != get_actual_display_pass_type(context.buffer_params)) {
    return DenoiserGPU::denoise_filter_color_preprocess(context, pass);
  }

  /* Input params (with resolution divider applied). */
  const BufferParams &buffer_params = context.buffer_params;

  const int work_size = buffer_params.width * buffer_params.height;

  const DeviceKernelArguments args(&tex_color_.surface_handle,
                                   &context.render_buffers->buffer.device_pointer,
                                   &buffer_params.full_x,
                                   &buffer_params.full_y,
                                   &buffer_params.width,
                                   &buffer_params.height,
                                   &buffer_params.offset,
                                   &buffer_params.stride,
                                   &buffer_params.pass_stride,
                                   &pass.denoised_offset,
                                   &pass.num_components);

  return denoiser_queue_->enqueue(
      DEVICE_KERNEL_FILTER_COLOR_PREPROCESS_TO_SURFACE, work_size, args);
}
bool DLSSDenoiser::denoise_filter_color_postprocess(const DenoiseContext &context,
                                                    const DenoisePass &pass)
{
  if (pass.type != get_actual_display_pass_type(context.buffer_params)) {
    return DenoiserGPU::denoise_filter_color_postprocess(context, pass);
  }

  /* Output params. */
  const BufferParams &buffer_params = context.denoised_buffer_params;

  const int work_size = buffer_params.width * buffer_params.height;

  const DeviceKernelArguments args(&tex_output_.surface_handle,
                                   &context.render_buffers->buffer.device_pointer,
                                   &buffer_params.full_x,
                                   &buffer_params.full_y,
                                   &buffer_params.width,
                                   &buffer_params.height,
                                   &buffer_params.offset,
                                   &buffer_params.stride,
                                   &buffer_params.pass_stride,
                                   &context.num_samples,
                                   &pass.noisy_offset,
                                   &pass.denoised_offset,
                                   &context.pass_sample_count,
                                   &pass.num_components,
                                   &params_.upscale_factor);

  return denoiser_queue_->enqueue(
      DEVICE_KERNEL_FILTER_COLOR_POSTPROCESS_FROM_SURFACE, work_size, args);
}

bool DLSSDenoiser::denoise_filter_guiding_preprocess(DenoiseContext &context)
{
  const BufferParams &buffer_params = context.buffer_params;

  const int work_size = buffer_params.width * buffer_params.height;

  const int pass_depth = context.buffer_params.get_pass_offset(PASS_DENOISING_DEPTH);
  const int pass_specular_albedo = context.buffer_params.get_pass_offset(
      PASS_DENOISING_SPECULAR_ALBEDO);
  const int pass_roughness = context.buffer_params.get_pass_offset(PASS_DENOISING_ROUGHNESS);
  const int pass_backward_motion = context.buffer_params.get_pass_offset(
      PASS_DENOISING_BACKWARD_MOTION);
  const int pass_specular_motion = context.buffer_params.get_pass_offset(
      PASS_DENOISING_SPECULAR_MOTION);
  const int pass_glass_weight = context.buffer_params.get_pass_offset(
      PASS_DENOISING_GLASS_WEIGHT);

  /* Neural Rendering: how the guides are prepared (see DLSSGuideFlag). */
  const int guide_flags = nr_dlss_guide_flags();

  const DeviceKernelArguments args(&tex_depth_.surface_handle,
                                   &tex_diffuse_albedo_.surface_handle,
                                   &tex_specular_albedo_.surface_handle,
                                   &tex_normal_roughness_.surface_handle,
                                   &tex_motion_.surface_handle,
                                   &tex_specular_motion_.surface_handle,
                                   &context.render_buffers->buffer.device_pointer,
                                   &buffer_params.offset,
                                   &buffer_params.stride,
                                   &buffer_params.pass_stride,
                                   &context.pass_sample_count,
                                   &pass_depth,
                                   &context.pass_denoising_albedo,
                                   &pass_specular_albedo,
                                   &context.pass_denoising_normal,
                                   &pass_roughness,
                                   &pass_backward_motion,
                                   &pass_specular_motion,
                                   &pass_glass_weight,
                                   &buffer_params.full_x,
                                   &buffer_params.full_y,
                                   &buffer_params.width,
                                   &buffer_params.height,
                                   &context.num_samples,
                                   &guide_flags);

  return denoiser_queue_->enqueue(
      DEVICE_KERNEL_FILTER_GUIDING_PREPROCESS_TO_SURFACE, work_size, args);
}

bool DLSSDenoiser::denoise_run(const DenoiseContext &context, const DenoisePass &pass)
{
  if (pass.type != get_actual_display_pass_type(context.buffer_params)) {
    return true;
  }

  NVSDK_NGX_Parameter *params = nullptr;
  if (NVSDK_NGX_FAILED(NVSDK_NGX_CUDA_AllocateParameters(&params))) {
    return false;
  }

  CUDADevice *const cuda_device = static_cast<CUDADevice *>(denoiser_device_);
  const CUDAContextScope scope(cuda_device);

  /* Neural Rendering: Cycles always passes 0, which is right for the viewport (it redraws the
   * same view) but hands a new shot the previous shot's history in a final animation render.
   * NR_DLSS_RESET=0 goes back to always 0 for comparison. */
  static const bool use_reset = std::getenv("NR_DLSS_RESET") ?
                                    std::atoi(std::getenv("NR_DLSS_RESET")) != 0 :
                                    true;
  const bool is_reset = use_reset && pending_reset_;
  pending_reset_ = false;
  params->Set(NVSDK_NGX_Parameter_Reset, is_reset ? 1 : 0);
  LOG_DEBUG << "Neural Rendering: DLSS evaluate, reset " << is_reset;

  /* Neural Rendering (from v4; the history was not being accumulated): DLSS counts the pixel centre half
   * a pixel further than Cycles' integer raster coordinate, so the jitter carries that half pixel.
   * Without it the reconstruction lands 0.5 px off, the history never lines up with the new
   * samples and the error does not fall over the frames (v4's 5.2 measurement: classroom 32 spp
   * 0.0399 -> 0.0197; the 5.3 port and so v5 had lost it). NR_DLSS_JITTER_HALF=0 leaves it out,
   * for comparison. */
  static const float jitter_half = (std::getenv("NR_DLSS_JITTER_HALF") &&
                                    std::atoi(std::getenv("NR_DLSS_JITTER_HALF")) == 0) ?
                                       0.0f :
                                       0.5f;
  /* FLT_MAX: no jitter, the samples are spread over the pixel by the pixel filter (Pixel Filter
   * setting), which is centred: DLSS is told zero, without the half pixel (with it the image came
   * out half a pixel off and drifted darker over the frames, 11-14). */
  const bool has_jitter = context.pixel_jitter.x != FLT_MAX;
  params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,
              has_jitter ? context.pixel_jitter.x + jitter_half : 0.0f);
  params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,
              has_jitter ? context.pixel_jitter.y + jitter_half : 0.0f);

  params->Set(NVSDK_NGX_Parameter_Color, &tex_color_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_Depth, &tex_depth_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_DiffuseAlbedo, &tex_diffuse_albedo_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_SpecularAlbedo, &tex_specular_albedo_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_GBuffer_Normals, &tex_normal_roughness_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, &tex_normal_roughness_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_MotionVectors, &tex_motion_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, &tex_specular_motion_.texture_handle);
  params->Set(NVSDK_NGX_Parameter_Output, &tex_output_.surface_handle);

  params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
              context.buffer_params.width);
  params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
              context.buffer_params.height);

  params->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, 1);

  const NVSDK_NGX_Result result = NVSDK_NGX_CUDA_EvaluateFeature(handle_, params, nullptr);

  NVSDK_NGX_CUDA_DestroyParameters(params);

  return NVSDK_NGX_SUCCEED(result);
}

CCL_NAMESPACE_END

#endif
