/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_mem_reclaim.hh"

#include <cstdio>
#include <cstring>

#ifndef _WIN32
#  include <dlfcn.h>
#endif

#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_time.h"

namespace blender::bke {

/* `int scalable_allocation_command(int cmd, void *param)` from `tbbmalloc`.
 * 0 == `TBBMALLOC_CLEAN_ALL_BUFFERS`, 0 == `TBBMALLOC_OK`,
 * 4 == `TBBMALLOC_NO_EFFECT` (nothing was held). */
using ScalableAllocationCommandFn = int (*)(int, void *);
static constexpr int TBBMALLOC_CLEAN_ALL_BUFFERS = 0;

static ScalableAllocationCommandFn reclaim_fn_get()
{
  static ScalableAllocationCommandFn fn = []() -> ScalableAllocationCommandFn {
#ifdef _WIN32
    return nullptr;
#else
    /* The proxy pulls `libtbbmalloc` into the global scope, so `RTLD_DEFAULT` finds
     * it without this having to link or dlopen anything. Null in a build without
     * the TBB malloc proxy, which makes every call below a no-op. */
    return reinterpret_cast<ScalableAllocationCommandFn>(
        dlsym(RTLD_DEFAULT, "scalable_allocation_command"));
#endif
  }();
  return fn;
}

static bool reclaim_enabled()
{
  static const bool enabled = []() {
    const char *env = BLI_getenv("FALCON_MEM_RECLAIM");
    /* On unless it is switched off explicitly. */
    return env == nullptr || !STREQ(env, "0");
  }();
  return enabled;
}

static bool reclaim_debug()
{
  static const bool debug = []() {
    const char *env = BLI_getenv("FALCON_MEM_RECLAIM_DEBUG");
    return env != nullptr && !STREQ(env, "0");
  }();
  return debug;
}

void mem_reclaim_to_os(const char *where)
{
  if (!reclaim_enabled()) {
    return;
  }
  const ScalableAllocationCommandFn fn = reclaim_fn_get();
  if (fn == nullptr) {
    return;
  }
  const double time_start = reclaim_debug() ? BLI_time_now_seconds() : 0.0;
  const int result = fn(TBBMALLOC_CLEAN_ALL_BUFFERS, nullptr);
  if (reclaim_debug()) {
    printf("FALCON_MEM_RECLAIM: %s -> rc=%d (%.1f ms)\n",
           where ? where : "?",
           result,
           (BLI_time_now_seconds() - time_start) * 1000.0);
    fflush(stdout);
  }
}

/* Nesting depth of the renders on this thread. A render always finishes on the
 * thread it started on, so this does not need to be atomic. */
static thread_local int reclaim_depth = 0;

MemReclaimScope::MemReclaimScope(const char *where) : where_(where)
{
  reclaim_depth++;
}

MemReclaimScope::~MemReclaimScope()
{
  reclaim_depth--;
  if (reclaim_depth == 0) {
    mem_reclaim_to_os(where_);
  }
}

}  // namespace blender::bke
