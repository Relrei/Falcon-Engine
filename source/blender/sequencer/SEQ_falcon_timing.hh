/* SPDX-FileCopyrightText: 2026 Falcon Engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 *
 * 再生 1 コマの時間を工程ごとに分ける計器(既定では何もしない)。
 *
 * ★なぜ要るか(2026-09-20): 「再生を GPU の経路にするか」を決めるのに、
 * **CPU の使用率の内訳では足りない**。2 本の再生で FFmpeg 66% / その他 178% という
 * 数字は「CPU をどれだけ食ったか」であって、「画面に出るまでの時間のどこが長いか」ではない。
 * 並列で回っている仕事の合計は壁時計より大きくなるし、待ちは CPU% に出ない。
 * ここでは **①工程ごとの仕事の合計(並列込み)** と **②1 コマの壁時計** の両方を出す。
 *
 * 使い方: `FALCON_VSE_TIMING=1`(標準出力)か `FALCON_VSE_TIMING=<書き出す先>`(JSONL)。
 * 切ってある時の費用は、各工程で `enabled()` の一度きりの読み出し 1 回だけ。
 *
 * ★プレビューと先読みは**別に数える**。同じ工程が両方で同時に走るので、混ぜると
 * 「1 コマに 2 倍かかっている」ように見える。 */

#pragma once

#include <cstdint>

namespace blender::seq::timing {

enum class Stage : int {
  /** 素材の復号(動画・画像の読み込み)。 */
  Decode = 0,
  /** 色空間の変換など、変形の前後の下ごしらえ(Transform を内側に含む)。 */
  Preprocess,
  /** 拡大・縮小・移動・回転(`IMB_transform`)。 */
  Transform,
  /** 重ね合わせ(blend mode)。 */
  Blend,
  /** キャッシュの追い出し。 */
  Evict,
  /** 表示用の GPU テクスチャへの転送。 */
  Upload,
  /** GPU 経路での変形と重ね(`SEQ_gpu_preview.hh`)。 */
  GpuComposite,
  Count,
};

/** 計器が入っているか(環境変数を 1 回だけ読む)。 */
bool enabled();

/** 工程に時間を足す。`prefetch` = 本家の先読みジョブの中での呼び出しか。 */
void add(Stage stage, double seconds, bool prefetch);

/** 1 コマ分の壁時計と、その間に積まれた工程の時間を 1 行にして出す。 */
void frame_done(int timeline_frame, double wall_seconds, bool prefetch);

/** 範囲で測る道具。`enabled()` が偽なら時計も読まない。 */
class Scope {
 public:
  Scope(Stage stage, bool prefetch);
  ~Scope();

 private:
  Stage stage_;
  bool prefetch_;
  double start_;
};

}  // namespace blender::seq::timing
