# Building Falcon Engine v0.7.0 (Linux)

Falcon Engine is distributed as source only. You build it on your own PC; the NVIDIA parts
(CUDA, OptiX, DLSS) are downloaded by you from NVIDIA, under NVIDIA's own licenses, and are
never part of this repository.

The Falcon Launcher can check all of this for you (Prep page) and takes the downloaded NVIDIA
packages by drag and drop. The steps below are the same thing done by hand.

## 1. What you need

| | Version | Where to get it |
|---|---|---|
| NVIDIA GPU | GeForce RTX 20 series or newer | |
| NVIDIA driver | 580 or newer | https://www.nvidia.com/Download/index.aspx (or your distribution's package) |
| Git, Git LFS | any recent | distribution package (`git`, `git-lfs`) |
| CMake | 3.10 or newer | distribution package |
| C++ compiler | GCC 11 or newer | `base-devel` (Arch) / `build-essential` (Debian, Ubuntu) / `gcc-c++ make` (Fedora) |
| Ninja (optional) | any | `ninja` / `ninja-build` |
| CUDA Toolkit | 12.8 or newer | https://developer.nvidia.com/cuda-downloads |
| OptiX SDK | 9.0 | https://developer.nvidia.com/designworks/optix/download (NVIDIA developer account) |
| DLSS SDK (optional) | 310.x | https://github.com/NVIDIA/DLSS — only the `include` folder is used at build time |
| Disk | about 20 GB free | source, Blender's prebuilt libraries, build folder, compiler cache |

Each NVIDIA download comes with its own license, which you accept when you download and install it.

## 2. Get the source

```sh
git clone https://github.com/Relrei/Falcon-Engine.git falcon-engine
cd falcon-engine
git checkout Falcon-Engine-v0.7.0
make update          # downloads Blender's prebuilt libraries (lib/linux_x64, via Git LFS)
```

## 3. Configure

```sh
mkdir ../build && cd ../build
cmake -G Ninja ../falcon-engine \
  -C ../falcon-engine/build_files/cmake/config/blender_release.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITH_CYCLES_CUDA_BINARIES=ON -DCYCLES_CUDA_BINARIES_ARCH="sm_86" \
  -DOPTIX_ROOT_DIR=/path/to/NVIDIA-OptiX-SDK-9.0.0-linux64-x86_64 \
  -DWITH_DLSS=ON -DDLSS_SDK_ROOT=/path/to/DLSS \
  -DWITH_CYCLES_DEVICE_HIP=OFF -DWITH_CYCLES_HIP_BINARIES=OFF -DWITH_CYCLES_DEVICE_HIPRT=OFF \
  -DWITH_CYCLES_DEVICE_ONEAPI=OFF -DWITH_CYCLES_ONEAPI_BINARIES=OFF
```

- `CYCLES_CUDA_BINARIES_ARCH`: build only for your own GPU to save time —
  `sm_75` RTX 20, `sm_86` RTX 30, `sm_89` RTX 40, `sm_120` RTX 50.
- Without the DLSS SDK, leave out the two DLSS options: everything except Neural Rendering mode still works.
- `-DWITH_FALCON_OFFICIAL_CYCLES=ON` also builds the official Cycles next to Cycles-expansion (off by default).

## 4. Build

```sh
ninja install        # first build: about 1–2 hours on 6 cores; later updates rebuild only what changed
```

Using `ccache` (`-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`) makes
rebuilds after an update much faster.

## 5. Run

```sh
./bin/blender
```

For Neural Rendering mode, point Preferences → Add-ons → Cycles-expansion → **DLSS Runtime Folder** at the
folder holding `libnvidia-ngx-dlssd.so.*` (in the DLSS SDK: `lib/Linux_x86_64/rel`), then restart.
Falcon loads DLSS from there or from the NVIDIA driver at run time. The folder may contain non-ASCII
characters: Falcon then copies the runtime into an ASCII-only cache folder (`~/.cache/falcon-dlss`) first,
because NVIDIA NGX does not load it from such a path.

## 6. Updating

```sh
cd falcon-engine
git fetch --tags && git checkout <new tag> && make update
cd ../build && ninja install
```

Only the changed files are rebuilt. Keep the previous `bin` folder if you want to be able to go back
(the Falcon Launcher installs each version into its own folder for this).

## Windows

Not supported yet.
