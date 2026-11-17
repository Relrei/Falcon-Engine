# SPDX-FileCopyrightText: 2026 Relrei
#
# SPDX-License-Identifier: Apache-2.0

# Falcon v5: the NGX entry points of the DLSS denoisers, resolved at run time from the NVIDIA
# driver (libnvidia-ngx.so.1 / _nvngx.dll) instead of linking the SDK's static stub
# (libnvsdk_ngx.a), so no NVIDIA object code ends up in the binary. Linked by cycles_integrator
# and nr_integrator.

# Read before intern/cycles and intern/neural_render, which find the SDK: a first configure had no
# DLSS_INCLUDE_DIR here yet (11-15, the Rocky Linux 8 build: nvsdk_ngx.h not found).
find_package(DLSS REQUIRED)

set(INC
)

set(INC_SYS
  ${DLSS_INCLUDE_DIR}
)

set(SRC
  ngx_dlopen.cpp
)

set(LIB
)

add_definitions(-DWITH_DLSS)

blender_add_lib(falcon_ngx_dlopen "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
