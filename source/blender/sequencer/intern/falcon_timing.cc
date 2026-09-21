/* SPDX-FileCopyrightText: 2026 Falcon Engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BLI_time.h"

#include "SEQ_falcon_timing.hh"

namespace blender::seq::timing {

namespace {

struct Counters {
  std::atomic<uint64_t> ns[int(Stage::Count)];
  std::atomic<uint64_t> calls[int(Stage::Count)];
};

/** [0] = プレビュー・[1] = 本家の先読みジョブ。 */
Counters g_counters[2];

FILE *g_out = nullptr;

/** 環境変数を 1 回だけ読む。`1` なら標準出力、パスならそのファイルへ足していく。 */
bool timing_init()
{
  const char *env = getenv("FALCON_VSE_TIMING");
  if (env == nullptr || env[0] == '\0' || strcmp(env, "0") == 0) {
    return false;
  }
  if (strcmp(env, "1") == 0) {
    g_out = stdout;
    return true;
  }
  g_out = fopen(env, "a");
  if (g_out == nullptr) {
    /* 書けない時は黙って標準出力へ(測定が丸ごと消えるより良い)。 */
    g_out = stdout;
  }
  return true;
}

const char *stage_name(const Stage stage)
{
  switch (stage) {
    case Stage::Decode:
      return "decode";
    case Stage::Preprocess:
      return "preprocess";
    case Stage::Transform:
      return "transform";
    case Stage::Blend:
      return "blend";
    case Stage::Evict:
      return "evict";
    case Stage::Upload:
      return "upload";
    case Stage::GpuComposite:
      return "gpu_composite";
    case Stage::Count:
      break;
  }
  return "?";
}

}  // namespace

bool enabled()
{
  static const bool on = timing_init();
  return on;
}

void add(const Stage stage, const double seconds, const bool prefetch)
{
  if (!enabled()) {
    return;
  }
  Counters &counters = g_counters[prefetch ? 1 : 0];
  counters.ns[int(stage)].fetch_add(uint64_t(seconds * 1.0e9), std::memory_order_relaxed);
  counters.calls[int(stage)].fetch_add(1, std::memory_order_relaxed);
}

void frame_done(const int timeline_frame, const double wall_seconds, const bool prefetch)
{
  if (!enabled()) {
    return;
  }
  Counters &counters = g_counters[prefetch ? 1 : 0];
  char line[1024];
  int len = snprintf(line,
                     sizeof(line),
                     "{\"k\":\"vse_frame\",\"who\":\"%s\",\"frame\":%d,\"wall_ms\":%.3f",
                     prefetch ? "prefetch" : "preview",
                     timeline_frame,
                     wall_seconds * 1000.0);
  for (int i = 0; i < int(Stage::Count); i++) {
    /* ★足した分をそのまま出して 0 に戻す = この 1 コマの間に積まれた分。
     * 並列に走った仕事の**合計**なので、壁時計より大きくなることがある(それが読みたい)。 */
    const uint64_t ns = counters.ns[i].exchange(0, std::memory_order_relaxed);
    const uint64_t calls = counters.calls[i].exchange(0, std::memory_order_relaxed);
    len += snprintf(line + len,
                    sizeof(line) - size_t(len),
                    ",\"%s_ms\":%.3f,\"%s_n\":%llu",
                    stage_name(Stage(i)),
                    double(ns) / 1.0e6,
                    stage_name(Stage(i)),
                    static_cast<unsigned long long>(calls));
    if (len >= int(sizeof(line)) - 64) {
      break;
    }
  }
  snprintf(line + len, sizeof(line) - size_t(len), "}\n");
  fputs(line, g_out);
  fflush(g_out);
}

Scope::Scope(const Stage stage, const bool prefetch)
    : stage_(stage), prefetch_(prefetch), start_(enabled() ? BLI_time_now_seconds() : 0.0)
{
}

Scope::~Scope()
{
  if (start_ != 0.0) {
    add(stage_, BLI_time_now_seconds() - start_, prefetch_);
  }
}

}  // namespace blender::seq::timing
