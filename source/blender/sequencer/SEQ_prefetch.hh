/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

namespace blender {
struct Main;

struct bContext;
struct Scene;

namespace seq {

void prefetch_stop_all();
/**
 * Use also to update scene and context changes
 * This function should almost always be called by cache invalidation, not directly.
 */
void prefetch_stop(Scene *scene);

/* Falcon 2026-09-26: F12 / Ctrl+F12 の間だけ、VSE の先読み・GPU プレビューの先回り・画像の先回りを止め、
 * 動画の読み手を全部閉じてからレンダーの糸を起こす(レンダーは素の読み手を開き直す)。
 * GUI の動画書き出しで、画面に無いストリップの読み手をレンダーの糸が閉じた瞬間に NVDEC の後始末で
 * SIGBUS になった(2026-09-26 07:09・headless では起きない = 別スレッドが同じ読み手を使っていた)。
 * begin は主スレッド(GPU 文脈あり)から、レンダーの糸を起こす前に。end はレンダーの job の終わりで。 */
void render_exclusive_begin(Main *bmain);
void render_exclusive_end();
bool render_exclusive_active();
bool prefetch_need_redraw(const bContext *C, Scene *scene);
/**
 * Falcon: 先読みの糸が「まだ後れを取り戻している最中」か。新しく足した連番の
 * 冷たいキャッシュを埋めている間だけ真になる(既に追いついて眠っている時は偽)。
 * UI に「読み込み中」を出すための問い合わせ専用(挙動は変えない)。
 */
bool prefetch_is_catching_up(Scene *scene);

}  // namespace seq
}  // namespace blender
