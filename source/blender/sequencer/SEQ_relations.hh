/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include <cstddef>

#include "DNA_listBase.h"

#include <string>

#include "BLI_enum_flags.hh"
#include "BLI_set.hh"

namespace blender {

struct Main;
struct MovieClip;
struct ReportList;
struct bNodeTree;
struct Scene;
struct ImBuf;
struct Strip;

namespace seq {

/**
 * Check if one strip is input to the other.
 */
bool relation_is_effect_of_strip(const Strip *effect, const Strip *input);
/**
 * Free currently open movie strip readers.
 */
void strip_free_movie_readers(Strip *strip);
bool relations_check_scene_recursion(Scene *scene, ReportList *reports);
/**
 * Check if "strip_main" (indirectly) uses strip "strip".
 */
bool relations_render_loop_check(Strip *strip_main, Strip *strip);
/**
 * Close movie readers (and rebuild speed maps) of the strips in `seqbase`.
 *
 * `only_movie_paths` (Falcon): when not null, only the movie strips whose source file is in the
 * set have their readers closed. Re-opening a movie strip means "open the file, seek to the
 * previous key frame, decode forward to the wanted frame", which happens synchronously inside the
 * preview draw, so dropping a reader that nothing invalidated costs a visible stall on the next
 * redraw (measured 0.80 s for a 3440x1440 HEVC with a 250 frame key frame interval).
 *
 * The filter is keyed on the *file* rather than on the strip because one file is normally shared
 * by several strips (cutting a clip in two leaves two strips on one file), and all of them have to
 * re-open once a proxy for that file appears on disk. Speed map rebuilds and the meta recursion
 * are unaffected by the filter.
 */
void relations_free_imbuf(Scene *scene,
                          ListBaseT<Strip> *seqbase,
                          bool for_render,
                          const Set<std::string> *only_movie_paths = nullptr);

/**
 * Invalidates various caches related to a given strip:
 * - Final cached frames over the length of the strip,
 * - Intra-frame caches of the current frame,
 * - Source/raw caches of the meta strip that contains this strip, if any,
 * - Media presence cache of the strip,
 * - Rebuilds speed index map if this is a speed effect strip,
 * - Tags DEG for strip recalculation,
 * - Stops prefetching job, if any.
 */
void relations_invalidate_cache(Scene *scene, Strip *strip);

/**
 * Does everything #relations_invalidate_cache does, plus invalidates cached raw source
 * images of the strip.
 */
void relations_invalidate_cache_raw(Scene *scene, Strip *strip);

/** Mark the current frame as potentially cached with a temporary animated property value. */
void relations_tag_temporary_animation_frame(Scene *scene);

/** Invalidate the marked frame after animation evaluation discards the temporary value. */
void relations_invalidate_temporary_animation_frame(Scene *scene);

void relations_invalidate_scene_strips(const Main *bmain, const Scene *scene_target);

/**
 * Sync sequencer scene strips that reference a view layer by name.
 * This updates `strip->scene_view_layer_name` from \a old_name to \a new_name for any matching
 * strip across all sequencer scenes that use \a scene as input.
 *
 * NOTE: If a view layer is deleted, `new_name = nullptr` should be passed to clear the strips'
 * `scene_view_layer_name` to the default view layer. In this case, the function will also clear
 * the caches of these matching strips to evict stale frames.
 */
void relations_update_view_layer_scene_strips(Main *bmain,
                                              Scene *scene,
                                              const char *old_name,
                                              const char *new_name);

/**
 * Invalidates the cache for all strips that uses the given compositor node tree.
 */
void relations_invalidate_compositor_users(const Main *bmain, const bNodeTree *node_tree);

void relations_invalidate_movieclip_strips(Main *bmain, MovieClip *clip_target);
/**
 * Release FFmpeg handles of strips that are not currently displayed to minimize memory usage.
 */
void relations_free_all_anim_ibufs(Scene *scene, int timeline_frame);
/**
 * A debug and development function which checks whether strips have unique UIDs.
 * Errors will be reported to the console.
 */
void relations_check_uids_unique_and_report(const Scene *scene);
/**
 * Generate new UID for the given strip.
 */
void relations_session_uid_generate(Strip *strip);

enum class CacheCleanup {
  FinalImage = (1 << 0),
  SourceImage = (1 << 1),
  Thumbnails = (1 << 2),
  IntraFrame = (1 << 3),

  /* All cache types. */
  All = FinalImage | SourceImage | Thumbnails | IntraFrame,

  /* Typical "what gets rendered" cache types: final frame
   * cache, plus various intra-frame cached things. */
  FinalAndIntra = FinalImage | IntraFrame,
};
ENUM_OPERATORS(CacheCleanup);

void cache_cleanup(Scene *scene, CacheCleanup mode);

void cache_settings_changed(Scene *scene);
bool is_cache_full(const Scene *scene);
/**
 * 空きメモリが「柔らかい下限」(`FALCON_VSE_MEM_SOFT_MB`・既定 4096) を割っているか。
 * 真の間は**新しくキャッシュへ入れない**(既に入っている物は捨てない)。
 * `is_cache_full()` の崖が来る手前で太るのを止めるための1段。
 */
bool cache_should_stop_growing(const Scene *scene);
bool evict_caches_if_full(Scene *scene);

void source_image_cache_iterate(Scene *scene,
                                void *userdata,
                                void callback_iter(void *userdata,
                                                   const Strip *strip,
                                                   int timeline_frame));
void final_image_cache_iterate(Scene *scene,
                               void *userdata,
                               void callback_iter(void *userdata, int timeline_frame));

size_t source_image_cache_calc_memory_size(const Scene *scene);
size_t final_image_cache_calc_memory_size(const Scene *scene);

/**
 * ★素材と仕上がりのキャッシュは**同じ `ImBuf` を共有することがある**(1 本のストリップで
 * 前処理が要らない時、読んだ絵がそのまま仕上がりになる)。上の 2 本を足すとその絵を 2 回数えるので、
 * `is_cache_full()` の `used` が実体の約 2 倍になり、**設定した上限の半分で「満杯」**になる。
 * 実測(2026-09-21・FHD 1 本): `raw` と `final` が 2048 / 2048 と完全に一致し、RSS はその合計より小さい。
 *
 * こちらは実体(ポインタ)ごとに 1 回だけ数える。
 * 戻す口: `FALCON_VSE_CACHE_DEDUP=0` で今までどおりの足し算に戻る。
 *
 * \note これは本家 Blender から続いている数え方で(9e4c26574a6・Aras Pranckevicius)、
 * 2026-09-21 時点の upstream main にも同じ式が残っている。
 */
size_t caches_calc_memory_size_unique(const Scene *scene);
void source_image_cache_collect_images(const Scene *scene, Set<const ImBuf *> &r_images);
void final_image_cache_collect_images(const Scene *scene, Set<const ImBuf *> &r_images);

bool exists_in_seqbase(const Strip *strip, const ListBaseT<Strip> *seqbase);

}  // namespace seq
}  // namespace blender
