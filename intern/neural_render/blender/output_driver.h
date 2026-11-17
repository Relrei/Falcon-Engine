/* SPDX-FileCopyrightText: 2021-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "session/output_driver.h"

namespace blender {
struct RenderEngine;
}

CCL_NAMESPACE_BEGIN

class BlenderOutputDriver : public OutputDriver {
 public:
  explicit BlenderOutputDriver(blender::RenderEngine &b_engine);
  ~BlenderOutputDriver() override;

  void write_render_tile(const Tile &tile) override;
  bool update_render_tile(const Tile &tile) override;
  bool read_render_tile(const Tile &tile) override;

  /* Neural Rendering: the DLSS pre-roll passes only warm the history inside DLSS, their picture
   * is replaced by the kept pass, so it is not written into the render result. */
  void set_discard_writes(const bool discard)
  {
    discard_writes_ = discard;
  }

 protected:
  blender::RenderEngine &b_engine_;
  bool discard_writes_ = false;
};

CCL_NAMESPACE_END
