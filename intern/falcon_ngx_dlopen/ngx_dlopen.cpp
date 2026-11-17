/* SPDX-FileCopyrightText: 2026 Relrei
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Falcon: the NGX entry points the DLSS denoiser calls, resolved at run time from the NGX core
 * that ships with the NVIDIA driver (`libnvidia-ngx.so.1` / `_nvngx.dll`), instead of linking
 * the SDK's static stub library (`libnvsdk_ngx.a`, NVIDIA object code).
 *
 * Falcon v5: always used in place of the stub, by both Cycles (`cycles_integrator`) and Neural
 * Rendering (`nr_integrator`), so the two DLSS denoisers share one table. Ported from v4
 * (`intern/cycles/integrator/denoiser_dlss_ngx_dlopen.cpp`, FALCON_DLSS_DLOPEN). The SDK headers
 * are used for the types and declarations only. Same approach as the demo (5.2.2) `NGXDriver`. When the driver has no NGX core, every call fails with
 * `NVSDK_NGX_Result_FAIL_FeatureNotSupported` and the denoiser reports itself as unavailable. */

#ifdef WITH_DLSS

#  include <nvsdk_ngx.h>

#  ifdef _WIN32
#    include "util/windows.h"
#  else
#    include <dlfcn.h>
#  endif

#  include <mutex>

namespace {

/* The driver's CUDA init takes the feature info (the paths where the DLSS runtime is looked up)
 * in the slot the header declares as `const NVSDK_NGX_Parameter *`. */
using tInit_Ext1 = NVSDK_NGX_Result (*)(unsigned long long,
                                        const wchar_t *,
                                        NVSDK_NGX_CUDADevice *,
                                        NVSDK_NGX_Version,
                                        const NVSDK_NGX_FeatureCommonInfo *);
/* The driver's shutdown also reports a count through a reference the header does not declare
 * (same as the demo's `NGXDriver`); calling it with one argument crashes inside the driver. */
using tShutdown1 = NVSDK_NGX_Result (*)(NVSDK_NGX_CUDADevice *, unsigned int &);
using tGetFeatureRequirements = decltype(&NVSDK_NGX_CUDA_GetFeatureRequirements);
using tCreateFeature1 = decltype(&NVSDK_NGX_CUDA_CreateFeature1);
using tEvaluateFeature = decltype(&NVSDK_NGX_CUDA_EvaluateFeature);
using tReleaseFeature = decltype(&NVSDK_NGX_CUDA_ReleaseFeature);
using tAllocateParameters = decltype(&NVSDK_NGX_CUDA_AllocateParameters);
using tDestroyParameters = decltype(&NVSDK_NGX_CUDA_DestroyParameters);

struct NGXCore {
  tInit_Ext1 Init_Ext1 = nullptr;
  tShutdown1 Shutdown1 = nullptr;
  tGetFeatureRequirements GetFeatureRequirements = nullptr;
  tCreateFeature1 CreateFeature1 = nullptr;
  tEvaluateFeature EvaluateFeature = nullptr;
  tReleaseFeature ReleaseFeature = nullptr;
  tAllocateParameters AllocateParameters = nullptr;
  tDestroyParameters DestroyParameters = nullptr;

  bool complete() const
  {
    return Init_Ext1 && Shutdown1 && CreateFeature1 && EvaluateFeature && ReleaseFeature &&
           AllocateParameters && DestroyParameters;
  }
};

#  ifdef _WIN32
void *open_core()
{
  WCHAR path[MAX_PATH] = L"";
  HKEY key = nullptr;
  LSTATUS result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                 L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore",
                                 0,
                                 KEY_READ,
                                 &key);
  if (result != ERROR_SUCCESS) {
    result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", 0, KEY_READ, &key);
  }
  if (result != ERROR_SUCCESS) {
    return nullptr;
  }
  DWORD size = ARRAYSIZE(path);
  result = RegQueryValueExW(key, L"NGXPath", 0, nullptr, reinterpret_cast<LPBYTE>(path), &size);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) {
    return nullptr;
  }
  wcscat_s(path, L"\\_nvngx.dll");
  return LoadLibraryW(path);
}
template<typename T> T find(void *lib, const char *name)
{
  return reinterpret_cast<T>(GetProcAddress(static_cast<HMODULE>(lib), name));
}
#  else
void *open_core()
{
  return dlopen("libnvidia-ngx.so.1", RTLD_NOW);
}
template<typename T> T find(void *lib, const char *name)
{
  return reinterpret_cast<T>(dlsym(lib, name));
}
#  endif

const NGXCore *core()
{
  static NGXCore table;
  static std::once_flag once;
  std::call_once(once, [] {
    void *const lib = open_core();
    if (lib == nullptr) {
      return;
    }
    NGXCore t;
    t.Init_Ext1 = find<tInit_Ext1>(lib, "NVSDK_NGX_CUDA_Init_Ext1");
    t.Shutdown1 = find<tShutdown1>(lib, "NVSDK_NGX_CUDA_Shutdown1");
    t.GetFeatureRequirements = find<tGetFeatureRequirements>(
        lib, "NVSDK_NGX_CUDA_GetFeatureRequirements");
    t.CreateFeature1 = find<tCreateFeature1>(lib, "NVSDK_NGX_CUDA_CreateFeature1");
    t.EvaluateFeature = find<tEvaluateFeature>(lib, "NVSDK_NGX_CUDA_EvaluateFeature");
    t.ReleaseFeature = find<tReleaseFeature>(lib, "NVSDK_NGX_CUDA_ReleaseFeature");
    t.AllocateParameters = find<tAllocateParameters>(lib, "NVSDK_NGX_CUDA_AllocateParameters");
    t.DestroyParameters = find<tDestroyParameters>(lib, "NVSDK_NGX_CUDA_DestroyParameters");
    /* Stays loaded for the life of the process, like the stub library would keep it. */
    if (t.complete()) {
      table = t;
    }
  });
  return table.complete() ? &table : nullptr;
}

constexpr NVSDK_NGX_Result kUnavailable = NVSDK_NGX_Result_FAIL_FeatureNotSupported;

}  // namespace

extern "C" {

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_CUDA_Init1(unsigned long long InApplicationId,
                                                 const wchar_t *InApplicationDataPath,
                                                 NVSDK_NGX_CUDADevice *InDevice,
                                                 const NVSDK_NGX_FeatureCommonInfo *InFeatureInfo,
                                                 NVSDK_NGX_Version InSDKVersion)
{
  const NGXCore *c = core();
  return c ? c->Init_Ext1(
                 InApplicationId, InApplicationDataPath, InDevice, InSDKVersion, InFeatureInfo) :
             kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_CUDA_Shutdown1(NVSDK_NGX_CUDADevice *InDevice)
{
  const NGXCore *c = core();
  unsigned int count = 0;
  return c ? c->Shutdown1(InDevice, count) : kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_CUDA_GetFeatureRequirements(int CudaDevice,
                                      const NVSDK_NGX_FeatureDiscoveryInfo *FeatureDiscoveryInfo,
                                      NVSDK_NGX_FeatureRequirement *OutSupported)
{
  const NGXCore *c = core();
  return (c && c->GetFeatureRequirements) ?
             c->GetFeatureRequirements(CudaDevice, FeatureDiscoveryInfo, OutSupported) :
             kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_CUDA_CreateFeature1(NVSDK_NGX_CUDADevice *InDevice,
                                                         NVSDK_NGX_Feature InFeatureID,
                                                         const NVSDK_NGX_Parameter *InParameters,
                                                         NVSDK_NGX_Handle **OutHandle)
{
  const NGXCore *c = core();
  return c ? c->CreateFeature1(InDevice, InFeatureID, InParameters, OutHandle) : kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_CUDA_EvaluateFeature(const NVSDK_NGX_Handle *InFeatureHandle,
                                                          const NVSDK_NGX_Parameter *InParameters,
                                                          PFN_NVSDK_NGX_ProgressCallback InCallback)
{
  const NGXCore *c = core();
  return c ? c->EvaluateFeature(InFeatureHandle, InParameters, InCallback) : kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_CUDA_ReleaseFeature(NVSDK_NGX_Handle *InHandle)
{
  const NGXCore *c = core();
  return c ? c->ReleaseFeature(InHandle) : kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_CUDA_AllocateParameters(NVSDK_NGX_Parameter **OutParameters)
{
  const NGXCore *c = core();
  return c ? c->AllocateParameters(OutParameters) : kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_CUDA_DestroyParameters(NVSDK_NGX_Parameter *InParameters)
{
  const NGXCore *c = core();
  return c ? c->DestroyParameters(InParameters) : kUnavailable;
}

}  // extern "C"

#endif /* WITH_DLSS */
