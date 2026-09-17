/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include <string>

namespace blender {

struct Scene;
struct Strip;

namespace seq {

bool sequencer_strip_generates_image(Strip *strip);
void strip_open_anim_file(Scene *scene, Strip *strip, bool openfile);

/**
 * Absolute, normalized path of the media file a movie strip reads from, or an empty string when
 * the strip has no source file.
 *
 * (Falcon) This is the key that ties a finished proxy build back to *every* strip that reads the
 * file. Cutting a clip in two (K) leaves several strips sharing one file, and only the first of
 * them ever reaches the proxy build queue, so a per-strip key misses the rest.
 */
std::string strip_movie_source_path_get(const Scene *scene, const Strip *strip);

}  // namespace seq
}  // namespace blender
