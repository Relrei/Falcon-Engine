# Falcon Engine v0.7.1 のビルド案内（Linux）

Falcon Engine はソースコードのみで配布しています。ビルド済みのバイナリは配っていないため、ご自分の PC でビルドしてください。
NVIDIA の部品（CUDA・OptiX・DLSS）はこのリポジトリには一切含まれていません。各自が NVIDIA の公式サイトでライセンスに同意したうえで入手します。

[Falcon Launcher](https://github.com/Relrei/Falcon-Launcher) の「準備」ページを使うと、必要な環境のチェックや、ダウンロードした NVIDIA パッケージのドラッグ＆ドロップ配置、ソース取得からビルドまでを画面上でまとめて行えます。
以下は手動でビルドする場合の案内です。

## 1. 必要なもの

| 項目 | バージョン | 入手先 |
|---|---|---|
| NVIDIA GPU | GeForce RTX 20 シリーズ以降 | — |
| NVIDIA ドライバー | 580 以降 | https://www.nvidia.com/Download/index.aspx （またはお使いのディストリビューションのパッケージ） |
| Git・Git LFS | 最近のバージョン | ディストリビューションのパッケージ（`git`、`git-lfs`） |
| CMake | 3.10 以降 | ディストリビューションのパッケージ |
| C++ コンパイラー | GCC 14.0 以上（または Clang 17.0 以上） | Arch は `base-devel`、Ubuntu 24.04 等は `build-essential` と `gcc-14` / `g++-14`、Fedora は `gcc-c++` と `make` |
| Ninja（任意） | 指定なし | `ninja` / `ninja-build` |
| CUDA Toolkit | 12.8 以降 | https://developer.nvidia.com/cuda-downloads |
| OptiX SDK | 9.0 | https://developer.nvidia.com/designworks/optix/download （NVIDIA 開発者アカウントが必要） |
| DLSS SDK（任意） | 310.x | https://github.com/NVIDIA/DLSS （ビルド時は `include` フォルダのみ使用） |
| 空き容量 | 約 20 GB 以上 | ソース、Blender の事前ビルド済みライブラリ、ビルド先フォルダ、コンパイラーキャッシュ |

各 NVIDIA パッケージには個別のライセンスが付属しており、ダウンロード・導入時にご自分で同意する必要があります。

## 2. ビルドの流れ

1. **ソースコードと事前ビルド済みライブラリの取得**
   - GitHub（`Relrei/Falcon-Engine`）からリポジトリを取得し、リリースタグ（例: `Falcon-Engine-v0.7.1`）へ切り替えます。
   - その後、Blender 標準の更新手順（`make update`）で事前ビルド済みライブラリ（`lib/linux_x64`、Git LFS 経由）を取得します。

2. **CMake でビルド構成を設定**
   - ソースとは別のビルド用フォルダを用意し、Ninja などのジェネレーターを指定して CMake の構成を行います。
   - 構成時に指定する主な設定項目は下の「3. 主な CMake 設定項目」を参照してください。

3. **ビルドとインストール**
   - ビルドツール（Ninja など）で `install` ターゲットを実行します。
   - 初回ビルドは 6 コア環境で約 1〜2 時間かかります。2 回目以降は変更された箇所だけが再ビルドされます。
   - コンパイラーキャッシュ（`ccache`）を CMake のランチャーに指定しておくと、更新時の再ビルド時間を短縮できます。

4. **起動と DLSS ランタイムの指定**
   - ビルド先フォルダ内の `bin/blender` から起動します。
   - Neural Rendering モードを使う場合は、Blender の `Preferences → Add-ons → Cycles-expansion → DLSS Runtime Folder` に、`libnvidia-ngx-dlssd.so.*`（DLSS SDK 内の `lib/Linux_x86_64/rel` にあります）を置いたフォルダを指定し、Blender を再起動してください。
   - 実行時に指定フォルダまたは NVIDIA ドライバーから DLSS ランタイムを読み込みます。
   - フォルダのパスに日本語などの非 ASCII 文字が含まれていても構いません。NVIDIA NGX が非 ASCII パスから直接読み込めないため、Falcon 側で自動的に ASCII のみのキャッシュフォルダ（`~/.cache/falcon-dlss`）へコピーしてから読み込みます。

5. **新しいバージョンへの更新**
   - ソースフォルダで最新のタグを取得して切り替え、事前ビルド済みライブラリを更新したあと、同じビルド先フォルダで再度ビルド・インストールを行います。
   - 変更分だけが再ビルドされます。前のバージョンへ戻せるようにしておきたい場合は、更新前の `bin` フォルダを別の場所へ残しておいてください（Falcon Launcher ではバージョンごとに別フォルダへ配置されます）。

## 3. 主な CMake 設定項目

CMake 実行時に渡す主なオプションの案内です。環境に合わせてパスや GPU アーキテクチャを置き換えて指定してください。

| 設定項目 | 役割・指定の目安 |
|---|---|
| 初期キャッシュ（`-C`） | ソース内の `build_files/cmake/config/blender_release.cmake` を指定します。 |
| `CMAKE_BUILD_TYPE` | `Release` を指定します。 |
| `WITH_CYCLES_CUDA_BINARIES` | CUDA カーネルをビルドするため `ON` にします。 |
| `CYCLES_CUDA_BINARIES_ARCH` | お使いの GPU 世代だけを指定するとビルド時間を短縮できます（RTX 20: `sm_75`、RTX 30: `sm_86`、RTX 40: `sm_89`、RTX 50: `sm_120`）。 |
| `OPTIX_ROOT_DIR` | 展開した OptiX SDK 9.0 のフォルダパスを指定します。 |
| `WITH_DLSS` / `DLSS_SDK_ROOT` | DLSS を使う場合は `WITH_DLSS=ON` にし、`DLSS_SDK_ROOT` へ DLSS SDK のフォルダパスを指定します。DLSS SDK がない場合はこれらを省いて構成できます（Neural Rendering 以外の機能はすべて動作します）。 |
| `WITH_CYCLES_DEVICE_HIP` / `WITH_CYCLES_HIP_BINARIES` / `WITH_CYCLES_DEVICE_HIPRT` | AMD GPU 向けビルドを省く場合はすべて `OFF` にします。 |
| `WITH_CYCLES_DEVICE_ONEAPI` / `WITH_CYCLES_ONEAPI_BINARIES` | Intel GPU 向けビルドを省く場合はすべて `OFF` にします。 |
| `WITH_FALCON_OFFICIAL_CYCLES` | `ON` にすると、Cycles-expansion と並べて公式の Cycles もビルドできます（既定は `OFF`）。 |
| `CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER` | OS 標準の GCC が 14 未満の場合（Ubuntu 24.04 など）に、`gcc-14` / `g++-14` を指定します。 |
| `CMAKE_C_COMPILER_LAUNCHER` / `CMAKE_CXX_COMPILER_LAUNCHER` | `ccache` を指定すると再ビルドが速くなります（任意）。 |

## 4. Windows について

現時点では未対応です。
