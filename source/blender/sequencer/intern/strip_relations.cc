/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2003-2009 Blender Authors
 * SPDX-FileCopyrightText: 2005-2006 Peter Schlaile <peter [at] schlaile [dot] de>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include <algorithm>
#include <atomic>
#include <cstdlib>

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_system.h"
#include "BLI_time.h"
#include "BLI_session_uid.h"
#include "BLI_string.h"

#include "IMB_imbuf.hh"

#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"

#include "MOV_read.hh"

#include "SEQ_iterator.hh"
#include "SEQ_prefetch.hh"
#include "SEQ_preview_cache.hh"
#include "SEQ_relations.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_thumbnail_cache.hh"
#include "SEQ_utils.hh"

#include "prefetch.hh"

#include "cache/final_image_cache.hh"
#include "cache/intra_frame_cache.hh"
#include "cache/source_image_cache.hh"
#include "effects/effects.hh"
#include "sequencer.hh"
#include "utils.hh"

namespace blender::seq {

bool relation_is_effect_of_strip(const Strip *effect, const Strip *input)
{
  return ELEM(input, effect->input1, effect->input2);
}

void cache_cleanup(Scene *scene, CacheCleanup mode)
{
  if (flag_is_set(mode, CacheCleanup::Thumbnails)) {
    thumbnail_cache_clear(scene);
  }
  if (flag_is_set(mode, CacheCleanup::SourceImage)) {
    source_image_cache_clear(scene);
  }
  if (flag_is_set(mode, CacheCleanup::FinalImage)) {
    final_image_cache_clear(scene);
  }
  if (flag_is_set(mode, CacheCleanup::IntraFrame)) {
    intra_frame_cache_invalidate(scene);
    preview_cache_invalidate(scene);
  }
}

void cache_settings_changed(Scene *scene)
{
  if (!(scene->ed->cache_flag & SEQ_CACHE_STORE_RAW)) {
    /* RAW caches has been disabled, clear them out. */
    source_image_cache_clear(scene);
  }
  if (!(scene->ed->cache_flag & SEQ_CACHE_STORE_FINAL_OUT)) {
    /* Final caches has been disabled, clear them out. */
    final_image_cache_clear(scene);
  }
}

/**
 * 空きメモリがこれを割ったら、設定した上限に達していなくてもキャッシュを手放す (MB)。
 * `FALCON_VSE_MEM_FLOOR_MB=0` で無効(従来どおり上限だけで判断する)。
 *
 * ★なぜ要るか: `U.memcachelimit` は「VSE のキャッシュがどこまで太ってよいか」しか見ておらず、
 * **機械の空きメモリを誰も見ていない**。設定値を大きくすると、空きが尽きかけていても
 * 1枚も捨てないので、他の作業(モデリング・アニメーション)ごと機械が溢れる。
 * しかも同じ `U.memcachelimit` は ImBuf のキャッシュ制限と汎用メモリキャッシュにも
 * 別々に渡されているので、実際の天井は設定値の数倍になりうる。
 */
static size_t seq_cache_memory_floor_bytes()
{
  static const size_t floor_bytes = []() -> size_t {
    const char *env = getenv("FALCON_VSE_MEM_FLOOR_MB");
    int mb = 1024;
    if (env != nullptr) {
      mb = std::max(0, atoi(env));
    }
    return size_t(mb) * 1024 * 1024;
  }();
  return floor_bytes;
}

/**
 * 「まだ捨てはしないが、これ以上は太らせない」帯の下限 (MB)。
 * `FALCON_VSE_MEM_SOFT_MB=0` で無効(2026-09-09 以前の挙動 = 崖だけ)。
 *
 * ★なぜ要るか (2026-09-10 実測): 上の非常ブレーキは「空き 1GB」まで**一切効かない**。
 * 400 コマ・1080p の 2 周目の中央値で
 *   圧力なし 2.17ms/コマ (461fps) 対 常時圧力 6.42ms/コマ (156fps)
 * = 掛かった瞬間に **3 倍遅くなる崖**。作者「VSE 再生時なぜか FPS 低下、
 * またメモリ管理がおかしい挙動をしてる、安定しない」の形はこれ。
 *
 * 崖の手前に1段置く: 空きがこの値を割ったら**新しくキャッシュへ入れるのをやめる**
 * (既に入っている物は捨てない)。太るのが止まるだけなので、
 * 既に温まっている再生は速度を保ったまま頭打ちになる。
 */
static size_t seq_cache_soft_floor_bytes()
{
  static const size_t soft_bytes = []() -> size_t {
    const char *env = getenv("FALCON_VSE_MEM_SOFT_MB");
    int mb = 4096;
    if (env != nullptr) {
      mb = std::max(0, atoi(env));
    }
    return size_t(mb) * 1024 * 1024;
  }();
  return soft_bytes;
}

/**
 * 機械の空きメモリが下限を割っているか。分からない時は false。
 *
 * ⚠**「分からない」を「空きが無い」と読まないこと。**
 * #BLI_system_memory_available_in_bytes は対応していないプラットフォームで 0 を返す。
 * そこで真を返すと、そのプラットフォームでは常時キャッシュが空になる。
 *
 * `/proc/meminfo` の読み出しは 100ms に1回までに間引く。この関数は追い出しのループの中から
 * 呼ばれるので、1回の追い出しの間は同じ答えを返す = 「下限を割っていたら、下の keep 分まで
 * 縮めて止まる」という決まった動きになる。
 */
static size_t seq_system_memory_available_throttled()
{
  static std::atomic<double> last_check_time{-1.0};
  static std::atomic<size_t> last_available{0};
  const double now = BLI_time_now_seconds();
  const double last = last_check_time.load(std::memory_order_relaxed);
  if (last >= 0.0 && now - last < 0.1) {
    return last_available.load(std::memory_order_relaxed);
  }
  const size_t available = BLI_system_memory_available_in_bytes();
  last_available.store(available, std::memory_order_relaxed);
  last_check_time.store(now, std::memory_order_relaxed);
  return available;
}

static bool seq_system_memory_is_below(const size_t floor_bytes)
{
  if (floor_bytes == 0) {
    return false;
  }
  const size_t available = seq_system_memory_available_throttled();
  /* 0 = 取れなかった。「空きが無い」ではない。 */
  return (available != 0) && (available < floor_bytes);
}

static bool seq_system_memory_is_low()
{
  return seq_system_memory_is_below(seq_cache_memory_floor_bytes());
}

size_t caches_calc_memory_size_unique(const Scene *scene)
{
  Set<const ImBuf *> seen;
  source_image_cache_collect_images(scene, seen);
  final_image_cache_collect_images(scene, seen);
  size_t size = 0;
  for (const ImBuf *ibuf : seen) {
    size += IMB_get_size_in_memory(ibuf);
  }
  return size;
}

/* ★素材と仕上がりが同じ絵を共有している分を二重に数えない(既定)。
 * `FALCON_VSE_CACHE_DEDUP=0` で今までどおりの足し算に戻る。 */
static bool cache_dedup_enabled()
{
  static const bool enabled = []() {
    const char *env = std::getenv("FALCON_VSE_CACHE_DEDUP");
    return env == nullptr || env[0] != '0';
  }();
  return enabled;
}

static size_t caches_calc_memory_size(const Scene *scene)
{
  if (cache_dedup_enabled()) {
    return caches_calc_memory_size_unique(scene);
  }
  return source_image_cache_calc_memory_size(scene) +
         final_image_cache_calc_memory_size(scene);
}

bool is_cache_full(const Scene *scene)
{
  const size_t cache_limit = size_t(U.memcachelimit) * 1024 * 1024;
  const size_t used = caches_calc_memory_size(scene);
  if (used > cache_limit) {
    return true;
  }
  /* 空きが下限を割っている間は、設定した上限に達していなくても手放す。
   * ただし丸ごと空にはしない: ここまでは残す、という下駄を履かせて追い出しのループを止める
   * (全部捨てると、圧力が一瞬かすめただけで再生が最初からやり直しになる)。 */
  const size_t keep = std::max<size_t>(cache_limit / 8, 64 * 1024 * 1024);
  if (used > keep && seq_system_memory_is_low()) {
    return true;
  }
  return false;
}

bool cache_should_stop_growing(const Scene * /*scene*/)
{
  /* ★崖の手前の1段。「捨てる」でなく「これ以上入れない」。
   *
   * `is_cache_full()` は上限(`U.memcachelimit`)か空き 1GB のどちらかに当たるまで
   * 一切効かない。32GB の機械に 16384MB × 3 系統が通っている今の設定では、
   * 先に当たるのは空きの側で、当たった瞬間に追い出しが走って再生が 3 倍遅くなる
   * (2026-09-10 実測: 461fps -> 156fps)。
   *
   * ここで太るのを止めておくと、既に入っている分はそのまま効くので、
   * 温まっている再生は**速度を保ったまま頭打ちになる**。 */
  return seq_system_memory_is_below(seq_cache_soft_floor_bytes());
}


/**
 * 追い出しの時、素材の絵(source)から先に捨てて仕上がりの絵(final)を残すか。
 * `FALCON_VSE_CACHE_KEEP_FINAL=0` で上流どおり(final を 1 枚捨ててから比率ぶんの source)。
 *
 * ★なぜ要るか(2026-09-20 実測・メモリの少ない機械向け): 1 コマにつきキャッシュに載るのは
 * 「素材の絵 × 重なっている本数」+「仕上がりの絵 1 枚」で、FHD 4 本なら約 40MB/コマ。
 * このうち**再生でそのまま出せるのは仕上がりの 8MB だけ**。上流は 1 巡ごとに final を必ず
 * 1 枚捨てるので、予算が小さいほど「いちばん効く物」から先に消える。source から先に捨てれば、
 * 同じ予算で持てる「すぐ出せるコマ」が重ねた本数ぶん(最大 5 倍)増える。
 *
 * ⚠ 素材を捨てると、色補正などを変えた後の描き直しは復号からやり直しになる。そこで
 * **先読みジョブが走っている間(= 再生中)だけ**この順番にし、編集中は上流のままにする。
 */
static bool seq_cache_keep_final_enabled()
{
  static const bool keep = []() {
    const char *env = getenv("FALCON_VSE_CACHE_KEEP_FINAL");
    return (env == nullptr) ? true : atoi(env) != 0;
  }();
  return keep;
}

bool evict_caches_if_full(Scene *scene)
{
  if (!is_cache_full(scene)) {
    /* Cache is not full, we don't have to evict anything. */
    return false;
  }

  /* Cache is full, so we want to remove some images. We always try to remove one final image,
   * and some amount of source images for each final image, so that ratio of cached images
   * stays the same. Depending on the frame composition complexity, there can be lots of
   * source images cached for a single final frame; if we only removed one source image
   * we'd eventually have the cache still filled only with source images. */
  bool evicted_final = false;
  bool evicted_source = false;
  /* 再生中(先読みジョブが走っている間)は、仕上がりの絵を残して素材の絵から捨てる。 */
  const bool keep_final = seq_cache_keep_final_enabled() && seq_prefetch_job_is_running(scene);
  do {
    const size_t count_final = final_image_cache_get_image_count(scene);
    const size_t count_source = source_image_cache_get_image_count(scene);
    evicted_final = false;
    evicted_source = false;
    const bool final_active = scene->ed->cache_flag & SEQ_CACHE_STORE_FINAL_OUT;

    if (keep_final) {
      /* 素材の絵が 1 枚でも捨てられる限り、仕上がりの絵には手を付けない。
       * 素材が尽きた時だけ、上流と同じく仕上がりを 1 枚捨てる。 */
      if (count_source != 0) {
        evicted_source = source_image_cache_evict(scene);
        for (size_t i = 1; evicted_source && i < count_source && is_cache_full(scene); i++) {
          if (!source_image_cache_evict(scene)) {
            break;
          }
        }
      }
      if (!evicted_source && count_final != 0) {
        evicted_final = final_image_cache_evict(scene);
      }
      continue;
    }

    /* Evict one final item, and as much from source as needed to maintain ratio. */
    if (count_final != 0) {
      evicted_final = final_image_cache_evict(scene);
    }
    /* Only remove source images if there's more of them than final ones. */
    if (count_source != 0 && (!final_active || count_source > count_final)) {
      evicted_source = source_image_cache_evict(scene);
      /* Only try to enforce the ratio when the final cache is active. */
      if (evicted_source && final_active) {
        const size_t items = divide_ceil_ul(count_source, std::max<size_t>(count_final, 1));
        /* Start at "1" to make sure we only try to evict more frames if the ratio is above 1:1. */
        for (size_t i = 1; i < items; i++) {
          if (!source_image_cache_evict(scene)) {
            /* Can't evict any more frames, stop. */
            break;
          }
        }
      }
    }

  } while (is_cache_full(scene) && (evicted_final || evicted_source));

  /* Did we evict anything to free up the cache? */
  return !(evicted_final || evicted_source);
}

static void update_range_with_effects(const Scene *scene, const Strip *strip, int2 &r_range)
{
  r_range.x = std::min(r_range.x, strip->left_handle());
  r_range.y = std::max(r_range.y, strip->right_handle(scene) - 1);
  Span<Strip *> effects = SEQ_lookup_effects_by_strip(scene->ed, strip);
  for (Strip *effect : effects) {
    update_range_with_effects(scene, effect, r_range);
  }
}

static void invalidate_final_cache_strip_range(Scene *scene, const Strip *strip)
{
  int2 range{MAXFRAME, -MAXFRAME};
  update_range_with_effects(scene, strip, range);
  final_image_cache_invalidate_frame_range(scene, range.x, range.y);
}

static void invalidate_raw_cache_of_parent_meta(Scene *scene, Strip *strip)
{
  Strip *meta = lookup_meta_by_strip(editing_get(scene), strip);
  if (meta == nullptr) {
    return;
  }

  relations_invalidate_cache_raw(scene, meta);
}

void relations_invalidate_cache_raw(Scene *scene, Strip *strip)
{
  source_image_cache_invalidate_strip(scene, strip);
  media_presence_invalidate_strip(scene, strip);
  relations_invalidate_cache(scene, strip);
}

void relations_invalidate_cache(Scene *scene, Strip *strip)
{
  if (strip->effectdata && strip->type == STRIP_TYPE_SPEED) {
    strip_effect_speed_rebuild_map(scene, strip);
  }

  /* Zero-input compositor effect source caches also need to be invalidated. */
  if (strip->type == STRIP_TYPE_COMPOSITOR && !strip->is_effect_with_inputs()) {
    source_image_cache_invalidate_strip(scene, strip);
  }

  invalidate_final_cache_strip_range(scene, strip);
  intra_frame_cache_invalidate(scene, strip);
  preview_cache_invalidate(scene);
  invalidate_raw_cache_of_parent_meta(scene, strip);

  /* Needed to update VSE sound. */
  DEG_id_tag_update(&scene->id, ID_RECALC_SEQUENCER_STRIPS);
  prefetch_stop(scene);
}

void relations_tag_temporary_animation_frame(Scene *scene)
{
  Editing *ed = editing_get(scene);
  if (ed != nullptr) {
    ed->runtime->temporary_animation_frame = BKE_scene_frame_get(scene);
  }
}

void relations_invalidate_temporary_animation_frame(Scene *scene)
{
  Editing *ed = editing_get(scene);
  if (ed == nullptr || !ed->runtime->temporary_animation_frame.has_value()) {
    return;
  }

  const float temp_frame = ed->runtime->temporary_animation_frame.value();
  if (temp_frame != BKE_scene_frame_get(scene)) {
    final_image_cache_invalidate_frame_range(scene, temp_frame, temp_frame);
    ed->runtime->temporary_animation_frame.reset();
  }
}

void relations_invalidate_scene_strips(const Main *bmain, const Scene *scene_target)
{
  for (Scene &scene : bmain->scenes) {
    if (scene.ed != nullptr) {
      for (Strip *strip : lookup_strips_by_scene(editing_get(&scene), scene_target)) {
        relations_invalidate_cache_raw(&scene, strip);
      }
    }
  }
}

void relations_update_view_layer_scene_strips(Main *bmain,
                                              Scene *scene,
                                              const char *old_name,
                                              const char *new_name)
{
  for (Scene &scene_iter : bmain->scenes) {
    Editing *ed = seq::editing_get(&scene_iter);
    if (ed == nullptr) {
      continue;
    }
    for (Strip *strip : seq::lookup_strips_by_scene(ed, scene)) {
      BLI_assert(strip->scene_view_layer_name != nullptr);
      if (!STREQ(strip->scene_view_layer_name, old_name)) {
        continue;
      }

      MEM_delete(strip->scene_view_layer_name);
      strip->scene_view_layer_name = new_name ?
                                         BLI_strdup(new_name) :
                                         BLI_strdup(
                                             BKE_view_layer_default_render(strip->scene)->name);
      if (new_name == nullptr) {
        /* View layer was deleted. */
        seq::relations_invalidate_cache_raw(&scene_iter, strip);
      }
    }
  }
}

void relations_invalidate_compositor_users(const Main *bmain, const bNodeTree *node_tree)
{
  for (Scene &scene : bmain->scenes) {
    if (scene.ed != nullptr) {
      for (Strip *strip : lookup_strips_by_compositor_node_group(editing_get(&scene), node_tree)) {
        relations_invalidate_cache(&scene, strip);
      }
    }
  }
}

static void invalidate_movieclip_strips(Scene *scene,
                                        MovieClip *clip_target,
                                        ListBaseT<Strip> *seqbase)
{
  for (Strip *strip = static_cast<Strip *>(seqbase->first); strip != nullptr; strip = strip->next)
  {
    if (strip->clip == clip_target) {
      relations_invalidate_cache_raw(scene, strip);
    }

    if (strip->seqbase.first != nullptr) {
      invalidate_movieclip_strips(scene, clip_target, &strip->seqbase);
    }
  }
}

void relations_invalidate_movieclip_strips(Main *bmain, MovieClip *clip_target)
{
  for (Scene *scene = static_cast<Scene *>(bmain->scenes.first); scene != nullptr;
       scene = static_cast<Scene *>(scene->id.next))
  {
    if (scene->ed != nullptr) {
      invalidate_movieclip_strips(scene, clip_target, &scene->ed->seqbase);
    }
  }
}

void relations_free_imbuf(Scene *scene,
                          ListBaseT<Strip> *seqbase,
                          bool for_render,
                          const Set<std::string> *only_movie_paths)
{
  if (scene->ed == nullptr) {
    return;
  }

  prefetch_stop(scene);

  for (Strip &strip : *seqbase) {
    if (for_render && strip.intersects_frame(scene, scene->r.cfra)) {
      continue;
    }

    if (strip.data) {
      if (strip.type == STRIP_TYPE_MOVIE) {
        if (only_movie_paths == nullptr ||
            only_movie_paths->contains(strip_movie_source_path_get(scene, &strip)))
        {
          strip_free_movie_readers(&strip);
        }
      }
      if (strip.type == STRIP_TYPE_SPEED) {
        strip_effect_speed_rebuild_map(scene, &strip);
      }
    }
    if (strip.type == STRIP_TYPE_META) {
      relations_free_imbuf(scene, &strip.seqbase, for_render, only_movie_paths);
    }
    if (strip.type == STRIP_TYPE_SCENE) {
      /* FIXME: recurse downwards,
       * but do recurse protection somehow! */
    }
  }
}

static void sequencer_all_free_anim_ibufs(const Scene *scene,
                                          ListBaseT<Strip> *seqbase,
                                          int timeline_frame,
                                          const int frame_range[2])
{
  Editing *ed = editing_get(scene);
  for (Strip *strip = static_cast<Strip *>(seqbase->first); strip != nullptr; strip = strip->next)
  {
    if (!strip->intersects_frame(scene, timeline_frame) ||
        !((frame_range[0] <= timeline_frame) && (frame_range[1] > timeline_frame)))
    {
      strip_free_movie_readers(strip);
    }
    if (strip->type == STRIP_TYPE_META) {
      int meta_range[2];

      MetaStack *ms = meta_stack_active_get(ed);
      if (ms != nullptr && ms->parent_strip == strip) {
        meta_range[0] = -MAXFRAME;
        meta_range[1] = MAXFRAME;
      }
      else {
        /* Limit frame range to meta strip. */
        meta_range[0] = max_ii(frame_range[0], strip->left_handle());
        meta_range[1] = min_ii(frame_range[1], strip->right_handle(scene));
      }

      sequencer_all_free_anim_ibufs(scene, &strip->seqbase, timeline_frame, meta_range);
    }
  }
}

void relations_free_all_anim_ibufs(Scene *scene, int timeline_frame)
{
  Editing *ed = editing_get(scene);
  if (ed == nullptr) {
    return;
  }

  const int frame_range[2] = {-MAXFRAME, MAXFRAME};
  sequencer_all_free_anim_ibufs(scene, &ed->seqbase, timeline_frame, frame_range);
}

static Strip *sequencer_check_scene_recursion(Scene *scene, ListBaseT<Strip> *seqbase)
{
  for (Strip &strip : *seqbase) {
    if (strip.type == STRIP_TYPE_SCENE && strip.scene == scene) {
      return &strip;
    }

    if (strip.type == STRIP_TYPE_SCENE && (strip.flag & SEQ_SCENE_STRIPS)) {
      if (strip.scene && strip.scene->ed &&
          sequencer_check_scene_recursion(scene, &strip.scene->ed->seqbase))
      {
        return &strip;
      }
    }

    if (strip.type == STRIP_TYPE_META && sequencer_check_scene_recursion(scene, &strip.seqbase)) {
      return &strip;
    }
  }

  return nullptr;
}

bool relations_check_scene_recursion(Scene *scene, ReportList *reports)
{
  Editing *ed = editing_get(scene);
  if (ed == nullptr) {
    return false;
  }

  Strip *recursive_seq = sequencer_check_scene_recursion(scene, &ed->seqbase);

  if (recursive_seq != nullptr) {
    BKE_reportf(reports,
                RPT_WARNING,
                "Recursion detected in video sequencer. Strip %s at frame %d will not be rendered",
                recursive_seq->name + 2,
                recursive_seq->left_handle());

    for (Strip &strip : ed->seqbase) {
      if (strip.type != STRIP_TYPE_SCENE && sequencer_strip_generates_image(&strip)) {
        /* There are other strips to render, so render them. */
        return false;
      }
    }
    /* No other strips to render - cancel operator. */
    return true;
  }

  return false;
}

bool relations_render_loop_check(Strip *strip_main, Strip *strip)
{
  if (strip_main == nullptr || strip == nullptr) {
    return false;
  }

  if (strip_main == strip) {
    return true;
  }

  if ((strip_main->input1 && relations_render_loop_check(strip_main->input1, strip)) ||
      (strip_main->input2 && relations_render_loop_check(strip_main->input2, strip)))
  {
    return true;
  }

  for (StripModifierData &smd : strip_main->modifiers) {
    if (smd.mask_strip && relations_render_loop_check(smd.mask_strip, strip)) {
      return true;
    }
  }

  return false;
}

void strip_free_movie_readers(Strip *strip)
{
  for (MovieReader *anim : strip->runtime->movie_readers) {
    MOV_close(anim);
  }
  strip->runtime->movie_readers.clear();
}

void relations_session_uid_generate(Strip *strip)
{
  strip->runtime->session_uid = BLI_session_uid_generate();
}

static bool get_uids_cb(Strip *strip, void *user_data)
{
  Set<SessionUID> &used_uids = *static_cast<Set<SessionUID> *>(user_data);
  const SessionUID &session_uid = strip->runtime->session_uid;
  if (!BLI_session_uid_is_generated(&session_uid)) {
    printf("Sequence %s does not have UID generated.\n", strip->name);
    return true;
  }

  if (used_uids.contains(session_uid)) {
    printf("Sequence %s has duplicate UID generated.\n", strip->name);
    return true;
  }
  used_uids.add(session_uid);
  return true;
}

void relations_check_uids_unique_and_report(const Scene *scene)
{
  if (scene->ed == nullptr) {
    return;
  }

  Set<SessionUID> used_uids;
  foreach_strip(&scene->ed->seqbase, get_uids_cb, &used_uids);
}

bool exists_in_seqbase(const Strip *strip, const ListBaseT<Strip> *seqbase)
{
  for (Strip &strip_test : *seqbase) {
    if (strip_test.type == STRIP_TYPE_META && exists_in_seqbase(strip, &strip_test.seqbase)) {
      return true;
    }
    if (&strip_test == strip) {
      return true;
    }
  }
  return false;
}

}  // namespace blender::seq
