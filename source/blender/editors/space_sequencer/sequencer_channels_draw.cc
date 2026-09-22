/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spseq
 */

#include "MEM_guardedalloc.h"

#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "BLI_math_base.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "ED_screen.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "SEQ_channels.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_time.hh"
#include "SEQ_transform.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"

#include "sequencer_intern.hh"

namespace blender::ed::vse {

static float draw_offset_get(const View2D *timeline_region_v2d)
{
  return timeline_region_v2d->cur.ymin;
}

static float channel_height_pixelspace_get(const View2D *timeline_region_v2d)
{
  return ui::view2d_view_to_region_y(timeline_region_v2d, 1.0f) -
         ui::view2d_view_to_region_y(timeline_region_v2d, 0.0f);
}

static float frame_width_pixelspace_get(const View2D *timeline_region_v2d)
{

  return ui::view2d_view_to_region_x(timeline_region_v2d, 1.0f) -
         ui::view2d_view_to_region_x(timeline_region_v2d, 0.0f);
}

static float icon_width_get(const SeqChannelDrawContext *context)
{
  return (U.widget_unit * 0.8 * context->scale);
}

static float widget_y_offset(const SeqChannelDrawContext *context)
{
  return ((context->channel_height / context->scale) - icon_width_get(context)) / 2;
}

static float channel_index_y_min(const SeqChannelDrawContext *context, const int index)
{
  float y = (seq::channel_to_y(index) - context->draw_offset) * context->channel_height;
  y /= context->scale;
  return y;
}

static void displayed_channel_range_get(const SeqChannelDrawContext *context,
                                        int r_channel_range[2])
{
  const View2D *v2d = context->timeline_region_v2d;
  if (seq::channel_flip_enabled()) {
    /* With the flip, `cur.ymax` (the top of the view) is where the *smallest* channel numbers
     * sit and `cur.ymin` (the bottom) is where the *largest* ones sit -- the opposite of the
     * non-flipped case below. Mirror both which edge feeds which bound and the `-1`/`ceil`
     * one-row margin (originally applied at the `ymax`/large-channel edge) so it still applies
     * at the edge that now shows the smallest channel numbers. */
    r_channel_range[0] = max_ii(1, seq::y_to_channel(v2d->cur.ymax) - 1);
    r_channel_range[1] = seq::y_to_channel(v2d->cur.ymin);
  }
  else {
    /* Channel 0 is not usable, so should never be drawn. */
    r_channel_range[0] = max_ii(1, seq::y_to_channel(v2d->cur.ymin));
    r_channel_range[1] = ceil(v2d->cur.ymax);
  }

  rctf strip_boundbox;
  BLI_rctf_init(&strip_boundbox, 0.0f, 0.0f, 1.0f, r_channel_range[1]);
  seq::timeline_expand_boundbox(context->scene, context->seqbase, &strip_boundbox);
  CLAMP(r_channel_range[0], strip_boundbox.ymin, strip_boundbox.ymax);
  CLAMP(r_channel_range[1], strip_boundbox.ymin, seq::MAX_CHANNELS);

  /* Falcon: no headers above the channels the timeline has. */
  const int falcon_shown = seq::falcon_timeline_channels_shown(context->scene, context->seqbase);
  if (falcon_shown) {
    r_channel_range[1] = min_ii(r_channel_range[1], falcon_shown);
  }
}

static std::string draw_channel_widget_tooltip(bContext * /*C*/,
                                               void *argN,
                                               const StringRef /*tip*/)
{
  char *dyn_tooltip = static_cast<char *>(argN);
  return dyn_tooltip;
}

static float draw_channel_widget_mute(const SeqChannelDrawContext *context,
                                      ui::Block *block,
                                      const int channel_index,
                                      const float offset)
{
  float y = channel_index_y_min(context, channel_index) + widget_y_offset(context);

  const float width = icon_width_get(context);
  SeqTimelineChannel *channel = seq::channel_get_by_index(context->channels, channel_index);
  const int icon = channel->is_muted() ? ICON_CHECKBOX_DEHLT : ICON_CHECKBOX_HLT;

  PointerRNA ptr = RNA_pointer_create_discrete(
      &context->scene->id, RNA_SequenceTimelineChannel, channel);
  PropertyRNA *hide_prop = RNA_struct_type_find_property(RNA_SequenceTimelineChannel, "mute");

  block_emboss_set(block, ui::EmbossType::None);
  ui::Button *but = uiDefIconButR_prop(block,
                                       ui::ButtonType::Toggle,
                                       icon,
                                       context->v2d->cur.xmax / context->scale - offset,
                                       y,
                                       width,
                                       width,
                                       &ptr,
                                       hide_prop,
                                       0,
                                       0,
                                       0,
                                       std::nullopt);

  char *tooltip = BLI_sprintfN(
      "%s channel %d", channel->is_muted() ? "Unmute" : "Mute", channel_index);
  button_func_tooltip_set(but, draw_channel_widget_tooltip, tooltip, MEM_delete_void);

  return width;
}

static float draw_channel_widget_lock(const SeqChannelDrawContext *context,
                                      ui::Block *block,
                                      const int channel_index,
                                      const float offset)
{

  float y = channel_index_y_min(context, channel_index) + widget_y_offset(context);
  const float width = icon_width_get(context);

  SeqTimelineChannel *channel = seq::channel_get_by_index(context->channels, channel_index);
  const int icon = channel->is_locked() ? ICON_LOCKED : ICON_UNLOCKED;

  PointerRNA ptr = RNA_pointer_create_discrete(
      &context->scene->id, RNA_SequenceTimelineChannel, channel);
  PropertyRNA *hide_prop = RNA_struct_type_find_property(RNA_SequenceTimelineChannel, "lock");

  block_emboss_set(block, ui::EmbossType::None);
  ui::Button *but = uiDefIconButR_prop(block,
                                       ui::ButtonType::Toggle,
                                       icon,
                                       context->v2d->cur.xmax / context->scale - offset,
                                       y,
                                       width,
                                       width,
                                       &ptr,
                                       hide_prop,
                                       0,
                                       0,
                                       0,
                                       "");

  char *tooltip = BLI_sprintfN(
      "%s channel %d", channel->is_locked() ? "Unlock" : "Lock", channel_index);
  button_func_tooltip_set(but, draw_channel_widget_tooltip, tooltip, MEM_delete_void);

  return width;
}

/* Falcon: 「前後の入れ替えを簡単にSwitchできるようにしたい」。矢印は**画面の上下**を指す
 * (反転(#seq::channel_flip_enabled)の間は上下でチャンネル番号の増減が逆になるので、
 * ここで向きを畳んでおく)。 */
static float draw_channel_widget_move(const SeqChannelDrawContext *context,
                                      ui::Block *block,
                                      const int channel_index,
                                      const float offset,
                                      const bool visually_up)
{
  float y = channel_index_y_min(context, channel_index) + widget_y_offset(context);
  const float width = icon_width_get(context);

  const bool flip = seq::channel_flip_enabled();
  const int direction = (visually_up != flip) ? 1 : -1;
  const int other = channel_index + direction;
  const int icon = visually_up ? ICON_TRIA_UP : ICON_TRIA_DOWN;

  block_emboss_set(block, ui::EmbossType::None);
  ui::Button *but = uiDefIconButO(block,
                                  ui::ButtonType::But,
                                  "SEQUENCER_OT_channel_move",
                                  wm::OpCallContext::ExecDefault,
                                  icon,
                                  context->v2d->cur.xmax / context->scale - offset,
                                  y,
                                  width,
                                  width,
                                  std::nullopt);
  if (other < 1 || other > seq::MAX_CHANNELS) {
    button_flag_enable(but, ui::BUT_DISABLED);
  }
  else {
    PointerRNA *opptr = button_operator_ptr_ensure(but);
    RNA_int_set(opptr, "channel", channel_index);
    RNA_int_set(opptr, "direction", direction);
  }

  char *tooltip = BLI_sprintfN("Swap with channel %d", other);
  button_func_tooltip_set(but, draw_channel_widget_tooltip, tooltip, MEM_delete_void);

  return width;
}

static bool channel_is_being_renamed(const SpaceSeq *sseq, const int channel_index)
{
  return sseq->runtime->rename_channel_index == channel_index;
}

static float text_size_get(const SeqChannelDrawContext *context)
{
  const uiStyle *style = ui::style_get_dpi();
  return ui::fontstyle_height_max(&style->widget) * 1.5f * context->scale;
}

/* TODO: decide what gets priority - label or buttons. */
static rctf label_rect_init(const SeqChannelDrawContext *context,
                            const int channel_index,
                            const float used_width)
{
  float text_size = text_size_get(context);
  float margin = (context->channel_height / context->scale - text_size) / 2.0f;
  float y = channel_index_y_min(context, channel_index) + margin;

  float margin_x = icon_width_get(context) * 0.65;
  float width = max_ff(0.0f, context->v2d->cur.xmax / context->scale - used_width);

  /* Text input has its own margin. Prevent text jumping around and use as much space as possible.
   */
  if (channel_is_being_renamed(CTX_wm_space_seq(context->C), channel_index)) {
    float input_box_margin = icon_width_get(context) * 0.5f;
    margin_x -= input_box_margin;
    width += input_box_margin;
  }

  rctf rect;
  BLI_rctf_init(&rect, margin_x, margin_x + width, y, y + text_size);
  return rect;
}

static void draw_channel_labels(const SeqChannelDrawContext *context,
                                ui::Block *block,
                                const int channel_index,
                                const float used_width)
{
  SpaceSeq *sseq = CTX_wm_space_seq(context->C);
  rctf rect = label_rect_init(context, channel_index, used_width);

  if (BLI_rctf_size_y(&rect) <= 1.0f || BLI_rctf_size_x(&rect) <= 1.0f) {
    return;
  }

  SeqTimelineChannel *channel = seq::channel_get_by_index(context->channels, channel_index);
  if (channel_is_being_renamed(sseq, channel_index)) {
    PointerRNA ptr = RNA_pointer_create_discrete(
        &context->scene->id, RNA_SequenceTimelineChannel, channel);
    PropertyRNA *prop = RNA_struct_name_property(ptr.type);

    block_emboss_set(block, ui::EmbossType::Emboss);
    ui::Button *but = uiDefButR(block,
                                ui::ButtonType::Text,
                                "",
                                rect.xmin,
                                rect.ymin,
                                BLI_rctf_size_x(&rect),
                                BLI_rctf_size_y(&rect),
                                &ptr,
                                RNA_property_identifier(prop),
                                -1,
                                0,
                                0,
                                std::nullopt);
    block_emboss_set(block, ui::EmbossType::None);

    if (button_active_only(context->C, context->region, block, but) == false) {
      sseq->runtime->rename_channel_index = 0;
    }

    WM_event_add_notifier(context->C, NC_SCENE | ND_SEQUENCER, context->scene);
  }
  else {
    const char *label = channel->name;
    uiDefBut(block,
             ui::ButtonType::Label,
             label,
             rect.xmin,
             rect.ymin,
             rect.xmax - rect.xmin,
             (rect.ymax - rect.ymin),
             nullptr,
             0,
             0,
             std::nullopt);
  }
}

static void draw_channel_headers(const SeqChannelDrawContext *context)
{
  GPU_matrix_push();
  wmOrtho2_pixelspace(context->region->winx / context->scale,
                      context->region->winy / context->scale);
  ui::Block *block = block_begin(context->C, context->region, __func__, ui::EmbossType::Emboss);

  int channel_range[2];
  displayed_channel_range_get(context, channel_range);

  const float icon_width = icon_width_get(context);
  const float offset_lock = icon_width * 1.5f;
  const float offset_mute = icon_width * 2.5f;
  const float offset_move_down = icon_width * 3.5f;
  const float offset_move_up = icon_width * 4.5f;
  const float offset_width = icon_width * 5.5f;
  /* Draw widgets separately from text labels so they are batched together,
   * instead of alternating between two fonts (regular and SVG/icons). */
  for (int channel = channel_range[0]; channel <= channel_range[1]; channel++) {
    draw_channel_widget_lock(context, block, channel, offset_lock);
    draw_channel_widget_mute(context, block, channel, offset_mute);
    draw_channel_widget_move(context, block, channel, offset_move_up, true);
    draw_channel_widget_move(context, block, channel, offset_move_down, false);
  }
  for (int channel = channel_range[0]; channel <= channel_range[1]; channel++) {
    draw_channel_labels(context, block, channel, offset_width);
  }

  block_end(context->C, block);
  block_draw(context->C, block);

  GPU_matrix_pop();
}

static void draw_background()
{
  ui::theme::frame_buffer_clear(TH_BACK);
}

/**
 * Falcon (2026-09-20): "-" / "+" for the number of channels the timeline shows, in a band at the
 * top of the channel region, level with the time scrubbing area of the timeline (which covers the
 * same rows on the right). Only drawn while the channel count is on (`FALCON_VSE_CHANNELS`).
 */
static void draw_channel_count_buttons(const SeqChannelDrawContext *context)
{
  const int shown = seq::falcon_timeline_channels_shown(context->scene, context->seqbase);
  if (shown == 0) {
    return;
  }
  const float band = UI_TIME_SCRUB_MARGIN_Y;
  const float winx = context->region->winx;
  const float winy = context->region->winy;
  if (winy < band * 2.0f || winx < UI_UNIT_X * 3.0f) {
    return;
  }

  GPU_matrix_push();
  wmOrtho2_region_pixelspace(context->region);

  /* Band background, so the rows scrolled below it do not show through. */
  {
    const uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformThemeColor(TH_BACK);
    immRectf(pos, 0.0f, winy - band, winx, winy);
    GPU_blend(GPU_BLEND_ALPHA);
    immUniformThemeColor(TH_TIME_SCRUB_BACKGROUND);
    immRectf(pos, 0.0f, winy - band, winx, winy);
    GPU_blend(GPU_BLEND_NONE);
    immUnbindProgram();
  }

  ui::Block *block = block_begin(context->C, context->region, __func__, ui::EmbossType::Emboss);
  const int button = int(UI_UNIT_X);
  const int y = int(winy - band + (band - button) / 2.0f);
  int x = int(U.widget_unit * 0.3f);
  block_align_begin(block);
  uiDefIconButO(block,
                ui::ButtonType::But,
                "SEQUENCER_OT_channel_remove",
                wm::OpCallContext::InvokeDefault,
                ICON_REMOVE,
                x,
                y,
                button,
                button,
                std::nullopt);
  x += button;
  uiDefIconButO(block,
                ui::ButtonType::But,
                "SEQUENCER_OT_channel_add",
                wm::OpCallContext::InvokeDefault,
                ICON_ADD,
                x,
                y,
                button,
                button,
                std::nullopt);
  block_align_end(block);
  x += button + int(U.widget_unit * 0.3f);

  /* ★2026-09-21 作者「番号が見ずらいから見やすくしてほしい」。
   * 以前は素の札(#ui::ButtonType::Label)で、時間帯の暗い背景の上に薄い字が乗るだけだった。
   * 数の窓にすると、widget の背景と明るい字で描かれ、そのうえ**打ち込んで段数を決められる**。
   * 読む値は「実際に見えている段数」(strip が居る段は数より下げられない)。 */
  PointerRNA scene_ptr = RNA_id_pointer_create(&context->scene->id);
  const int num_width = max_ii(int(UI_UNIT_X * 2.6f), int(U.widget_unit * 2.6f));
  /* ★空の文字列 = 名前を描かず**数字だけ**。`std::nullopt` にすると RNA の名前
   * ("Channels")が描かれ、この幅では「C」だけ出て数字が押し出される
   * (2026-09-21 作者「表記が C のまま」)。 */
  uiDefButR(block,
            ui::ButtonType::Num,
            "",
            x,
            y,
            num_width,
            button,
            &scene_ptr,
            "falcon_vse_channels",
            0,
            0,
            0,
            TIP_("Channels in the timeline"));
  x += num_width + int(U.widget_unit * 0.3f);

  /* ★2026-09-21 作者「上下入れ替えをここに置いて」= 段数のすぐ隣。
   * ツールバーに出していた物はここへ移し、ツールバーの札は取り下げた。
   * 絵は今の向きを表す(1 が上なら昇り・1 が下なら降り)。 */
  const bool flip_now = seq::channel_flip_enabled();
  uiDefIconButR(block,
                ui::ButtonType::Toggle,
                flip_now ? ICON_SORT_ASC : ICON_SORT_DESC,
                x,
                y,
                button,
                button,
                &scene_ptr,
                "falcon_vse_channel_flip",
                0,
                0,
                0,
                TIP_("Put channel 1 at the top and count downward"));

  block_end(context->C, block);
  block_draw(context->C, block);
  GPU_matrix_pop();
}

void channel_draw_context_init(const bContext *C,
                               ARegion *region,
                               SeqChannelDrawContext *r_context)
{
  r_context->C = C;
  r_context->area = CTX_wm_area(C);
  r_context->region = region;
  r_context->v2d = &region->v2d;
  r_context->scene = CTX_data_sequencer_scene(C);
  r_context->ed = seq::editing_get(r_context->scene);
  r_context->seqbase = seq::active_seqbase_get(r_context->ed);
  r_context->channels = seq::channels_displayed_get(r_context->ed);
  r_context->timeline_region = BKE_area_find_region_type(r_context->area, RGN_TYPE_WINDOW);
  BLI_assert(r_context->timeline_region != nullptr);
  r_context->timeline_region_v2d = &r_context->timeline_region->v2d;

  r_context->channel_height = channel_height_pixelspace_get(r_context->timeline_region_v2d);
  r_context->frame_width = frame_width_pixelspace_get(r_context->timeline_region_v2d);
  r_context->draw_offset = draw_offset_get(r_context->timeline_region_v2d);

  r_context->scale = min_ff(r_context->channel_height / (U.widget_unit * 0.6), 1);
}

void draw_channels(const bContext *C, ARegion *region)
{
  draw_background();
  Scene *scene = CTX_data_sequencer_scene(C);
  if (!scene) {
    return;
  }

  Editing *ed = seq::editing_get(scene);
  if (ed == nullptr) {
    return;
  }

  SeqChannelDrawContext context;
  channel_draw_context_init(C, region, &context);

  if (round_fl_to_int(context.channel_height) == 0) {
    return;
  }

  ui::view2d_view_ortho(context.v2d);

  draw_channel_headers(&context);

  ui::view2d_view_restore(C);

  draw_channel_count_buttons(&context);
}

}  // namespace blender::ed::vse
