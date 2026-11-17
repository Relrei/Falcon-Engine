/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "util/implicit_sharing.h"

#include "BLI_implicit_sharing.hh"

#include "blender/NR_api.h"

namespace blender {

void NR_implicit_sharing_init()
{
  nr::implicit_sharing_init(
      [](nr::ImplicitSharingInfo data) {
        const auto *info = static_cast<const blender::ImplicitSharingInfo *>(data);
        info->add_user();
      },
      [](nr::ImplicitSharingInfo data) {
        const auto *info = static_cast<const blender::ImplicitSharingInfo *>(data);
        info->remove_user_and_delete_if_last();
      });
}

}  // namespace blender
