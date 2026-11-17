# Falcon Engine: the Linux release build environment, Rocky Linux 8 (glibc 2.28) as the official
# Blender builds (linux_rocky8_setup.sh), with only what Blender itself needs: the libraries come
# precompiled (lib/linux_x64), so the packages that build them and ROCm are left out.
#   docker build --network host -t falcon-rocky8 -f falcon_rocky8.Dockerfile .
FROM rockylinux:8

RUN dnf -y install 'dnf-command(config-manager)' && \
    dnf config-manager --set-enabled powertools && \
    dnf -y install epel-release && \
    dnf -y install scl-utils scl-utils-build && \
    dnf -y install gcc-toolset-14 && \
    dnf clean all

RUN dnf -y install git git-lfs patch make ninja-build patchelf wget tar bzip2 zstd ccache \
        python3 python3-devel \
        mesa-libEGL-devel mesa-libGL-devel mesa-libGLU-devel mesa-libgbm-devel libdrm-devel \
        libxkbcommon-devel libX11-devel libXcursor-devel libXi-devel libXinerama-devel \
        libXrandr-devel libXt-devel libXxf86vm-devel libXext-devel libXrender-devel libXfixes-devel \
        dbus-devel systemd-devel libffi-devel \
        alsa-lib-devel pulseaudio-libs-devel jack-audio-connection-kit-devel && \
    (dnf -y install pipewire-devel || true) && \
    dnf clean all

# CUDA 12.8 as the official builds: the compiler and its headers only (Cycles loads the driver API
# at run time). 12.8 builds sm_75 (RTX 20) to sm_120 (RTX 50).
RUN dnf config-manager --add-repo \
        https://developer.download.nvidia.com/compute/cuda/repos/rhel8/x86_64/cuda-rhel8.repo && \
    dnf -y install cuda-nvcc-12-8 cuda-cudart-devel-12-8 cuda-driver-devel-12-8 && \
    dnf clean all
ENV PATH=/usr/local/cuda-12.8/bin:$PATH

# CMake 3.31 as linux_rocky8_setup.sh (the packaged one is too old).
RUN wget -q https://github.com/Kitware/CMake/releases/download/v3.31.12/cmake-3.31.12-linux-x86_64.sh && \
    sh cmake-3.31.12-linux-x86_64.sh --prefix=/usr --skip-license && rm -f cmake-3.31.12-linux-x86_64.sh

# The tool-chain of the VFX platform, as `scl enable gcc-toolset-14`.
ENV PATH=/opt/rh/gcc-toolset-14/root/usr/bin:$PATH \
    LD_LIBRARY_PATH=/opt/rh/gcc-toolset-14/root/usr/lib64 \
    PKG_CONFIG_PATH=/opt/rh/gcc-toolset-14/root/usr/lib64/pkgconfig

# Ninja 1.12 (Apache-2.0): the packaged 1.8 refuses Blender's build.ninja ("multiple outputs aren't
# (yet?) supported by depslog").
RUN dnf -y install unzip && dnf clean all && \
    wget -q https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip && \
    unzip -o ninja-linux.zip -d /usr/local/bin && rm -f ninja-linux.zip && /usr/local/bin/ninja --version
