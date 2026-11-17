#!/bin/bash
# Falcon Engine: build the Linux release in the Rocky Linux 8 image (falcon_rocky8.Dockerfile), so it
# runs on the Linux distributions the official Blender runs on (glibc 2.28 and newer).
# The release options of Blender (blender_release.cmake), NVIDIA only: CUDA kernels for RTX 20 to 50
# (sm_75 sm_86 sm_89 sm_120) and PTX (compute_75) for the others.
#   falcon_rocky8_build.sh [ninja target...]   (paths from the environment, defaults below)
set -e
SRC=${SRC:-$(cd "$(dirname "$0")/../../.." && pwd)}
BUILD=${BUILD:-$(dirname "$SRC")/build_rocky8}
LIBDIR=${LIBDIR:-$SRC/lib/linux_x64}
# The NVIDIA SDKs are not in this tree: download them from NVIDIA and point to them.
OPTIX=${OPTIX:?set OPTIX to the OptiX SDK folder (e.g. OPTIX=~/sdk/optix-9.0.0)}
DLSS=${DLSS:?set DLSS to the DLSS SDK folder (e.g. DLSS=~/sdk/DLSS)}
CCACHE=${CCACHE:-$(dirname "$SRC")/ccache_rocky8}
JOBS=${JOBS:-7}
mkdir -p "$BUILD" "$CCACHE"
# --network none: nothing is fetched while building (and no veth is needed).
exec docker run --rm --network none --user "$(id -u):$(id -g)" -e HOME=/tmp -e CCACHE_DIR="$CCACHE" \
  -v "$SRC:$SRC:ro" -v "$LIBDIR:$LIBDIR:ro" -v "$OPTIX:$OPTIX:ro" -v "$DLSS:$DLSS:ro" \
  -v "$BUILD:$BUILD" -v "$CCACHE:$CCACHE" -w "$BUILD" falcon-rocky8 bash -c "
set -e
if [ ! -f build.ninja ]; then
  cmake -G Ninja -DCMAKE_MAKE_PROGRAM=/usr/local/bin/ninja '$SRC' -C '$SRC/build_files/cmake/config/blender_release.cmake' \
    -DCMAKE_BUILD_TYPE=Release -DLIBDIR='$LIBDIR' -DWITH_LIBS_PRECOMPILED=ON \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DWITH_INSTALL_PORTABLE=ON -DWITH_DOC_MANPAGE=OFF -DWITH_LINKER_MOLD=OFF -DWITH_LINKER_LLD=OFF \
    -DWITH_FALCON_NEURAL_RENDER=ON \
    -DWITH_CYCLES_DEVICE_HIP=OFF -DWITH_CYCLES_HIP_BINARIES=OFF -DWITH_CYCLES_DEVICE_HIPRT=OFF \
    -DWITH_CYCLES_DEVICE_ONEAPI=OFF -DWITH_CYCLES_ONEAPI_BINARIES=OFF \
    -DWITH_CYCLES_CUDA_BINARIES=ON -DCYCLES_CUDA_BINARIES_ARCH='sm_75;sm_86;sm_89;sm_120;compute_75' \
    -DCUDA_TOOLKIT_ROOT_DIR=/usr/local/cuda-12.8 -DOPTIX_ROOT_DIR='$OPTIX' \
    -DWITH_DLSS=ON -DDLSS_SDK_ROOT='$DLSS'
fi
nice -n 10 ninja -j$JOBS ${*:-all install}
"
