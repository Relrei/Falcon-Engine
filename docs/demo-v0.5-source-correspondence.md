# v0.5-demo のバイナリとソースの対応

配布物 `FalconEngine-v0.5-demo-5.2.2-20260928-c9cb877032d-linux-x64.tar.xz`(sha256 は Release の `SHA256SUMS`)が、このリポジトリのどのソースに対応するかの記録です。

## 対応

- バイナリの build hash(`blender --version`)は `c9cb877032d3` です。これは作者の作業用リポジトリのコミット `c9cb877032d` で、**このリポジトリにはありません**。
- このリポジトリの `v0.5-demo` タグ(`25cef50`)は、そのコミットから「公開しない物を除き、コメントの表現を整えて」書き出したものです。そのためコミット ID は一致しません。
- 下の内訳のとおり、実行されるコードは同じです(2026-09-29 に両方のツリーを突き合わせて確かめました)。

## 差の全内訳

両ツリーに共通のファイルは 13,346 本で、そのうち内容が違うのは 404 本です。

| 種類 | 本数 | 中身 |
|---|---|---|
| Git LFS | 362 | 作業用リポジトリはポインタ、このリポジトリは実体を置いています。実体の sha256 とポインタの `oid` が**全数一致**しました |
| `README.md` `.gitattributes` | 2 | このリポジトリ用の版です |
| Falcor の条件付きビルド記述 | 4 | `CMakeLists.txt`・`intern/CMakeLists.txt`・`source/blender/python/intern/CMakeLists.txt`・`source/blender/python/intern/bpy_interface.cc` の `WITH_FALCOR` の記述を除いています。Falcor(`intern/falcor`)はこのリリースに含まれず、バイナリも `WITH_FALCOR` なしで建てているので、実行コードには影響しません |
| コメントのみ | 36 | 作業メモへの参照を外す、一人称の表現を「作者」に直す、などの言い換えです。コメント・空行以外の行に差はありません |

## このリポジトリに無いファイル

作業用リポジトリだけにあるファイルが 8,325 本、このリポジトリだけにあるファイルが 2 本(`README.blender.md`・`docs/vse-2026-09-23-updates.md`)です。

- `tests/files`(テスト用データ)6,897 本
- `intern/falcor`(Falcor 一式・このリリースに含まれません)1,383 本
- その他 45 本: 開発用の検査スクリプト(`tests/python` など)・作業メモ・`.gitea` / `.github` の設定・資金提供の案内のほか、**`scripts/addons_core/mcp`(9 本)**

### 同梱している MCP アドオンについて

配布物には、Blender Lab の MCP アドオン(v1.0.0・`GPL-3.0-or-later`・Copyright Blender Authors)が `scripts/addons_core/mcp` として入っています。Python のソースそのままで、配布物の中でソースを読めます。このリポジトリのツリーには入れていません。

## 確かめ方

作業用リポジトリの `c9cb877032d` とこのリポジトリの `25cef50` のそれぞれで `git ls-tree -r` の一覧を取り、パスとブロブ ID を突き合わせました。内容が違うパスについて、LFS のポインタは `oid sha256:` を実体の sha256 と比べ、テキストは行単位の差分を取っています。
