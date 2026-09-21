# Falcon Engine

Blender 5.2.2 をベースに、個人で手を入れているカスタムビルドです。
自分が動画編集やレンダリングで「ここが遅い」「ここが不便」と感じた所を、少しずつ直しています。
ビルドは Blender 本家と同じ手順でできます(本家の README は [README.blender.md](README.blender.md) に置いてあります)。

- 不具合や進み具合は [Issues](../../issues) に書いています。気になる所があれば気軽にどうぞ。

## 入れたもの

- VSE(動画編集)の書き出しの高速化: 切っただけの区間は復号も符号化もせずに通す(fast path)・GPU での符号化(NVENC)・GPU で開けない時は CPU の符号化へ落とす
- VSE 本体の直し(プロキシ・キャッシュ・書き出しの末尾のコマなど)・書き出しで何が効いたかを 1 行で出す・書き出しの後にメモリを返す
- VSE 連携アドオン(レンダーエンジン「VSE」と書き出しのプリセット)
- F-Cycles: Cycles に光子(フォトン)・SHARC・分散を足し、コースティクスをチェック 1 つで出せるようにしたもの
- DLSS の受け口とプラグインフォルダ(下の「DLSS」)
- VSE のチャンネル: 段を 10 まで増やせるように・段の並びを上下逆にできるように(`FALCON_VSE_CHANNELS` / `FALCON_VSE_FLIP_CHANNELS`)

## ビルド(Linux x64)

★**事前ビルド済みライブラリ(`lib/`)はこのリポジトリに入れていません。**容量が大きいのと、
本家が配っている物をそのまま使えるからです。下の 2 で取ってきてください(git-lfs が要ります)。

```sh
# 1) この木を取る
git clone https://github.com/Relrei/Falcon-Engine.git
cd Falcon-Engine

# 2) 本家の事前ビルド済みライブラリ(git-lfs が要ります)
make update                # 本家と同じ入口。5.2 用の枝を選んで lib/linux_x64 へ入れてくれます

# 使えない時は手で取っても同じです。
# ★枝(-b blender-v5.2-release)を必ず付けてください。付けないと 5.3 開発版の物が来てビルドが通りません
mkdir -p lib
git clone --depth 1 -b blender-v5.2-release \
    https://projects.blender.org/blender/lib-linux_x64.git lib/linux_x64

# 3) ビルドする(どちらか片方で大丈夫です)
make release               # 本家と同じ入口。出来上がりは ../build_linux_release/bin/blender

# もしくは cmake を直に叩く
# ★`-G Ninja` を付けてください。付けないと Makefile が出来るので次の ninja が動きません
cmake -G Ninja -S . -B ../build-falcon \
      -C build_files/cmake/config/blender_release.cmake \
      -DLIBDIR=$PWD/lib/linux_x64 -DCMAKE_BUILD_TYPE=Release
ninja -C ../build-falcon install     # 出来上がりは ../build-falcon/bin/blender
```

★**Intel の GPU(oneAPI)のカーネルで止まる時**: 既定の設定だとこれもビルドします。その時に使う
dpcpp が**システムの C++ ヘッダ**を探すので、最小限のビルド環境(コンテナなど)では
`fatal error: 'iostream' file not found` で止まります。Intel の GPU を使わないなら切ってしまって大丈夫です。

```sh
cmake -B ../build-falcon -DWITH_CYCLES_DEVICE_ONEAPI=OFF -DWITH_CYCLES_ONEAPI_BINARIES=OFF
```

| 置き場 | 何か |
|---|---|
| `lib/linux_x64` | 本家の事前ビルド済みライブラリ(上の 2 で取る)|
| `../build_linux_release/bin/blender` | `make release` で建てた時の実行ファイル |
| `../build-falcon` | cmake を直に叩いた時の作業場所(ソースの外に置くのが本家のやり方です)|
| `../build-falcon/bin/blender` | そちらで建てた時の実行ファイル |

いまのところ、動作を確かめているのは Linux x64 だけです。
- **Windows**: 対応するつもりです。ビルド自体は本家と同じ手順で通るかもしれませんが、まだ試せていません。
  足した機能は NVIDIA の GPU(CUDA / NVENC / DLSS)と x86-64 の CPU が前提の物が多いので、使えない機能がそれなりに出ると思います。
- **macOS**: 対応しません。足した機能の多くが NVIDIA の GPU と x86-64 の CPU 前提なのと、私が Mac を持っていなくて確かめようがないためです。ごめんなさい。

必要な物と細かい手順は Blender 本家と同じです:
https://developer.blender.org/docs/handbook/building_blender/linux/

- GPU(CUDA)のカーネルは、既定では最初の GPU 描画の時に組み立てます(CUDA Toolkit が要ります。本家と同じです)。
- VSE の GPU 書き出し(NVENC)には、NVENC を有効にした FFmpeg が要ります。本家の事前ビルド済みライブラリの FFmpeg は NVENC を無効にしてあるので、そのままビルドすると CPU の符号化で書き出します。

### DLSS(任意)

- 既定では DLSS 無し(`WITH_DLSS=OFF`)でビルドされます。
- DLSS を使いたい時は、NVIDIA の DLSS SDK(https://github.com/NVIDIA/DLSS)のヘッダを用意して `-DWITH_DLSS=ON -DDLSS_SDK_ROOT=<SDK の場所>` でビルドしてください。
- DLSS の本体(NVIDIA のランタイム `libnvidia-ngx-dlssd.so.<版>`)は、ライセンスの都合でこのリポジトリにも配布物にも入れていません。SDK の `lib/Linux_x86_64/rel/` から取って、札 `falcon_plugin.toml` と一緒にプラグインフォルダへ置いてください。

```
~/.config/blender/5.2/falcon_plugins/dlss/      (または blender の実行ファイルの隣の falcon_plugins/dlss/)
    falcon_plugin.toml
    libnvidia-ngx-dlssd.so.310.7.0
```

```toml
schema_version = "1.0.0"
id = "dlss"
name = "NVIDIA DLSS Ray Reconstruction runtime"
version = "310.7.0"
blender_version_min = "5.2.0"
platforms = ["linux-x64"]
files = ["libnvidia-ngx-dlssd.so.310.7.0"]
```

- 置くと Preferences > Add-ons に「NVIDIA DLSS denoiser」が出ます(既定は無効)。有効にすると、デノイザの一覧に DLSS が出ます。
- 札に書いていないファイルがフォルダにあると、安全のためそのプラグインは繋がずにエラーを出します。`FALCON_PLUGINS=0` で起動すると、プラグインフォルダを読まずに立ち上がります。

## 入れていないもの

- 分離ビルド(Dragon・Kajigata・Yotsuba4・Rapid)
- まだ試している途中で、実用と言える数字が出ていない機能
- NVIDIA DLSS の SDK とランタイム
- 試験用のデータ(`tests/files`)と開発用の道具

## ライセンス

Blender 由来のコードは GNU GPL([COPYING](COPYING)・[doc/license](doc/license))、Cycles は Apache License 2.0 です。各ファイル先頭の SPDX 表記に従います。
NVIDIA と DLSS は NVIDIA Corporation の商標です。

---

*English:* Falcon Engine is my personal custom build of Blender 5.2.2 — I fix the things that feel slow or awkward in my own video editing and rendering. It builds with the same steps as upstream Blender (`make update && make release`); only Linux x64 is verified. Windows is planned (the build may work, but many features are limited because they assume an NVIDIA GPU and an x86-64 CPU); macOS is not supported (I don't have a Mac to test on, sorry). It adds faster VSE export (a pass-through fast path and NVENC encoding, which needs an FFmpeg built with NVENC; the upstream precompiled FFmpeg has it disabled, so a plain build encodes on the CPU), VSE fixes, a VSE bridge add-on, and F-Cycles (Cycles with photon mapping, SHARC and dispersion for caustics). DLSS support is optional (`-DWITH_DLSS=ON -DDLSS_SDK_ROOT=...`); the NVIDIA DLSS SDK and runtime are not included. Place the runtime from the SDK, together with the `falcon_plugin.toml` manifest shown above, in `~/.config/blender/5.2/falcon_plugins/dlss/` (or `falcon_plugins/dlss/` next to the executable); the "NVIDIA DLSS denoiser" add-on then appears in Preferences > Add-ons, and enabling it offers DLSS as a denoiser. The separately built renderers (Dragon, Kajigata, Yotsuba4, Rapid) and features still in testing are not part of this repository. Bugs and progress are tracked in Issues — feel free to drop by.
