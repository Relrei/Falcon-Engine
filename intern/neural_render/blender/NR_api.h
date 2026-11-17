/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <string>

namespace blender {

struct Image;

/* Create python module _cycles used by addon. */
void *NR_python_module_init();

void NR_log_init();
void NR_implicit_sharing_init();

/* Texture cache generation. */

bool NR_resolve_texture_cache(const Image *image,
                               const char *filepath,
                               const char *texture_cache_directory,
                               std::string &r_tx_filepath);

bool NR_generate_texture_cache(const Image *image,
                                const char *filepath,
                                const char *texture_cache_directory = "");

}  // namespace blender
