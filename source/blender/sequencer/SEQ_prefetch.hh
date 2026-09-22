/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

namespace blender {

struct bContext;
struct Scene;

namespace seq {

void prefetch_stop_all();
/**
 * Use also to update scene and context changes
 * This function should almost always be called by cache invalidation, not directly.
 */
void prefetch_stop(Scene *scene);
bool prefetch_need_redraw(const bContext *C, Scene *scene);
/**
 * Falcon: 先読みの糸が「まだ後れを取り戻している最中」か。新しく足した連番の
 * 冷たいキャッシュを埋めている間だけ真になる(既に追いついて眠っている時は偽)。
 * UI に「読み込み中」を出すための問い合わせ専用(挙動は変えない)。
 */
bool prefetch_is_catching_up(Scene *scene);

}  // namespace seq
}  // namespace blender
