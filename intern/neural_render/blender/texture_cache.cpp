/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "DNA_image_types.h"

#include "blender/NR_api.h"

#include "util/image_maketx.h"
#include "util/image_metadata.h"
#include "util/types_image.h"

namespace blender {

bool NR_resolve_texture_cache(const Image *image,
                               const char *filepath,
                               const char *texture_cache_directory,
                               std::string &r_tx_filepath)
{
  nr::ImageMetaData tx_metadata;
  return nr::resolve_tx(filepath,
                         texture_cache_directory,
                         nr::ustring(image->colorspace_settings.name),
                         nr::ImageAlphaType(image->alpha_mode),
                         nr::IMAGE_FORMAT_PLAIN,
                         r_tx_filepath,
                         tx_metadata);
}

bool NR_generate_texture_cache(const Image *image,
                                const char *filepath,
                                const char *texture_cache_directory)
{
  std::string tx_filepath;
  nr::ImageMetaData tx_metadata;
  const bool is_valid = nr::resolve_tx(filepath,
                                        texture_cache_directory,
                                        nr::ustring(image->colorspace_settings.name),
                                        nr::ImageAlphaType(image->alpha_mode),
                                        nr::IMAGE_FORMAT_PLAIN,
                                        tx_filepath,
                                        tx_metadata);

  if (is_valid) {
    return true;
  }

  if (tx_filepath.empty()) {
    return false;
  }

  return make_tx(filepath,
                 tx_filepath,
                 nr::ustring(image->colorspace_settings.name),
                 nr::ImageAlphaType(image->alpha_mode),
                 nr::IMAGE_FORMAT_PLAIN);
}

}  // namespace blender
