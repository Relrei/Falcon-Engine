/* SPDX-FileCopyrightText: 2026 Falcon Engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 *
 * 再生プレビューの 1 枚を **GPU の上だけで** 組み立てる経路(既定では切ってある)。
 *
 * ★なぜ要るか(2026-09-20 の実測): 先読みが 1 コマを作る時間のうち、
 * 変形(`IMB_transform`)と重ね(Alpha Over)で FHD 4 本なら 20ms / 4K 4 本なら 110ms を
 * 使っている(`(internal notes)`)。どちらも GPU なら 1ms 未満で終わる。
 * ただし **CPU に戻す(読み戻す)と 32〜203ms** かかって全部帳消しになるので
 * (`RESULT_gpu_probe.md`)、**作った絵を GPU に置いたまま画面へ渡す**のがこの経路の要点。
 *
 * 何をするか:
 *   素材の復号(CPU)→ テクスチャへ送る → 変形と重ねを GPU → そのまま表示。
 *   仕上がりの ImBuf を作らないので、**最終キャッシュも GPU への転送も要らない**。
 *
 * 通せる配置だけを通す(それ以外は今までの CPU 経路へ落ちる)。条件は `gpu_preview_render()`
 * の中の `strip_is_eligible()` を参照。合わない物が 1 本でもあれば丸ごと諦める。
 *
 * 戻す口: `FALCON_VSE_GPU_PREVIEW=0`(既定)。 */

#pragma once

namespace blender {
namespace gpu {
class Texture;
}
struct Scene;

namespace seq {

struct RenderData;

/** この経路が有効か(場面に保存された値・無ければ環境変数・既定は無効)。 */
bool gpu_preview_enabled();

/** VSE の色補正を建てた時の命令セットの名札("O3 (auto-vectorized)" 等)。 */
const char *falcon_simd_tier();

/** 色補正の核の選び方(0=自動 1=携帯 2=AVX2 3=参照)。 */
int falcon_cpu_kernel_get();
void falcon_cpu_kernel_store(Scene *scene, int value);
void falcon_cpu_kernel_sync_from_scene(const Scene *scene);

/** この CPU が AVX2 を持っているか(実行時の判定)。 */
bool falcon_cpu_has_avx2();

/** 実行時の値だけを変える。保存もするなら #gpu_preview_store。 */
void gpu_preview_set(bool enable);

/** 場面が値を持っていれば実行時へ写す(描く直前に呼ぶ)。 */
void gpu_preview_sync_from_scene(const Scene *scene);

/** 実行時の値を変え、場面にも保存する(開き直しても残る)。 */
void gpu_preview_store(Scene *scene, bool enable);

/**
 * 直近のプレビューで GPU 経路が**実際に通ったか**。
 *
 * 先読みの側が「素材の復号だけをすればよい」かどうかの判断に使う。通らない配置(効果や
 * 切り抜きが混ざっている等)では今までどおり先読みに仕上がりまで作らせないと、
 * 画面を描く側が毎コマ合成することになって逆に遅くなる。
 */
bool gpu_preview_is_active();

/**
 * 1 コマを GPU の上で組み立てて返す。通せない配置なら nullptr。
 *
 * 呼べるのは **GPU コンテキストが有効なスレッド**(= 描画中の主スレッド)だけ。
 * 返るテクスチャの持ち主は呼び手(`GPU_texture_free` するか preview cache へ預ける)。
 * `r_colorspace_name` には、そのテクスチャの色空間の名前が入る(表示側が要る)。
 * 中身は **アルファを掛けた形(premultiplied)** なので、表示側は predivide を真にする。
 * `r_owned` が偽なら**輪から借りた物**なので、呼び手は手放してはいけない。
 */
gpu::Texture *gpu_preview_render(const RenderData *context,
                                 float timeline_frame,
                                 int chanshown,
                                 const char **r_colorspace_name,
                                 bool *r_owned);

/**
 * 先読みのスレッドで 1 コマを作って輪へ預ける(副 GPU 文脈を使う)。
 *
 * ★これが段 2b の要点。画面を描く側で合成すると、**先読み(別スレッド)で隠れていた仕事が
 * 主スレッドへ出てきて**、先読みが余っている編集や 4K では逆に遅くなる(2026-09-20 実測)。
 * ここで作っておけば、画面を描く側は**描くだけ**になる。
 *
 * 何コマ先まで作るかは `FALCON_VSE_GPU_AHEAD`(既定 6・0 で無効)。
 * 作れなかった時(GPU が使えないスレッド・通らない配置)は false。素材の復号は無駄にならない。
 */
bool gpu_preview_produce(const RenderData *context, float timeline_frame, int chanshown);

/** 輪を空にする。★GPU 文脈が有効な所からだけ呼ぶこと。 */
void gpu_preview_ring_clear();

/**
 * 何コマ先まで輪に入れておくか(`FALCON_VSE_GPU_AHEAD`・**既定 0 = 先読み側では作らない**)。
 *
 * ★先読みはこの数より先へ行ってはいけない。行っても輪から押し出されるだけで、
 * 画面を描く側が使う頃には残っていない(2026-09-20: ここを縛らずに作って、
 * 先読みが 600 コマ先まで走り、輪が 1 度も当たらなかった)。
 *
 * ★ただし縛ると読み先が尽きる。既定を 0 にしてあるのはそのため(実測の経緯は実装側の注記)。
 */
int gpu_preview_ahead_frames();

}  // namespace seq
}  // namespace blender
