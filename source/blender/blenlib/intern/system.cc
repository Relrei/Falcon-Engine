/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CLG_log.h"

#include "BLI_math_base.h"
#include "BLI_mutex.hh"
#include "BLI_string.h"
#include "BLI_system.h"
#include "BLI_utildefines.h"

/* for backtrace and gethostname/GetComputerName */
#if defined(WIN32)
#  include <intrin.h>

#  include "BLI_winstuff.h"
#else
#  if defined(HAVE_EXECINFO_H)
#    include <execinfo.h>
#  endif
#  include <sys/resource.h>
#  include <unistd.h>
#endif

namespace blender {

static CLG_LogRef LOG = {"system"};

int BLI_cpu_support_sse2()
{
#if defined(__x86_64__) || defined(_M_X64)
  /* x86_64 always has SSE2 instructions */
  return 1;
#elif defined(__GNUC__) && defined(i386)
  /* for GCC x86 we check cpuid */
  uint d;
  __asm__(
      "pushl %%ebx\n\t"
      "cpuid\n\t"
      "popl %%ebx\n\t"
      : "=d"(d)
      : "a"(1));
  return (d & 0x04000000) != 0;
#elif (defined(_MSC_VER) && defined(_M_IX86))
  /* also check cpuid for MSVC x86 */
  uint d;
  __asm {
    xor     eax, eax
    inc eax
    push ebx
    cpuid
    pop ebx
    mov d, edx
  }
  return (d & 0x04000000) != 0;
#else
  return 0;
#endif
}

/* Windows stack-walk lives in system_win32.cc */
#if !defined(_MSC_VER)
void BLI_system_backtrace_with_os_info(FILE *fp, const void * /*os_info*/)
{
  /* ----------------------- */
  /* If system as execinfo.h */
#  if defined(HAVE_EXECINFO_H)

#    define SIZE 100
  void *buffer[SIZE];
  int nptrs;
  char **strings;
  int i;

  /* Include a back-trace for good measure.
   *
   * NOTE: often values printed are addresses (no line numbers of function names),
   * this information can be expanded using `addr2line`, a utility is included to
   * conveniently run addr2line on the output generated here:
   *
   *   `./tools/utils/addr2line_backtrace.py --exe=/path/to/blender trace.txt`
   */
  nptrs = backtrace(buffer, SIZE);
  strings = backtrace_symbols(buffer, nptrs);
  for (i = 0; i < nptrs; i++) {
    fputs(strings[i], fp);
    fputc('\n', fp);
  }

  free(strings);
#    undef SIZE

#  else
  /* --------------------- */
  /* Non MSVC/Apple/Linux. */
  (void)fp;
#  endif
}
#endif
/* end BLI_system_backtrace_with_os_info */

void BLI_system_backtrace(FILE *fp)
{
  static Mutex mutex;
  std::scoped_lock lock(mutex);
  BLI_system_backtrace_with_os_info(fp, nullptr);
}

/* NOTE: The code for CPU brand string is adopted from Cycles. */

#if !defined(_WIN32) || defined(FREE_WINDOWS)
static void __cpuid(
    /* Cannot be const, because it is modified below.
     * NOLINTNEXTLINE: readability-non-const-parameter. */
    int data[4],
    int selector)
{
#  if defined(__x86_64__)
  asm("cpuid" : "=a"(data[0]), "=b"(data[1]), "=c"(data[2]), "=d"(data[3]) : "a"(selector));
#  elif defined(__i386__)
  asm("pushl %%ebx    \n\t"
      "cpuid          \n\t"
      "movl %%ebx, %1 \n\t"
      "popl %%ebx     \n\t"
      : "=a"(data[0]), "=r"(data[1]), "=c"(data[2]), "=d"(data[3])
      : "a"(selector)
      : "ebx");
#  else
  (void)selector;
  data[0] = data[1] = data[2] = data[3] = 0;
#  endif
}
#endif

char *BLI_cpu_brand_string()
{
#if !defined(_M_ARM64)
  char buf[49] = {0};
  int result[4] = {0};
  __cpuid(result, 0x80000000);
  if (result[0] >= int(0x80000004)) {
    __cpuid(reinterpret_cast<int *>(buf + 0), 0x80000002);
    __cpuid(reinterpret_cast<int *>(buf + 16), 0x80000003);
    __cpuid(reinterpret_cast<int *>(buf + 32), 0x80000004);
    char *brand = BLI_strdup(buf);
    /* TODO(sergey): Make it a bit more presentable by removing trademark. */
    return brand;
  }
#else
  /* No CPUID on ARM64, so we pull from the registry (on Windows) instead. */
  DWORD processorNameStringLength = 255;
  char processorNameString[255];
  if (RegGetValueA(HKEY_LOCAL_MACHINE,
                   "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                   "ProcessorNameString",
                   RRF_RT_REG_SZ,
                   nullptr,
                   &processorNameString,
                   &processorNameStringLength) == ERROR_SUCCESS)
  {
    return BLI_strdup(processorNameString);
  }
#endif
  return nullptr;
}

int BLI_cpu_support_sse42()
{
#if !defined(_M_ARM64)
  int result[4], num;
  __cpuid(result, 0);
  num = result[0];

  if (num >= 1) {
    __cpuid(result, 0x00000001);
    return (result[2] & (int(1) << 20)) != 0;
  }
#endif
  return 0;
}

void BLI_hostname_get(char *buffer, size_t buffer_maxncpy)
{
#ifndef WIN32
  if (gethostname(buffer, buffer_maxncpy - 1) < 0) {
    BLI_strncpy(buffer, "-unknown-", buffer_maxncpy);
  }
  /* When `gethostname()` truncates, it doesn't guarantee the trailing `\0`. */
  buffer[buffer_maxncpy - 1] = '\0';
#else
  DWORD buffer_size_in_out = buffer_maxncpy;
  if (!GetComputerName(buffer, &buffer_size_in_out)) {
    BLI_strncpy(buffer, "-unknown-", buffer_maxncpy);
  }
#endif
}

size_t BLI_system_memory_max_in_megabytes()
{
  /* Maximum addressable bytes on this platform.
   *
   * NOTE: Due to the shift arithmetic this is a half of the memory. */
  const size_t limit_bytes_half = size_t(1) << (sizeof(size_t[8]) - 1);
  /* Convert it to megabytes and return. */
  return (limit_bytes_half >> 20) * 2;
}

int BLI_system_memory_max_in_megabytes_int()
{
  const size_t limit_megabytes = BLI_system_memory_max_in_megabytes();
  /* NOTE: The result will fit into integer. */
  return int(min_zz(limit_megabytes, size_t(INT_MAX)));
}


#if defined(__linux__)
/**
 * cgroup v2 で掛かっているメモリの上限と、その枠で今使っている量。取れなければ 0 を返す。
 *
 * ★なぜ要るか: `/proc/meminfo` は**機械全体**の数字で、容器(Flatpak / Docker / podman)や
 * `systemd-run -p MemoryMax=...` の中では嘘になる。枠が 2GB でも機械に 20GB 空いていれば
 * 「まだ 20GB ある」と読んでしまい、キャッシュを太らせたまま枠の外に出て殺される。
 * メモリの少ない機械ほど容器で動かす人が多いので、ここは両方を見て小さい方を採る。
 * `FALCON_MEM_CGROUP=0` で従来どおり(機械全体だけを見る)。
 */
static bool blender_cgroup_memory_enabled()
{
  static const bool enabled = []() {
    const char *env = getenv("FALCON_MEM_CGROUP");
    return (env == nullptr) ? true : atoi(env) != 0;
  }();
  return enabled;
}

static bool blender_cgroup_read_value(const char *relative_path, const char *name, size_t *r_value)
{
  char path[1024];
  SNPRINTF(path, "/sys/fs/cgroup%s/%s", relative_path, name);
  FILE *f = fopen(path, "r");
  if (f == nullptr) {
    return false;
  }
  char buf[64] = {0};
  const bool read_ok = fgets(buf, sizeof(buf), f) != nullptr;
  fclose(f);
  if (!read_ok || STRPREFIX(buf, "max")) {
    /* "max" = 上限なし。「0」ではないので区別する。 */
    return false;
  }
  unsigned long long value = 0;
  if (sscanf(buf, "%llu", &value) != 1) {
    return false;
  }
  *r_value = size_t(value);
  return true;
}

/** この処理が属する cgroup v2 の相対パス(`/proc/self/cgroup` の `0::` の行)。
 * 処理の一生の間変わらないので 1 回だけ読む(再生中は 100ms に 1 回ここを通り、
 * 先読みの糸からも呼ばれるので、初期化が 1 回で済む形にしてある)。 */
struct BlenderCGroupPath {
  bool ok = false;
  char path[1024] = {0};
};

static const BlenderCGroupPath &blender_cgroup_path()
{
  static const BlenderCGroupPath cached = []() {
    BlenderCGroupPath result;
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (f == nullptr) {
      return result;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
      if (STRPREFIX(line, "0::")) {
        char *start = line + 3;
        char *end = strchr(start, '\n');
        if (end != nullptr) {
          *end = '\0';
        }
        BLI_strncpy(result.path, STREQ(start, "/") ? "" : start, sizeof(result.path));
        result.ok = true;
        break;
      }
    }
    fclose(f);
    return result;
  }();
  return cached;
}

/** 枠の残り(上限 - 使用中)。枠が無い・読めない時は 0。 */
static size_t blender_cgroup_memory_available()
{
  if (!blender_cgroup_memory_enabled()) {
    return 0;
  }
  const BlenderCGroupPath &cg = blender_cgroup_path();
  if (!cg.ok) {
    return 0;
  }
  size_t limit = 0;
  if (!blender_cgroup_read_value(cg.path, "memory.max", &limit) || limit == 0) {
    return 0;
  }
  size_t current = 0;
  if (!blender_cgroup_read_value(cg.path, "memory.current", &current)) {
    return limit;
  }
  return (current >= limit) ? 0 : limit - current;
}

/** 枠の大きさそのもの(上限)。枠が無い・読めない時は 0。 */
static size_t blender_cgroup_memory_limit()
{
  if (!blender_cgroup_memory_enabled()) {
    return 0;
  }
  const BlenderCGroupPath &cg = blender_cgroup_path();
  if (!cg.ok) {
    return 0;
  }
  size_t limit = 0;
  if (!blender_cgroup_read_value(cg.path, "memory.max", &limit)) {
    return 0;
  }
  return limit;
}
#endif /* __linux__ */

size_t BLI_system_memory_available_in_bytes()
{
#if defined(WIN32)
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) {
    return 0;
  }
  return size_t(status.ullAvailPhys);
#elif defined(__linux__)
  /* `MemAvailable` is the kernel's own estimate of what a new allocation can get without
   * pushing the system into swap. It already accounts for reclaimable page cache, which is why
   * it is the right number here and `MemFree` is not. */
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == nullptr) {
    return 0;
  }
  char line[256];
  size_t bytes = 0;
  while (fgets(line, sizeof(line), f)) {
    unsigned long long kb = 0;
    if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
      bytes = size_t(kb) * 1024;
      break;
    }
  }
  fclose(f);
  /* 容器や systemd の枠の中では、機械全体の空きより枠の残りの方が小さい。小さい方を採る。 */
  const size_t cgroup_bytes = blender_cgroup_memory_available();
  if (cgroup_bytes != 0 && (bytes == 0 || cgroup_bytes < bytes)) {
    bytes = cgroup_bytes;
  }
  return bytes;
#else
  /* Unknown: callers must not read this as "no memory available". */
  return 0;
#endif
}

size_t BLI_system_memory_total_in_bytes()
{
#if defined(WIN32)
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) {
    return 0;
  }
  return size_t(status.ullTotalPhys);
#elif defined(__linux__)
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == nullptr) {
    return 0;
  }
  char line[256];
  size_t bytes = 0;
  while (fgets(line, sizeof(line), f)) {
    unsigned long long kb = 0;
    if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) {
      bytes = size_t(kb) * 1024;
      break;
    }
  }
  fclose(f);
  /* 枠の中では、機械の RAM でなく枠の大きさがその機械の「全部」。 */
  const size_t cgroup_bytes = blender_cgroup_memory_limit();
  if (cgroup_bytes != 0 && (bytes == 0 || cgroup_bytes < bytes)) {
    bytes = cgroup_bytes;
  }
  return bytes;
#else
  /* Unknown: callers must not read this as "no memory". */
  return 0;
#endif
}

void BLI_system_max_open_files_ensure()
{
  /* The Windows maximum is documented as 8192. */
  constexpr int max_open_files = 8192;
  bool ok = true;

#if defined(WIN32)
  if (_getmaxstdio() < max_open_files) {
    ok = _setmaxstdio(max_open_files) == max_open_files;
  }
#else
  struct rlimit limit;
  ok = getrlimit(RLIMIT_NOFILE, &limit) == 0;
  if (ok && limit.rlim_cur < rlim_t(max_open_files)) {
    limit.rlim_cur = std::min(rlim_t(max_open_files), limit.rlim_max);
    ok = setrlimit(RLIMIT_NOFILE, &limit) == 0;
  }
#endif

  if (!ok) {
    CLOG_DEBUG(&LOG,
               "Failed to ensure max open files is at least %d: %s",
               max_open_files,
               strerror(errno));
  }
}

}  // namespace blender
