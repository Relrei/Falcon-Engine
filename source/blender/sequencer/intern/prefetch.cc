/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "MEM_guardedalloc.h"

#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_sequence_types.h"
#include "DNA_space_types.h"

#include "BLI_threads.h"
#include "BLI_vector.hh"
#include "BLI_system.h"
#include "BLI_task.hh"

#include "IMB_imbuf.hh"

#include "BKE_anim_data.hh"
#include "BKE_animsys.h"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_debug.hh"
#include "DEG_depsgraph_query.hh"

#include "GPU_context.hh"

#include "SEQ_channels.hh"
#include "SEQ_iterator.hh"
#include "SEQ_prefetch.hh"
#include "SEQ_relations.hh"
#include "SEQ_render.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_time.hh"

#include "SEQ_gpu_preview.hh"

#include "prefetch.hh"
#include "render.hh"

namespace blender {

struct RenderResult;
struct Scene;
struct ThreadSlot;

namespace seq {

/* Prefetch several frames before the playhead, so that it is fast to move it a bit backwards. */
static constexpr int before_playhead_frames = 5;

struct PrefetchJob {
  PrefetchJob *next = nullptr;
  PrefetchJob *prev = nullptr;

  Main *bmain = nullptr;
  Main *bmain_eval = nullptr;
  Scene *scene = nullptr;
  Scene *scene_eval = nullptr;
  Depsgraph *depsgraph = nullptr;

  ThreadMutex prefetch_suspend_mutex = {};
  ThreadCondition prefetch_suspend_cond = {};

  ListBaseT<ThreadSlot> threads = {};

  /* context */
  RenderData context = {};
  RenderData context_cpy = {};

  /* prefetch area */
  int cfra = 0;
  int timeline_start = 0;
  int timeline_end = 0;
  int timeline_length = 0;
  int num_frames_prefetched = 0;
  int cache_flags = 0; /* Only used to detect cache flag changes. */
  /* Falcon: 画像の連番を先回りして並列に復号した範囲 [from, until)。`falcon_prefetch_decode_ahead`。 */
  int falcon_decoded_from = 0;
  int falcon_decoded_until = 0;

  /* Control: */
  /* Set by prefetch. */
  bool running = false;
  bool waiting = false;
  bool stop = false;
  /* Set from outside. */
  bool is_scrubbing = false;

 public:
  void init_depsgraph();
  void free_depsgraph();

  void init_gpu();
  void free_gpu();
};

static PrefetchJob *seq_prefetch_job_get(Scene *scene)
{
  if (scene && scene->ed) {
    return scene->ed->runtime->prefetch_job;
  }
  return nullptr;
}

bool seq_prefetch_job_is_running(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (!pfjob) {
    return false;
  }

  return pfjob->running;
}

static void seq_prefetch_job_scrubbing_set(Scene *scene, bool is_scrubbing)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (!pfjob) {
    return;
  }

  pfjob->is_scrubbing = is_scrubbing;
}

static bool seq_prefetch_job_is_waiting(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (!pfjob) {
    return false;
  }

  return pfjob->waiting;
}

static Strip *original_strip_get(const Strip *strip, ListBaseT<Strip> *seqbase)
{
  for (Strip &strip_orig : *seqbase) {
    if (STREQ(strip->name, strip_orig.name)) {
      return &strip_orig;
    }

    if (strip_orig.type == STRIP_TYPE_META) {
      Strip *match = original_strip_get(strip, &strip_orig.seqbase);
      if (match != nullptr) {
        return match;
      }
    }
  }

  return nullptr;
}

static Strip *original_strip_get(const Strip *strip, Scene *scene)
{
  Editing *ed = scene->ed;
  return original_strip_get(strip, &ed->seqbase);
}

static RenderData *get_original_context(const RenderData *context)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(context->scene);
  return pfjob ? &pfjob->context : nullptr;
}

Scene *prefetch_get_original_scene(const RenderData *context)
{
  Scene *scene = context->scene;
  if (context->is_prefetch_render) {
    context = get_original_context(context);
    if (context != nullptr) {
      scene = context->scene;
    }
  }
  return scene;
}

Scene *prefetch_get_original_scene_and_strip(const RenderData *context, const Strip *&strip)
{
  Scene *scene = context->scene;
  if (context->is_prefetch_render) {
    context = get_original_context(context);
    if (context != nullptr) {
      scene = context->scene;
      strip = original_strip_get(strip, scene);
    }
  }
  return scene;
}

static bool seq_prefetch_is_cache_full(Scene *scene)
{
  return evict_caches_if_full(scene);
}

static int seq_prefetch_cfra(PrefetchJob *pfjob)
{
  int new_frame = pfjob->cfra + pfjob->num_frames_prefetched;
  const ScenePlaybackRange playback_range = BKE_scene_get_playback_range(pfjob->scene);
  if (new_frame >= playback_range.end_frame) {
    /* Wrap around to where we will jump when we reach the end frame. */
    new_frame = playback_range.start_frame + new_frame - playback_range.end_frame;
  }
  return new_frame;
}

static AnimationEvalContext seq_prefetch_anim_eval_context(PrefetchJob *pfjob)
{
  return BKE_animsys_eval_context_construct(pfjob->depsgraph, seq_prefetch_cfra(pfjob));
}

void seq_prefetch_get_time_range(Scene *scene, int *r_start, int *r_end)
{
  /* When there is no prefetch job, return "impossible" negative values. */
  *r_start = std::numeric_limits<int>::min();
  *r_end = std::numeric_limits<int>::min();

  PrefetchJob *pfjob = seq_prefetch_job_get(scene);
  if (pfjob == nullptr) {
    return;
  }
  if ((scene->ed->cache_flag & SEQ_CACHE_PREFETCH_ENABLE) == 0 || !pfjob->running) {
    return;
  }

  *r_start = pfjob->cfra;
  *r_end = seq_prefetch_cfra(pfjob);
}

void PrefetchJob::free_depsgraph()
{
  if (this->depsgraph != nullptr) {
    DEG_graph_free(this->depsgraph);
  }
  this->depsgraph = nullptr;
  this->scene_eval = nullptr;
}

static void seq_prefetch_update_depsgraph(PrefetchJob *pfjob)
{
  DEG_evaluate_on_framechange(pfjob->depsgraph, seq_prefetch_cfra(pfjob));
  /* Prevent depsgraph from copying scene data to evaluated scene. It would reset updated frame. */
  DEG_ids_clear_recalc(pfjob->depsgraph, false);
}

void PrefetchJob::init_depsgraph()
{
  ViewLayer *view_layer = BKE_view_layer_default_render(this->scene);

  this->depsgraph = DEG_graph_new(this->bmain_eval, this->scene, view_layer, DAG_EVAL_RENDER);
  DEG_debug_name_set(this->depsgraph, "SEQUENCER PREFETCH");

  /* Make sure there is a correct evaluated scene pointer. */
  DEG_graph_build_for_render_pipeline(this->depsgraph);

  /* Update immediately so we have proper evaluated scene. */
  seq_prefetch_update_depsgraph(this);

  this->scene_eval = DEG_get_evaluated_scene(this->depsgraph);
  this->scene_eval->ed->cache_flag = SEQ_CACHE_NONE;
}

void PrefetchJob::init_gpu()
{
  this->context_cpy.gpu_context = gpu::GPU_create_secondary_context();
}

void PrefetchJob::free_gpu()
{
  if (this->context_cpy.gpu_context.ghost_context != nullptr) {
    gpu::GPU_destroy_secondary_context(this->context_cpy.gpu_context);
    this->context_cpy.gpu_context = {};
  }
}

static void seq_prefetch_update_area(PrefetchJob *pfjob)
{
  int cfra = math::max(pfjob->scene->r.cfra - before_playhead_frames, pfjob->timeline_start);

  /* rebase */
  if (cfra > pfjob->cfra) {
    int delta = cfra - pfjob->cfra;
    pfjob->cfra = cfra;
    pfjob->num_frames_prefetched -= delta;

    pfjob->num_frames_prefetched = std::max(pfjob->num_frames_prefetched, 0);
  }

  /* reset */
  if (cfra < pfjob->cfra) {
    pfjob->cfra = cfra;
    pfjob->num_frames_prefetched = 0;
  }

  /* timeline span changes */
  const ScenePlaybackRange playback_range = BKE_scene_get_playback_range(pfjob->scene);
  if (pfjob->timeline_start != playback_range.start_frame ||
      pfjob->timeline_end != playback_range.end_frame)
  {
    pfjob->timeline_start = playback_range.start_frame;
    pfjob->timeline_end = playback_range.end_frame;
    pfjob->timeline_length = playback_range.end_frame - playback_range.start_frame;
    /* Reset the number of prefetched frames as we need to re-evaluate which
     * frames to keep in the cache.
     */
    pfjob->num_frames_prefetched = 0;
  }

  /* cache flag changes */
  Scene *scene = pfjob->scene;
  if (pfjob->cache_flags != scene->ed->cache_flag) {
    pfjob->cache_flags = scene->ed->cache_flag;
    pfjob->num_frames_prefetched = 0;
  }
}

void prefetch_stop_all()
{
  /* TODO(Richard): Use wm_jobs for prefetch, or pass main. */
  for (Scene *scene = static_cast<Scene *>(G.main->scenes.first); scene;
       scene = static_cast<Scene *>(scene->id.next))
  {
    prefetch_stop(scene);
  }
}

void prefetch_stop(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (!pfjob) {
    return;
  }

  pfjob->stop = true;

  while (pfjob->running) {
    BLI_condition_notify_one(&pfjob->prefetch_suspend_cond);
  }
}

static void seq_prefetch_update_context(const RenderData *context)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(context->scene);

  render_new_render_data(pfjob->bmain_eval,
                         pfjob->depsgraph,
                         pfjob->scene_eval,
                         context->rectx,
                         context->recty,
                         context->preview_render_size,
                         nullptr,
                         &pfjob->context_cpy);
  pfjob->context_cpy.is_prefetch_render = true;

  render_new_render_data(pfjob->bmain,
                         pfjob->depsgraph,
                         pfjob->scene,
                         context->rectx,
                         context->recty,
                         context->preview_render_size,
                         nullptr,
                         &pfjob->context);
  pfjob->context.is_prefetch_render = false;
}

static void seq_prefetch_update_scene(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (!pfjob) {
    return;
  }

  pfjob->scene = scene;
  pfjob->free_depsgraph();
  pfjob->init_depsgraph();
}

static void seq_prefetch_update_active_seqbase(PrefetchJob *pfjob)
{
  MetaStack *ms_orig = meta_stack_active_get(editing_get(pfjob->scene));
  Editing *ed_eval = editing_get(pfjob->scene_eval);

  if (ms_orig != nullptr) {
    Strip *meta_eval = original_strip_get(ms_orig->parent_strip, pfjob->scene_eval);
    ed_eval->current_meta_strip = meta_eval;
  }
  else {
    ed_eval->current_meta_strip = nullptr;
  }
}

static void seq_prefetch_resume(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);

  if (pfjob && pfjob->waiting) {
    BLI_condition_notify_one(&pfjob->prefetch_suspend_cond);
  }
}

void seq_prefetch_free(Scene *scene)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(scene);
  if (!pfjob) {
    return;
  }

  prefetch_stop(scene);

  BLI_threadpool_remove(&pfjob->threads, pfjob);
  BLI_threadpool_end(&pfjob->threads);
  BLI_mutex_end(&pfjob->prefetch_suspend_mutex);
  BLI_condition_end(&pfjob->prefetch_suspend_cond);
  pfjob->free_depsgraph();
  pfjob->free_gpu();
  BKE_main_free(pfjob->bmain_eval);
  scene->ed->runtime->prefetch_job = nullptr;
  MEM_delete(pfjob);
}

static bool strip_renders_scene_strip(const Scene *scene,
                                      ListBaseT<SeqTimelineChannel> *channels,
                                      ListBaseT<Strip> *seqbase,
                                      Strip *strip,
                                      int frame,
                                      SeqRenderState state);

/* Find whether any strip shown in `seqbase` renders a camera-input or recursive scene strip. */
static bool seqbase_renders_scene_strip(const Scene *scene,
                                        ListBaseT<SeqTimelineChannel> *channels,
                                        ListBaseT<Strip> *seqbase,
                                        int frame,
                                        SeqRenderState state)
{
  for (Strip *strip : query_rendered_strips_sorted(scene, channels, seqbase, frame, 0)) {
    if (strip_renders_scene_strip(scene, channels, seqbase, strip, frame, state)) {
      return true;
    }
  }
  return false;
}

/* Find whether rendering `strip` directly or indirectly renders a camera-input or recursive scene
 * strip. */
static bool strip_renders_scene_strip(const Scene *scene,
                                      ListBaseT<SeqTimelineChannel> *channels,
                                      ListBaseT<Strip> *seqbase,
                                      Strip *strip,
                                      int frame,
                                      SeqRenderState state)
{
  /* Recursive sequencer-input scene strip detected, no point in attempting to render it. */
  if (state.strips_in_progress.contains(strip)) {
    return true;
  }

  /* Camera-input scene strip detected. */
  if (strip->type == STRIP_TYPE_SCENE && (strip->flag & SEQ_SCENE_STRIPS) == 0 &&
      strip->scene != nullptr)
  {
    return true;
  }

  /* Recurse on effect input strips. */
  if ((strip->input1 &&
       strip_renders_scene_strip(scene, channels, seqbase, strip->input1, frame, state)) ||
      (strip->input2 &&
       strip_renders_scene_strip(scene, channels, seqbase, strip->input2, frame, state)))
  {
    return true;
  }

  /* Recurse on mask modifier strips. */
  for (StripModifierData &smd : strip->modifiers) {
    if (smd.mask_strip &&
        strip_renders_scene_strip(scene, channels, seqbase, smd.mask_strip, frame, state))
    {
      return true;
    }
  }

  /* Adjustment strips with 'replace' blending indirectly render all strips in channels below them.
   * See #151629. */
  if (strip->type == STRIP_TYPE_ADJUSTMENT && strip->blend_mode == STRIP_BLEND_REPLACE &&
      strip->channel > 1)
  {
    for (Strip *below :
         query_rendered_strips_sorted(scene, channels, seqbase, frame, strip->channel - 1))
    {
      if (strip_renders_scene_strip(scene, channels, seqbase, below, frame, state)) {
        return true;
      }
    }
  }

  /* Recurse on all strips in the meta strip `seqbase`. */
  if (strip->type == STRIP_TYPE_META &&
      seqbase_renders_scene_strip(scene, &strip->channels, &strip->seqbase, frame, state))
  {
    return true;
  }

  /* Recurse on all strips in the sequencer-input scene strip `seqbase`. */
  if (strip->type == STRIP_TYPE_SCENE && (strip->flag & SEQ_SCENE_STRIPS) != 0 &&
      strip->scene != nullptr && editing_get(strip->scene))
  {
    state.strips_in_progress.add(strip);

    const Scene *target_scene = strip->scene;
    Editing *target_ed = editing_get(target_scene);
    int target_timeline_frame = give_frame_index(scene, strip, frame) + target_scene->r.sfra;

    if (seqbase_renders_scene_strip(target_scene,
                                    target_ed->current_channels(),
                                    target_ed->current_strips(),
                                    target_timeline_frame,
                                    state))
    {
      return true;
    }
  }

  return false;
}

static bool seq_prefetch_must_skip_frame(PrefetchJob *pfjob)
{
  const Scene *scene = pfjob->scene_eval;
  const Editing *ed = editing_get(pfjob->scene_eval);
  ListBaseT<Strip> *seqbase = active_seqbase_get(ed);
  ListBaseT<SeqTimelineChannel> *channels = channels_displayed_get(ed);
  int timeline_frame = seq_prefetch_cfra(pfjob);

  /* Do not render the current frame from the prefetch: a user might be interactively
   * editing some animated property, and the scene copy inside prefetch would not get
   * that temporary edited value. */
  if (timeline_frame == pfjob->scene->r.cfra) {
    return true;
  }

  /* Pass in state to check for infinite recursion of "sequencer-type" scene strips. */
  SeqRenderState state = {};

  /* Camera-input scene strips are not supported, nor are recursive sequencer-input scene
   * strips. */
  return seqbase_renders_scene_strip(scene, channels, seqbase, timeline_frame, state);
}

/**
 * 先読みが再生ヘッドの何コマ先まで走るか。`FALCON_VSE_PREFETCH_WINDOW_S` 秒で指定し、
 * 0(既定)なら上流どおり = タイムラインの端まで走る。
 *
 * ★なぜ要るか(2026-09-20 実測・メモリの少ない機械向け): 上流の先読みは端まで走り、
 * 走った範囲は `source_image_cache_evict()` / `final_image_cache_evict()` が
 * 「先読みの範囲は捨てない」規則で守る。その結果、キャッシュが上限に達すると
 * **捨てられる物が 1 枚も無くなり、先読みジョブが眠る**。眠った後の再生は素の同期復号の
 * 速さまで落ちる(FHD H.264 4 本で 4〜6fps -> 0.8〜1.5fps・仮)。
 * 窓を切っておけば、守るのは「再生ヘッドの少し先」だけになり、後ろは常に捨てられるので、
 * 予算が小さくても「貯めては入れ替える」形で回り続ける。
 */
static int seq_prefetch_window_frames(const Scene *scene)
{
  static const float window_sec = []() {
    const char *env = getenv("FALCON_VSE_PREFETCH_WINDOW_S");
    return (env == nullptr) ? 0.0f : std::max(0.0f, float(atof(env)));
  }();
  if (window_sec <= 0.0f) {
    return std::numeric_limits<int>::max();
  }
  const float fps = float(scene->r.frs_sec) / std::max(float(scene->r.frs_sec_base), 1e-6f);
  return std::max(1, int(window_sec * fps));
}

/** 先読みが走ってよい範囲。GPU 経路の時は仕上がりを置く輪の大きさで頭を押さえる。 */
static int seq_prefetch_window_frames_effective(const Scene *scene)
{
  int frames = seq_prefetch_window_frames(scene);
  if (gpu_preview_enabled() && gpu_preview_is_active()) {
    /* ★輪より先へ行っても押し出されるだけ。行った先の仕事は丸ごと捨てることになる。 */
    const int ahead = gpu_preview_ahead_frames();
    if (ahead > 0) {
      frames = std::min(frames, ahead);
    }
  }
  return frames;
}

static bool seq_prefetch_need_suspend(PrefetchJob *pfjob)
{
  return seq_prefetch_is_cache_full(pfjob->scene) || pfjob->is_scrubbing ||
         (pfjob->num_frames_prefetched >= pfjob->timeline_length) ||
         (pfjob->num_frames_prefetched >= seq_prefetch_window_frames_effective(pfjob->scene));
}

static void seq_prefetch_do_suspend(PrefetchJob *pfjob)
{
  BLI_mutex_lock(&pfjob->prefetch_suspend_mutex);
  while (seq_prefetch_need_suspend(pfjob) &&
         (pfjob->scene->ed->cache_flag & SEQ_CACHE_PREFETCH_ENABLE) && !pfjob->stop)
  {
    pfjob->waiting = true;
    BLI_condition_wait(&pfjob->prefetch_suspend_cond, &pfjob->prefetch_suspend_mutex);
    seq_prefetch_update_area(pfjob);
  }
  pfjob->waiting = false;
  BLI_mutex_unlock(&pfjob->prefetch_suspend_mutex);
}

/* 画像の連番を先回りして並列に復号する(`falcon_decode_images_ahead` の注記)。 */
static void falcon_prefetch_decode_ahead(PrefetchJob *pfjob)
{
  const int threads = falcon_image_decode_threads();
  if (threads <= 1) {
    return;
  }
  const int cfra = seq_prefetch_cfra(pfjob);
  if (cfra >= pfjob->falcon_decoded_from && cfra < pfjob->falcon_decoded_until) {
    return; /* この範囲はもう済んでいる。 */
  }
  const int last = std::min(cfra + threads * 2 - 1, pfjob->timeline_end);
  pfjob->falcon_decoded_from = cfra;
  pfjob->falcon_decoded_until = last + 1;
  falcon_decode_images_ahead(&pfjob->context_cpy, pfjob->scene_eval, cfra, last, &pfjob->stop);
}

static void *seq_prefetch_frames(void *job)
{
  PrefetchJob *pfjob = static_cast<PrefetchJob *>(job);

  while (true) {
    if (pfjob->cfra < pfjob->timeline_start || pfjob->cfra > pfjob->timeline_end) {
      /* Don't try to prefetch anything when we are outside of the timeline range. */
      break;
    }
    pfjob->scene_eval->ed->runtime->prefetch_job = nullptr;

    seq_prefetch_update_depsgraph(pfjob);
    AnimData *adt = BKE_animdata_from_id(&pfjob->context_cpy.scene->id);
    AnimationEvalContext anim_eval_context = seq_prefetch_anim_eval_context(pfjob);
    BKE_animsys_evaluate_animdata(
        &pfjob->context_cpy.scene->id, adt, &anim_eval_context, ADT_RECALC_ALL, false);

    /* This is quite hacky solution:
     * We need cross-reference original scene with copy for cache.
     * However depsgraph must not have this data, because it will try to kill this job.
     * Scene copy don't reference original scene. Perhaps, this could be done by depsgraph.
     * Set to nullptr before return!
     */
    pfjob->scene_eval->ed->runtime->prefetch_job = pfjob;

    if (seq_prefetch_must_skip_frame(pfjob)) {
      pfjob->num_frames_prefetched++;
      /* Break instead of keep looping if the job should be terminated. */
      if (!(pfjob->scene->ed->cache_flag & SEQ_CACHE_PREFETCH_ENABLE) ||
          !(pfjob->scene->ed->cache_flag & SEQ_CACHE_ALL_TYPES) || pfjob->stop)
      {
        break;
      }
      continue;
    }

    falcon_prefetch_decode_ahead(pfjob);
    ImBuf *ibuf = render_give_ibuf(&pfjob->context_cpy, seq_prefetch_cfra(pfjob), 0);
    pfjob->num_frames_prefetched++;
    IMB_freeImBuf(ibuf);

    /* Suspend thread if there is nothing to be prefetched. */
    seq_prefetch_do_suspend(pfjob);

    if (!(pfjob->scene->ed->cache_flag & SEQ_CACHE_PREFETCH_ENABLE) ||
        !(pfjob->scene->ed->cache_flag & SEQ_CACHE_ALL_TYPES) || pfjob->stop)
    {
      break;
    }

    seq_prefetch_update_area(pfjob);
  }

  pfjob->running = false;
  pfjob->scene_eval->ed->runtime->prefetch_job = nullptr;

  return nullptr;
}

static PrefetchJob *seq_prefetch_start_ex(const RenderData *context, float cfra)
{
  PrefetchJob *pfjob = seq_prefetch_job_get(context->scene);

  if (!pfjob) {
    if (!context->scene->ed) {
      return nullptr;
    }
    pfjob = MEM_new<PrefetchJob>("PrefetchJob");
    context->scene->ed->runtime->prefetch_job = pfjob;

    BLI_threadpool_init(&pfjob->threads, seq_prefetch_frames, 1);
    BLI_mutex_init(&pfjob->prefetch_suspend_mutex);
    BLI_condition_init(&pfjob->prefetch_suspend_cond);

    pfjob->bmain_eval = BKE_main_new();
    pfjob->scene = context->scene;
    pfjob->init_depsgraph();
    pfjob->init_gpu();
  }
  pfjob->bmain = context->bmain;

  Scene *scene = pfjob->scene;
  const ScenePlaybackRange playback_range = BKE_scene_get_playback_range(pfjob->scene);
  pfjob->timeline_start = playback_range.start_frame;
  pfjob->timeline_end = playback_range.end_frame;
  pfjob->timeline_length = playback_range.end_frame - playback_range.start_frame;

  pfjob->cfra = math::max(int(cfra - before_playhead_frames), pfjob->timeline_start);

  pfjob->num_frames_prefetched = 0;
  pfjob->cache_flags = scene->ed->cache_flag;

  pfjob->waiting = false;
  pfjob->stop = false;
  pfjob->running = true;

  seq_prefetch_update_scene(context->scene);
  seq_prefetch_update_context(context);
  seq_prefetch_update_active_seqbase(pfjob);

  BLI_threadpool_remove(&pfjob->threads, pfjob);
  BLI_threadpool_insert(&pfjob->threads, pfjob);

  return pfjob;
}

void seq_prefetch_start(const RenderData *context, float timeline_frame)
{
  Scene *scene = context->scene;
  Editing *ed = scene->ed;
  bool has_strips = bool(ed->current_strips()->first);

  if (!context->is_prefetch_render) {
    bool playing = context->is_playing;
    bool scrubbing = context->is_scrubbing;
    bool running = seq_prefetch_job_is_running(scene);
    seq_prefetch_job_scrubbing_set(scene, scrubbing);
    seq_prefetch_resume(scene);

    /* conditions to start:
     * prefetch enabled, prefetch not running, not scrubbing, not playing,
     * cache storage enabled, has strips to render, not rendering, not doing modal transform -
     * important, see D7820. */
    if ((ed->cache_flag & SEQ_CACHE_PREFETCH_ENABLE) && !running && !scrubbing && !playing &&
        (ed->cache_flag & SEQ_CACHE_ALL_TYPES) && has_strips && !G.is_rendering && !G.moving)
    {
      seq_prefetch_start_ex(context, timeline_frame);
    }
  }
}

bool prefetch_need_redraw(const bContext *C, Scene *scene)
{
  bScreen *screen = CTX_wm_screen(C);
  bool playing = screen->animtimer != nullptr;
  bool scrubbing = screen->scrubbing;
  bool running = seq_prefetch_job_is_running(scene);
  bool suspended = seq_prefetch_job_is_waiting(scene);

  SpaceSeq *sseq = CTX_wm_space_seq(C);
  bool showing_cache = sseq->cache_overlay.flag & SEQ_CACHE_SHOW;

  /* force redraw, when prefetching and using cache view. */
  if (running && !playing && !suspended && showing_cache) {
    return true;
  }
  /* Sometimes scrubbing flag is set when not scrubbing. In that case I want to catch "event" of
   * stopping scrubbing */
  if (scrubbing) {
    return true;
  }
  return false;
}

}  // namespace seq
}  // namespace blender
