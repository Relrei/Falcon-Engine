日本語 | [English](../translations/en/docs/BUILD.md)

# Falcon Engine v0.7.1 のビルド手順 (Linux)

Falcon Engine はソースコードのみを配布しています。ご自分の PC でビルドしてください。
NVIDIA の部品（CUDA・OptiX・DLSS）はこのリポジトリには一切含まれておらず、各自が NVIDIA のサイトでライセンスに同意して入手します。

[Falcon Launcher](https://github.com/Relrei/Falcon-Launcher) の「建てる準備」ページを使えば、必要な物の確認や、ダウンロードした NVIDIA パッケージのドラッグ＆ドロップ配置、ソース取得からビルドまでを画面上で行えます。以下は手動でビルドする場合の手順です。

## 1. 要る物

| 項目 | バージョン | 入手先 |
|---|---|---|
| NVIDIA GPU | GeForce RTX 20 シリーズ以上 | |
| NVIDIA ドライバー | 580 以上 | https://www.nvidia.com/Download/index.aspx （またはお使いの Linux ディストリビューションのパッケージ） |
| Git, Git LFS | 最近の版 | ディストリビューションのパッケージ（`git`, `git-lfs`） |
| CMake | 3.10 以上 | ディストリビューションのパッケージ |
| C++ コンパイラー | GCC 14.0 以上（または Clang 17.0 以上） | `base-devel` (Arch) / `build-essential gcc-14 g++-14` (Ubuntu 24.04) / `gcc-c++ make` (Fedora) |
| Ninja（任意） | 任意 | `ninja` / `ninja-build` |
| CUDA Toolkit | 12.8 以上 | https://developer.nvidia.com/cuda-downloads |
| OptiX SDK | 9.0 | https://developer.nvidia.com/designworks/optix/download （NVIDIA 開発者アカウントが必要） |
| DLSS SDK（任意） | 310.x | https://github.com/NVIDIA/DLSS — ビルド時には `include` フォルダのみ使います |
| 空き容量 | 約 20 GB 以上 | ソース、Blender の事前ビルド済みライブラリ、ビルド先フォルダ、コンパイラキャッシュ |

NVIDIA の各ダウンロードにはそれぞれのライセンスがあり、ダウンロード・導入時に同意します。

## 2. ソースの取得

```sh
git clone https://github.com/Relrei/Falcon-Engine.git falcon-engine
cd falcon-engine
git checkout Falcon-Engine-v0.7.1
make update          # Blender の事前ビルド済みライブラリ (lib/linux_x64) を Git LFS で取得します
```

## 3. 設定 (CMake)

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

- 標準の `gcc` が 14.0 未満のディストリビューション（Ubuntu 24.04 など）では、`gcc-14 g++-14` を入れた上で `-DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14` を追加してください。
- `CYCLES_CUDA_BINARIES_ARCH`: お使いの GPU の世代だけを指定するとビルド時間を短縮できます —
  `sm_75` RTX 20, `sm_86` RTX 30, `sm_89` RTX 40, `sm_120` RTX 50。
- DLSS SDK を使わない場合は DLSS の 2 つのオプションを外してください（Neural Rendering モード以外の全機能が使えます）。
- `-DWITH_FALCON_OFFICIAL_CYCLES=ON` を付けると、Cycles-expansion と並べて公式の Cycles もビルドします（既定は OFF）。

## 4. ビルド

```sh
ninja install        # 初回ビルド: 6 コアで約 1〜2 時間。2 回目以降の更新では変更箇所のみ再ビルドされます
```

`ccache`（`-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`）を使うと、更新後の再ビルドが大幅に速くなります。

## 5. 起動

```sh
./bin/blender
```

Neural Rendering モードを使う場合は、`Preferences → Add-ons → Cycles-expansion → DLSS Runtime Folder` に `libnvidia-ngx-dlssd.so.*` が入ったフォルダ（DLSS SDK の `lib/Linux_x86_64/rel`）を指定して再起動してください。
Falcon は実行時にそのフォルダまたは NVIDIA ドライバーから DLSS を読み込みます。フォルダのパスに日本語などの非 ASCII 文字が含まれていても構いません（NVIDIA NGX が非 ASCII パスを直接読めないため、Falcon が `~/.cache/falcon-dlss` の英数字パスへ自動コピーしてから読み込みます）。

## 6. 更新

```sh
cd falcon-engine
git fetch --tags && git checkout <新しいタグ> && make update
cd ../build && ninja install
```

変更されたファイルだけが再ビルドされます。前のビルドに戻せるようにしたい場合は、前の `bin` フォルダを残しておいてください（Falcon Launcher はビルドごとにフォルダを分けて残します）。

## Windows

現在は未対応です。
