/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup imbuf
 */

namespace blender {

/**
 * Falcon: この糸で読む 16bit 整数の画像(16bit PNG/TIFF など)を 8bit の絵で読む。
 * VSE のプレビュー用(書き出しには使わない)。16bit のままだと浮動小数の絵になり 1 枚 32MB・
 * GPU の再生経路にも乗らない。半精度・浮動小数の画像(EXR など)は HDR なので落とさない。
 */
void IMB_prefer_byte_for_thread(bool prefer_byte);

/** Falcon: 上の指定で 8bit へ落とした絵を作ったことがあるか(読むと倒れる)。 */
bool IMB_take_byte_downgrade_happened();

}  // namespace blender
