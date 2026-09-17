# Falcon Engine

Blender 5.2.1 をもとにしたカスタムビルドです。
このリポジトリには、Falcon が Blender 本体に加えた変更のうち、**公開できるソースの一部だけ**を置いています。

- **ここからはビルドできません。** Blender 本体の残りと、ビルドの仕組み(CMake)は含みません。
- 問題と進捗は [Issues](../../issues) で管理しています。

## 構成

| 区分 | パーツ | このリポジトリのソース |
|---|---|---|
| 本体カスタム | VSE(動画編集の GPU 書き出し・高速書き出し) | 一部あり |
| 本体カスタム | F-Cycles(コースティクス・光子) | 一部あり |
| 本体カスタム | スカルプト GPU・ビューポートのメモリ退避・MaterialX 出力 | 一部あり |
| 本体カスタム | RIFE(レンダー後のコマ補間の呼び出し口) | 一部あり |
| 本体カスタム | DLSS RR | 含まない |
| 本体カスタム | GPU SIM・MTS・Nao・RSY・FSR | 未合流 |
| 分離ビルド | Dragon・Kajigata・Yotsuba4・Rapid | 含まない(進捗だけ Issues で管理) |

## 含まないもの

- 分離ビルド(Dragon・Kajigata・Yotsuba4・Rapid)
- NVIDIA DLSS と SDK に関わるもの
- ビルドの仕組み・試験用のデータと道具

## ライセンス

Blender 由来のコードは GNU GPL([COPYING](COPYING)・[doc/license](doc/license))、Cycles は Apache License 2.0 です。各ファイル先頭の SPDX 表記に従います。

---

*English:* Falcon Engine is a custom build based on Blender 5.2.1. This repository contains only part of the source changes Falcon makes to Blender. It is not buildable on its own (the rest of Blender and the build system are not included). Issues are used to track bugs and progress, including the separately built renderers whose source is not published here.
