/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Falcon: what the status bar shows about a running render job. Kept apart from `ED_render.hh`,
 * which `makesrna` includes, so that changing this does not regenerate RNA.
 */

#pragma once

namespace blender {

struct Scene;
struct wmWindowManager;

/**
 * The frame an animation render job of \a scene is on (1 = first) and how many frames it
 * renders. False when there is no such job or it is not an animation.
 */
bool ED_render_job_frame_info(wmWindowManager *wm, const Scene *scene, int *r_index, int *r_total);

}  // namespace blender
