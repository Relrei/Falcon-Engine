/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include <algorithm>
#include <cstdlib>

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BKE_anim_data.hh"
#include "BKE_main.hh"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "SEQ_channels.hh"
#include "SEQ_fastpath.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_time.hh"

namespace blender::seq {

#define FAIL(...) \
  do { \
    BLI_snprintf(r_reason, reason_maxncpy, __VA_ARGS__); \
    return false; \
  } while (false)

/** 1本のストリップが「切っただけ」か。
 *
 * 判定の順は「直しやすい物から」。最初に当たった1件だけを理由として返す。
 */
static bool strip_is_plain_cut(const Scene *scene,
                               const Strip *strip,
                               char *r_reason,
                               int reason_maxncpy)
{
  const char *n = strip->name + 2;

  if (strip->type != STRIP_TYPE_MOVIE) {
    FAIL("Strip '%s' is not a movie strip", n);
  }
  if (!BLI_listbase_is_empty(&strip->modifiers)) {
    FAIL("Strip '%s' has a modifier on it", n);
  }
  if (strip->data == nullptr || strip->data->stripdata == nullptr) {
    FAIL("Strip '%s' has no source file", n);
  }
  /* ★通常の描画は、この旗があると素材の絵を作り直す(飛び越し除去・浮動小数化)。
   * パケットをそのまま流すと、その処理が丸ごと抜ける。 */
  if (strip->flag & SEQ_DEINTERLACE) {
    FAIL("Strip '%s' is deinterlaced", n);
  }
  if (strip->flag & SEQ_MAKE_FLOAT) {
    FAIL("Strip '%s' is converted to float", n);
  }
  /* ★素材の色空間がシーケンサの作業空間と違うと、通常の描画は色空間を変換する。
   * 同じ名前の時だけが「何もしない」変換なので、それ以外は通常の書き出しへ回す。 */
  if (!STREQ(strip->data->colorspace_settings.name, scene->sequencer_colorspace_settings.name)) {
    FAIL("Strip '%s' is in colour space '%s', which the normal render converts",
         n,
         strip->data->colorspace_settings.name);
  }
  if (const StripTransform *tr = strip->data->transform) {
    if (tr->xofs != 0.0f || tr->yofs != 0.0f) {
      FAIL("Strip '%s' is moved", n);
    }
    if (tr->scale_x != 1.0f || tr->scale_y != 1.0f) {
      FAIL("Strip '%s' is scaled", n);
    }
    if (tr->rotation != 0.0f) {
      FAIL("Strip '%s' is rotated", n);
    }
  }
  if (const StripCrop *cr = strip->data->crop) {
    if (cr->top || cr->bottom || cr->left || cr->right) {
      FAIL("Strip '%s' is cropped", n);
    }
  }
  if (strip->sat != 1.0f || strip->mul != 1.0f) {
    FAIL("Strip '%s' has a colour change on it", n);
  }
  /* ⚠ 動画ストリップの既定は REPLACE ではなく **ALPHA_OVER**(実機で確認)。
   * 不透明な素材を全開で乗せるのは REPLACE と同じ絵になるので、両方を通す。
   * ★ここを REPLACE だけにすると**素の動画ストリップが全部落ちて、機能が一度も
   * 発動しない**。`strobe` の既定が 0.0 で同じ穴を踏んだのと同じ形。 */
  if (!ELEM(strip->blend_mode, STRIP_BLEND_REPLACE, STRIP_BLEND_ALPHAOVER)) {
    FAIL("Strip '%s' is blended into what is under it", n);
  }
  if (strip->blend_opacity != 100.0f) {
    FAIL("Strip '%s' is not fully opaque (opacity %.0f%%)", n, strip->blend_opacity);
  }
  if (strip->flag & (SEQ_FLIPX | SEQ_FLIPY)) {
    FAIL("Strip '%s' is flipped", n);
  }
  if (strip->flag & SEQ_REVERSE_FRAMES) {
    FAIL("Strip '%s' plays in reverse", n);
  }
  if (strip->flag & SEQ_USE_PROXY) {
    FAIL("Strip '%s' renders from a proxy, not the original", n);
  }
  if (strip->flag & SEQ_MULTIPLY_ALPHA) {
    FAIL("Strip '%s' multiplies alpha", n);
  }
  /* ⚠ `strobe` の既定は 0.0 で、1.0 ではない。どちらも「毎コマ出す」の意味。
   * `!= 1.0f` で弾くと**素の動画ストリップが全部落ちる**(アドオン側で実測済み)。 */
  if (strip->strobe != 0.0f && strip->strobe != 1.0f) {
    FAIL("Strip '%s' uses strobe", n);
  }
  /* ★2本でも止める。速度の変更(`strip_speed_set`)は**キー2本**で表され、その2本目の
   * 係数が速度になる。`> 2` で通すと、速度を変えた素の1本が「切っただけ」に見える。 */
  if (strip->retiming_keys_num > 0) {
    FAIL("Strip '%s' is retimed", n);
  }
  if (strip->speed_factor != 0.0f && strip->speed_factor != 1.0f) {
    FAIL("Strip '%s' plays at a changed speed", n);
  }
  /* ★ハンドルを素材の外へ引くと、通常の描画は端のコマを止めて見せる(ホールド)。
   * こちらはパケットを進めるだけなので、別のコマ・ファイルの終わりに当たる。 */
  if (strip->startofs < 0.0f || strip->endofs < 0.0f) {
    FAIL("Strip '%s' holds its first or last frame past the source", n);
  }
  return true;
}

/** 戻す口。`FALCON_VSE_FASTPATH=0` で切ると、従来どおり全部を描き直す。
 *
 * ★既定は ON。切れるようにしてあるのは、**壊れた時に作者が1つの環境変数で
 * 元の挙動へ戻せる**ようにするため(絵が変わったように見える時の切り分けにも使う)。 */
static bool fastpath_enabled()
{
  static const bool enabled = [] {
    const char *env = getenv("FALCON_VSE_FASTPATH");
    if (env == nullptr || env[0] == '\0') {
      return true;
    }
    return atoi(env) != 0;
  }();
  return enabled;
}

bool fastpath_cuts_get(const Scene *scene,
                       const RenderData *rd,
                       const int frame_start,
                       const int frame_end,
                       const int frame_step,
                       Vector<FastPathCut> &r_cuts,
                       char *r_reason,
                       int reason_maxncpy)
{
  r_reason[0] = '\0';

  if (!fastpath_enabled()) {
    FAIL("Switched off with FALCON_VSE_FASTPATH=0");
  }

  const Editing *ed = editing_get(scene);
  if (ed == nullptr) {
    FAIL("This scene has no sequencer");
  }
  if ((rd->scemode & R_DOSEQ) == 0) {
    FAIL("The sequencer is switched off for this scene");
  }
  if (rd->size != 100) {
    FAIL("Output resolution is %d%%; the fast path needs 100%%", rd->size);
  }
  if (rd->scemode & R_MULTIVIEW) {
    FAIL("Multi-view output cannot be copied straight through");
  }
  /* ★刻みは `rd` でなく、**実際に描く刻み**(`RE_RenderAnim` の引数)で見る。 */
  if (frame_step != 1) {
    FAIL("Frame stepping needs the normal render");
  }
  /* Copying compressed packets cannot apply display transforms or animated edits.
   * Be conservative for drivers and NLA too, including animation outside the strips. */
  if (BKE_animdata_id_is_animated(&scene->id)) {
    FAIL("Scene animation or drivers need the normal render");
  }
  const ColorManagedViewSettings &view = scene->view_settings;
  if (!STREQ(view.view_transform, "Standard") || !STREQ(view.look, "None") ||
      view.exposure != 0.0f || view.gamma != 1.0f ||
      (view.flag & (COLORMANAGE_VIEW_USE_CURVES | COLORMANAGE_VIEW_USE_WHITE_BALANCE)) ||
      !STREQ(scene->display_settings.display_device, "sRGB") ||
      !STREQ(scene->sequencer_colorspace_settings.name, "sRGB") ||
      rd->im_format.color_management == R_IMF_COLOR_MANAGEMENT_OVERRIDE)
  {
    FAIL("Color management needs the normal render");
  }
  if ((rd->scemode & R_DOCOMP) && scene->compositing_node_group != nullptr) {
    FAIL("Scene compositing needs the normal render");
  }
  /* ★通常の書き出しは、この条件で絵に文字(スタンプ)を焼き込む(`do_render_full_pipeline`)。
   * パケットをそのまま流すと、その焼き込みが丸ごと抜ける。 */
  if ((scene->r.stamp & R_STAMP_ALL) && (scene->r.stamp & R_STAMP_DRAW)) {
    FAIL("Burning the stamp into the picture needs the normal render");
  }

  Vector<const Strip *> strips;
  for (const Strip &strip : ed->seqbase) {
    const SeqTimelineChannel *channel = channel_get_by_index(&ed->channels, strip.channel);
    if (channel != nullptr && channel->is_muted()) {
      /* Do not silently concatenate across a channel that the user hid. */
      FAIL("A muted channel needs the normal render");
    }
    if (strip.flag & SEQ_MUTE) {
      continue;
    }
    /* ★音のストリップは素通しする。**音は運ばずに、Blender の普通のミックス
     * ダウンにそのまま作らせる**(fast path が省くのは映像の復号と符号化だけ)。
     * こうすると音量もフェードも普通に効くので、判定で縛る必要がない。 */
    if (ELEM(strip.type, STRIP_TYPE_SOUND, STRIP_TYPE_SOUND_HD)) {
      continue;
    }
    if (!strip_is_plain_cut(scene, &strip, r_reason, reason_maxncpy)) {
      return false;
    }
    strips.append(&strip);
  }
  if (strips.is_empty()) {
    FAIL("The timeline has no unmuted strip");
  }

  std::sort(strips.begin(), strips.end(), [](const Strip *a, const Strip *b) {
    return a->left_handle() < b->left_handle();
  });

  /* 重なっていると合成が要るので、そのままは流せない。 */
  for (int i = 1; i < strips.size(); i++) {
    if (strips[i]->left_handle() < strips[i - 1]->right_handle(scene)) {
      FAIL("Strips '%s' and '%s' overlap in time; that needs compositing",
           strips[i - 1]->name + 2,
           strips[i]->name + 2);
    }
  }

  /* ★書くのは**レンダー範囲**であって、タイムライン全体ではない。
   *
   * ここを見ていないと、開始/終了を絞って書き出したのに**タイムラインの全部**が
   * 出る。しかも本数が違うだけで絵は正しいので、書き出しが終わるまで気づけない。
   *
   * ★範囲は `rd->sfra/efra` でなく、**呼び出し側が実際に描く範囲**(`RE_RenderAnim` の
   * `sfra/efra`)で取る。`blender -f N` や、操作の開始/終了フレームの指定は
   * シーンの範囲と違う。 */
  const int range_start = frame_start;
  const int range_end = frame_end + 1; /* 終端は開いた区間で持つ。 */
  if (range_end <= range_start) {
    FAIL("The render range is empty");
  }
  /* ★通常の書き出しは、音を**シーンの開始から**混ぜて `(コマ - sfra + 1)` 秒ぶんまで
   * 書く(`ffmpeg_movie_append`)。範囲の頭がシーンの開始と違う時は、こちらの音の長さが
   * 通常と食い違うので、通常の書き出しへ回す。 */
  if (frame_start != rd->sfra && rd->ffcodecdata.audio_codec_id_get() != FFMPEG_CODEC_ID_NONE) {
    FAIL("The render range does not start at the scene start; the audio timing needs the "
         "normal render");
  }

  const char *blendfile_path = BKE_main_blendfile_path_from_global();
  int covered_until = range_start;
  for (const Strip *strip : strips) {
    /* ⚠ `start` は「素材の0コマ目が来るタイムライン位置」であって、ストリップの
     * 左端ではない。取り違えると切り出す位置が丸ごとずれる(絵は出るので
     * 気づきにくい)。左端は `left_handle()`。 */
    const int left = strip->left_handle();
    const int right = strip->right_handle(scene);
    const int from = std::max(left, range_start);
    const int to = std::min(right, range_end);
    if (to <= from) {
      continue; /* レンダー範囲の外。 */
    }
    /* ★範囲の中に隙間があってはいけない。通常の書き出しは隙間を黒で埋めるが、
     * こちらは詰めて繋いでしまうので、**尺は同じで中身がずれた動画**になる。 */
    if (from > covered_until) {
      FAIL("There is a gap in the timeline at frame %d; the normal render fills it",
           covered_until);
    }
    covered_until = to;

    /* ★素材のどのコマを取るかは、通常の描画(`seq_render_movie_strip_view`)と**同じ式**で決める。
     * 素材のコマ = `give_frame_index()` を丸めた値 + `anim_startofs`。
     * `anim_startofs` は「素材の頭から捨てるコマ数」で、ハード分割(`SPLIT_HARD`)は
     * 右側のストリップの `start` を分割位置へ動かして、そのぶんをここへ積む。
     * これを足さないと、分割した右側が**素材の頭から**再生される(絵は出るのでずれに
     * 気づけない)。再生速度の倍率(`media_playback_rate_factor`)やホールドも
     * `give_frame_index()` が面倒を見るので、頭と尻の両方を突き合わせ、1コマずつ
     * 進まない(= 通常の描画と同じにならない)区間は通さない。 */
    const int src_first = round_fl_to_int(give_frame_index(scene, strip, float(from))) +
                          strip->anim_startofs;
    const int src_last = round_fl_to_int(give_frame_index(scene, strip, float(to - 1))) +
                         strip->anim_startofs;
    if (src_first < 0 || src_last - src_first != to - from - 1) {
      FAIL("Strip '%s' does not map one timeline frame to one source frame "
           "(playback rate, hold frames or offsets)",
           strip->name + 2);
    }

    FastPathCut cut;
    cut.in_frame = src_first;
    cut.n_frames = to - from;
    BLI_path_join(
        cut.path, sizeof(cut.path), strip->data->dirpath, strip->data->stripdata->filename);
    BLI_path_abs(cut.path, blendfile_path);
    r_cuts.append(cut);
  }
  if (r_cuts.is_empty()) {
    FAIL("The render range has no strip in it");
  }
  if (covered_until < range_end) {
    FAIL("The timeline stops at frame %d but the render range goes to %d",
         covered_until,
         range_end - 1);
  }
  return true;
}

#undef FAIL

}  // namespace blender::seq
