/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spseq
 */

#include <algorithm>
#include <cstdlib>

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BLI_listbase.h"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"

#include "SEQ_relations.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_time.hh"
#include "SEQ_transform.hh"

#include "sequencer_intern.hh"

namespace blender::ed::vse {

static wmOperatorStatus sequencer_rename_channel_invoke(bContext *C,
                                                        wmOperator * /*op*/,
                                                        const wmEvent *event)
{
  SeqChannelDrawContext context;
  SpaceSeq *sseq = CTX_wm_space_seq(C);
  channel_draw_context_init(C, CTX_wm_region(C), &context);
  float mouse_y = ui::view2d_region_to_view_y(context.timeline_region_v2d, event->mval[1]);

  sseq->runtime->rename_channel_index = seq::y_to_channel(mouse_y);
  WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, CTX_data_sequencer_scene(C));
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_rename_channel(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Rename Channel";
  ot->idname = "SEQUENCER_OT_rename_channel";

  /* API callbacks. */
  ot->invoke = sequencer_rename_channel_invoke;
  ot->poll = sequencer_edit_with_channel_region_poll;

  /* Flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/* -------------------------------------------------------------------- */
/** \name Falcon: number of channels the timeline shows (2026-09-20)
 *
 * The count is kept on the scene (#seq::falcon_timeline_channels, default 10). "+" shows one
 * channel more, "-" one fewer; a channel that holds strips is always shown.
 * \{ */

static bool sequencer_channel_count_poll(bContext *C)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  return scene != nullptr && seq::falcon_timeline_channels(scene) > 0 &&
         !ID_IS_LINKED(&scene->id);
}

/** Move the timelines that show the top channel when a channel is **removed**, so the view does
 * not hold on to a row that no longer exists.
 *
 * ★2026-09-21 作者「チャンネルを増やすとそっちに画面が映るから移動しないようにしたい」。
 * 足した時は**動かさない** — 編集中に段を足すと見ている場所が飛ぶのが理由。足した段は
 * スクロールすれば出てくる。減らした時だけは、消えた段に貼り付いたままになるのでついていく。
 * 戻す口: `FALCON_VSE_CHANNEL_FOLLOW=1` で足した時も本家と同じについていく形に戻る。 */
static bool channel_add_follows_view()
{
  static const bool follow = []() {
    const char *env = std::getenv("FALCON_VSE_CHANNEL_FOLLOW");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return follow;
}

static void sequencer_channel_count_follow_views(bContext *C,
                                                 const Scene *scene,
                                                 const int old_shown,
                                                 const int new_shown)
{
  const float delta = float(new_shown - old_shown);
  /* ★上下反転(#seq::channel_flip_enabled)では段が**下**へ伸びるので、ついていく向きも鏡写しになる。
   * 反転していない時は段が上へ伸び、view も上(+)へ動く(本家と同じ)。 */
  const bool flip = seq::channel_flip_enabled();
  const float view_delta = flip ? -delta : delta;
  wmWindowManager *wm = CTX_wm_manager(C);
  for (wmWindow &win : wm->windows) {
    bScreen *screen = WM_window_get_active_screen(&win);
    const WorkSpace *workspace = WM_window_get_active_workspace(&win);
    if (screen == nullptr || workspace == nullptr || workspace->sequencer_scene != scene) {
      continue;
    }
    for (ScrArea &area : screen->areabase) {
      if (area.spacetype != SPACE_SEQ) {
        continue;
      }
      SpaceSeq *sseq = static_cast<SpaceSeq *>(area.spacedata.first);
      ARegion *region = BKE_area_find_region_type(&area, RGN_TYPE_WINDOW);
      if (sseq == nullptr || region == nullptr || sseq->view == SEQ_VIEW_PREVIEW) {
        continue;
      }
      View2D *v2d = &region->v2d;
      /* Only views that reach the far edge of the top channel — 反転では「いちばん大きい番号」が
       * 画面の**下**に来るので、見る辺も ymax から ymin へ入れ替わる。 */
      const bool at_top_channel = flip ? (v2d->cur.ymin <= seq::channel_to_y(old_shown)) :
                                         (v2d->cur.ymax >= seq::channel_to_y(old_shown) + 1.0f);
      const bool follow = (delta < 0.0f) || (delta > 0.0f && channel_add_follows_view());
      if (follow && at_top_channel) {
        v2d->cur.ymin += view_delta;
        v2d->cur.ymax += view_delta;
        if (flip) {
          /* 反転の行き止まりはチャンネル 1 の上端(`channel_to_y(1) + 1`)。 */
          const float top_limit = seq::channel_to_y(1) + 1.0f;
          if (v2d->cur.ymax > top_limit) {
            v2d->cur.ymin -= v2d->cur.ymax - top_limit;
            v2d->cur.ymax = top_limit;
          }
        }
        else if (v2d->cur.ymin < 0.0f) {
          v2d->cur.ymax -= v2d->cur.ymin;
          v2d->cur.ymin = 0.0f;
        }
        /* The clamp of the timeline (`sequencer_main_clamp_view`) settles the view on the next
         * layout; keep it from holding on to the old top. */
        sseq->runtime->timeline_clamp_custom_range = v2d->cur.ymax;
      }
      ED_area_tag_redraw(&area);
    }
  }
}

static wmOperatorStatus sequencer_channel_count_change(bContext *C, const int step)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  Editing *ed = seq::editing_get(scene);
  const ListBaseT<Strip> *seqbase = ed ? seq::active_seqbase_get(ed) : nullptr;
  const int old_shown = seq::falcon_timeline_channels_shown(scene, seqbase);
  if (old_shown == 0) {
    return OPERATOR_CANCELLED;
  }
  /* Start from what is shown: with strips above the count, "+" adds above them. */
  const int count = std::clamp(old_shown + step, 1, seq::MAX_CHANNELS);
  seq::falcon_timeline_channels_set(scene, count);
  const int new_shown = seq::falcon_timeline_channels_shown(scene, seqbase);

  sequencer_channel_count_follow_views(C, scene, old_shown, new_shown);
  WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, scene);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus sequencer_channel_add_exec(bContext *C, wmOperator * /*op*/)
{
  return sequencer_channel_count_change(C, 1);
}

static wmOperatorStatus sequencer_channel_remove_exec(bContext *C, wmOperator * /*op*/)
{
  return sequencer_channel_count_change(C, -1);
}

void SEQUENCER_OT_channel_add(wmOperatorType *ot)
{
  ot->name = "Add Channel";
  ot->idname = "SEQUENCER_OT_channel_add";
  ot->description = "Show one more channel in the timeline";

  ot->exec = sequencer_channel_add_exec;
  ot->poll = sequencer_channel_count_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ★2026-09-21 作者「空いてる余白に…上下反転のボタンが欲しい」。
 * 値は場面に保存されるので、切り替えてから保存すれば次に開いた時も同じ向き。 */
static wmOperatorStatus sequencer_channel_flip_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  if (scene == nullptr) {
    return OPERATOR_CANCELLED;
  }
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "enable");
  const bool enable = RNA_property_is_set(op->ptr, prop) ?
                          RNA_property_boolean_get(op->ptr, prop) :
                          !seq::channel_flip_enabled();
  seq::channel_flip_store(scene, enable);
  WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, scene);
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_channel_flip(wmOperatorType *ot)
{
  ot->name = "Flip Channel Order";
  ot->idname = "SEQUENCER_OT_channel_flip";
  ot->description =
      "Put channel 1 at the top and count downward, or back to channel 1 at the bottom";

  ot->exec = sequencer_channel_flip_exec;
  ot->poll = sequencer_channel_count_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "enable",
                  true,
                  "Channel 1 on Top",
                  "Leave unset to toggle whichever way the timeline is showing now");
}

void SEQUENCER_OT_channel_remove(wmOperatorType *ot)
{
  ot->name = "Remove Channel";
  ot->idname = "SEQUENCER_OT_channel_remove";
  ot->description = "Show one channel fewer in the timeline (channels with strips stay shown)";

  ot->exec = sequencer_channel_remove_exec;
  ot->poll = sequencer_channel_count_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Falcon: swap two channels' content (2026-09-22)
 *
 * 本人「ここは前後の入れ替えを簡単にSwitchできるようにしたい」。チャンネル番号は
 * そのまま前後関係(重なりの手前・奥)を決めているので、丸ごと入れ替えるには両方の
 * チャンネルに乗っている Strip の `channel` を交換するだけでよい。時間方向へは
 * 何も動かさない。
 * \{ */

static bool sequencer_channel_move_poll(bContext *C)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  return scene != nullptr && seq::editing_get(scene) != nullptr && !ID_IS_LINKED(&scene->id);
}

static wmOperatorStatus sequencer_channel_move_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  Editing *ed = seq::editing_get(scene);
  ListBaseT<Strip> *seqbase = seq::active_seqbase_get(ed);

  const int channel = RNA_int_get(op->ptr, "channel");
  const int other = channel + RNA_int_get(op->ptr, "direction");
  if (other < 1 || other > seq::MAX_CHANNELS) {
    return OPERATOR_CANCELLED;
  }

  bool changed = false;
  for (Strip &strip : *seqbase) {
    if (strip.channel == channel) {
      strip.channel = other;
      changed = true;
    }
    else if (strip.channel == other) {
      strip.channel = channel;
      changed = true;
    }
    else {
      continue;
    }
    seq::relations_invalidate_cache(scene, &strip);
  }
  if (!changed) {
    return OPERATOR_CANCELLED;
  }

  WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, scene);
  return OPERATOR_FINISHED;
}

void SEQUENCER_OT_channel_move(wmOperatorType *ot)
{
  ot->name = "Swap Channel";
  ot->idname = "SEQUENCER_OT_channel_move";
  ot->description = "Swap this channel's strips with the neighboring channel's";

  ot->exec = sequencer_channel_move_exec;
  ot->poll = sequencer_channel_move_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna, "channel", 1, 1, seq::MAX_CHANNELS, "Channel", "", 1, seq::MAX_CHANNELS);
  RNA_def_int(ot->srna, "direction", 1, -1, 1, "Direction", "-1 or 1", -1, 1);
}

/** \} */

}  // namespace blender::ed::vse
