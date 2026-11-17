# SPDX-FileCopyrightText: 2006 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

add_subdirectory(atomic)
add_subdirectory(clog)
add_subdirectory(ghost)
add_subdirectory(guardedalloc)
add_subdirectory(libmv)
add_subdirectory(memutil)
add_subdirectory(opensubdiv)
add_subdirectory(profile)
add_subdirectory(mikktspace)
add_subdirectory(eigen)
add_subdirectory(sky)

add_subdirectory(openvdb)

if(WITH_MOD_REMESH)
  add_subdirectory(dualcon)
endif()

if(WITH_IK_SOLVER)
  add_subdirectory(iksolver)
endif()

if(WITH_IK_ITASC)
  add_subdirectory(itasc)
endif()

# Falcon 1.0: 公式の Cycles は WITH_FALCON_OFFICIAL_CYCLES の時だけ(FALCON_BUILD_OFFICIAL_CYCLES・上の CMakeLists.txt)
if(FALCON_BUILD_OFFICIAL_CYCLES)
  add_subdirectory(cycles)
endif()

# Falcon v5: DLSS の NGX をドライバーから実行時に読む口(Cycles と NR の DLSS が使う)
if(WITH_CYCLES AND WITH_DLSS)
  add_subdirectory(falcon_ngx_dlopen)
endif()

# Falcon: Neural Rendering(Cycles を写した別のエンジン)
if(WITH_CYCLES AND WITH_FALCON_NEURAL_RENDER)
  add_subdirectory(neural_render)
endif()

if(WITH_BULLET)
  add_subdirectory(rigidbody)
endif()


if(WIN32)
  # Only windows needs utf16 converter.
  add_subdirectory(utfconv)

  # Only used for Windows for now.
  add_subdirectory(uriconvert)
endif()

if(WITH_MOD_FLUID)
  add_subdirectory(mantaflow)
endif()

if(WITH_UV_SLIM)
  add_subdirectory(slim)
endif()

if(WITH_QUADRIFLOW)
  add_subdirectory(quadriflow)
endif()

if(UNIX AND NOT APPLE)
  add_subdirectory(libc_compat)
endif()

if(WITH_RENDERDOC)
  add_subdirectory(renderdoc_dynload)
endif()

if(UNIX AND NOT APPLE)
  # Important this comes after "ghost" as it uses includes defined by GHOST's CMake.
  if(WITH_GHOST_WAYLAND AND WITH_GHOST_WAYLAND_DYNLOAD)
    add_subdirectory(wayland_dynload)
  endif()
  if(WITH_GHOST_DBUS AND WITH_GHOST_DBUS_DYNLOAD)
    add_subdirectory(dbus_dynload)
  endif()
endif()

if(WITH_DRACO)
  add_subdirectory(draco_bridge)
endif()

if(WITH_MESHOPTIMIZER)
  add_subdirectory(meshoptimizer_bridge)
endif()

if(WITH_KTX)
  add_subdirectory(ktx_bridge)
endif()
