# GPU まわりの調査メモ(NoGraphicsAPI・NVIDIA 専用化・OptiX と DLSS・SER)

2026-09-29 時点の調査です。**調査だけで、コードは変更していません。** この環境には、ビルド用ライブラリも GPU もないので、
性能に関する記述は、コードの読解と公開情報に基づく見立てで、**実測はしていません**。

## 目次

1. NoGraphicsAPI は Falcon Engine の役に立つか
2. 汎用を捨てて NVIDIA 専用にしたら、何が変わるか
3. CUDA から OptiX にすると、DLSS は変わるか
4. RTX 3080 で SER は使えるか
5. 実測の手順(Nsight)
6. まとめ

## 1. NoGraphicsAPI は Falcon Engine の役に立つか

**結論: 直接の採用は勧めません。** 実利は小さく、導入コストが非常に大きいです。

### NoGraphicsAPI とは

- [sebbbi/NoGraphicsAPI](https://github.com/sebbbi/NoGraphicsAPI): Sebastian Aaltonen 氏の「No Graphics API」の設計を実装した、**実験的な**最小構成の API。C++20、シェーダは Slang、MIT ライセンス。
- 設計の要点: 64bit の GPU ポインタ、bindless(ディスクリプタヒープ)、描画ごとに 1 つのルート引数、バリアは依存関係の記述(画像レイアウトを管理しない)、PSO の組み合わせ爆発の回避。
- バックエンド: Vulkan 1.4 と Metal 4。
  - Vulkan は最新の拡張が必須: `VK_EXT_descriptor_heap`、`VK_KHR_device_address_commands`、`VK_EXT_mesh_shader`、`VK_KHR_shader_untyped_pointers`。
  - 対応 GPU: NVIDIA Turing 以降、AMD RDNA 2 以降(Windows の RDNA 2 は不可の場合あり)。Pascal(GTX 10)と Intel Arc は非対応。Linux はヘッドレスのみ。
- README には、CUDA との相互運用、動画デコード、レイトレーシングの記述がありません。

### Falcon Engine の GPU まわり(コードから確認)

| 領域 | 使っているもの | NoGraphicsAPI との関係 |
|---|---|---|
| レンダリング(Cycles) | CUDA / OptiX の計算 | 無関係(描画 API を使わない) |
| DLSS | NGX の CUDA 経路 | 無関係 |
| 書き出し・デコード | FFmpeg の NVENC / NVDEC | 無関係 |
| 画面表示と VSE の GPU プレビュー | Blender の GPU モジュール(既定は OpenGL、Vulkan も選べる) | 唯一の接点 |

- Blender の Vulkan バックエンドは約 1.5 万行。Vulkan 1.2、デスクリプタセット、自動追跡のレンダーグラフ(バリア管理)。GLSL のシェーダは 400 本以上。
- Falcon の GPU モジュールへの変更は、メモリ不足時の対処の 22 行だけです。
- NVDEC → CUDA → GL / Vulkan の映像面の直送は、Falcon が独自に実装済みです。

### 評価

| 観点 | 評価 |
|---|---|
| Cycles、DLSS、エンコード・デコード | 効かない(グラフィックス API の話で、CUDA・NGX・FFmpeg は別の層) |
| Blender の GPU モジュールの置き換え | 非現実的(バックエンドの書き直しと、400 本超のシェーダの移植。上流との乖離。Falcon は 5.2.2 ベースと 5.3 alpha ベースの 2 本あるので、負担は 2 倍) |
| 動作環境 | 悪化(最新拡張が前提で、古い GPU が使えなくなる) |
| VSE の GPU プレビューだけ | 小さい(律速はデコードと転送で、Falcon は直送済み。描画 API を変えても動かない) |

### 参考になる考え方(導入はしない)

- bindless で、1 回の描画で複数レイヤーを合成する考え方は、重なった動画の合成に効く。既存の GPU モジュールのテクスチャ配列でも、近い形にできる。
- PSO の爆発は、シェーダの初回コンパイルによるカクつきの問題。Blender の Vulkan は、それに効く拡張(graphics pipeline library)を、すでに使っている。

### 再検討する条件

1. 上流の Blender が Vulkan を既定にして、新しい拡張へ寄せた時(Falcon は 5.3 ベースへの追従で、恩恵を受けられる)。
2. Blender とは別に、小さな GPU ツール(専用の動画プレビューなど)を作る時(対象は Turing 以降の GPU に限られる)。

### 注意点

- 作者の記事本体は、ネットワーク制限で読めませんでした。README と検索結果で確認した内容です。
- README の取得結果に「production-ready」とありましたが、README にその記載はなく、検索結果は「experimental」としています。実験的なものとして扱うのが妥当です。

## 2. 汎用を捨てて NVIDIA 専用にしたら、何が変わるか

**変わります。ただし、NoGraphicsAPI の採否は、ほぼ変わりません。** 効くのは別の場所です。

### 変わらないこと

- 最大の壁は、Blender の GPU モジュール(バックエンド約 1.5 万行 + 400 本超のシェーダ)の置き換えと、上流との乖離です。NVIDIA 専用にしても、消えません。
- 消えるのは「対応 GPU の壁」だけです(NVIDIA は Turing 以降で、必要な拡張に対応できる。Pascal 以前は切り捨て)。

### 変わること(効きそうな順)

1. **Cycles の保守負担が減る。** 非 NVIDIA バックエンドの規模(コードから計測):

   | バックエンド | 行数 |
   |---|---|
   | HIP | 約 2.7k |
   | HIP-RT | 約 2.8k |
   | Metal | 約 7.7k |
   | oneAPI | 約 4.3k |
   | **合計(非 NVIDIA)** | **約 1.75 万行** |
   | (参考)CUDA + OptiX | 約 7.2k |

   Falcon 独自の機能は CUDA 前提で書かれていて、バグ修正でも HIP や Metal への互換対応が必要でした。
2. **OptiX / CUDA 固有の高速化を、前提にできる。** OptiX の hit object API はすでに使っていますが、`optixReorder`(SER)の使用は見当たりません。
   ほかに、アルファテスト形状向けの OMM、Tensor コアを使った処理などがあります。SDK のバージョンと GPU 世代に依存し、効果は**未確認**です。
3. **動画の経路を、NVIDIA 前提で単純にできる。** NVENC / NVDEC に固定できるので、FFmpeg 側のハードウェア別の分岐が要らない。
   現状、VSE の合成は CPU で、GPU はプレビュー表示だけ。デコード → GPU で合成 → NVENC を GPU 内で完結できれば、書き出しの往復が減る(大きな変更)。
4. **DLSS まわり**は、RTX 前提にできるので、非対応 GPU 向けの分岐が要らなくなる。
5. **表示側**は、CUDA と GL / Vulkan の相互運用を、NVIDIA 前提で深くできる(すでに CUDA → GL の直送はある)。

### 失うもの

- AMD、Intel、Apple の GPU を使うユーザー(macOS は、README ですでに非対応)。
- 上流(Blender 5.3 以降)への追従。バックエンドを**削除**すると、マージで衝突が増えるので、CMake で無効化して非対応と明記するのが安全です。

### 推奨

1. 非 NVIDIA バックエンドを、CMake で無効化して非対応と明記する。
2. Cycles の OptiX 固有の最適化を、実測で試す。
3. 動画の GPU 完結パイプラインは、規模が大きいので後回し。

## 3. CUDA から OptiX にすると、DLSS は変わるか

**DLSS 自体は変わりません。** 変わるのは、レンダリング速度です。

- Falcon の DLSS は、デバイスの種類を CUDA と OptiX の**両方**受け付けます(`is_device_supported`)。
  OptiX のデバイスは CUDA のデバイスを継承していて(`class OptiXDevice : public CUDADevice`)、DLSS は CUDA のコンテキスト上で動きます。DLSS の処理も画質も、同じです。
- OptiX は RT コア(3080 は第 2 世代)で交差判定を行うので、一般に CUDA より速い。DLSS を使うと 1 フレームあたりの spp を減らせるので、速度差が 1 フレームの時間に効く。実際の差は、シーン次第で、**未測定**です。
- OptiX の AI デノイザーも選べるようになるが、DLSS を選べば使わない。
- 「今は CUDA」なのは、多くの場合、Preferences の Compute Device が CUDA になっているだけです。リリース設定では、OptiX はすでに ON です(`blender_release.cmake`)。ビルドには OptiX SDK(`OPTIX_ROOT_DIR`)が必要です。
- ビューポートも、OptiX のままにする設定が既定です(CUDA への降格は、`FALCON_VIEWPORT_CUDA=1` を付けた時だけ)。

### 注意点

- Falcon の光子・ライトトレース・SHARC が、OptiX でも同じ絵になるかは、**確認できていません**。コードは、OptiX を意識した経路(レイトレース用のカーネルを通す形)を持っています。
- 試すなら、**同じシーンで CUDA と OptiX を比べて、絵と時間を見てください。** 絵が違う場合は、不具合の可能性があります。

## 4. RTX 3080 で SER は使えるか

**3080(Ampere)では、SER の効果は期待できません。** 「動かない」のではなく、「動くが何も起きない」です。

- OptiX 8.0 以降の SER の API は、レイトレーシング対応の GPU なら呼べる。ただし、実際に並べ替えるのは Ada(RTX 40)以降だけ。それ以前の世代では、`optixReorder` は何もしない(no-op)呼び出しになる。
- したがって、3080 で測っても、効果があるかどうかが分かりません。**3080 を基準にするなら、SER は候補から外してよい。**

### Cycles には、すでにソフト版の SER 的な仕組みがある

- GPU で、SHADE_SURFACE と SHADE_SURFACE_RAYTRACE のカーネルを実行する前に、待機中のパスを**シェーダごとに並べ替えている**(`SORT_BUCKET_PASS`、`SORT_WRITE_PASS`、`SORTED_PATHS_ARRAY`)。
- シェーダ数が 300 未満なら、メモリの局所性のために、パーティション分割もしている(`num_sort_partitions`)。
- 3080 でも、この仕組みはそのまま働いている。Cycles は、交差とシェーディングを別カーネルに分ける方式なので、SER が狙う効果の一部を、すでに得ている。SER を足しても、上乗せの効果は限定的な可能性が高い。

### 3080 で試せること(効果が見込める順)

1. **まず律速を測る**(次の「実測の手順」)。交差判定(RT コア)が大半なら、分岐の最適化は効かない。
2. **OptiX に切り替える。** RT コアを使うので、多くのシーンで、分岐の最適化より大きな効果が出る。
3. **Falcon 特有の分岐を減らす。** 光子パス、ライトトレース、SHARC、ガラスの反射・屈折の分岐は、同じシェーダの中の分岐なので、シェーダ単位のソートでは分けられない。ガラスは、`docs/dlss-glass-guides.md` の「経路の分岐」案と組み合わせると効く。
4. **ワープ内の再グルーピング**(`__match_any_sync`)。CUDA の機能で、3080 でも動く。ただし、Cycles がソート済みなので、効果は小さい見込みで、実装は大きめ。

### SER 自体を入れるなら

- `optixReorder` を、ノブ付き・既定オフで用意する形になる。3080 では no-op で検証できないので、RTX 40 / 50 のテスターが要る。
- OptiX の SER は、1 つのカーネルの中で「交差 → 並べ替え → シェーディング」を行う前提。Cycles の「交差カーネルとシェーディングカーネルを分ける」設計に、そのまま入れられず、改造が大きくなる。

## 5. 実測の手順(Nsight)

「律速は交差か、シェーディングか」「シェーディングの分岐は本当に重いか」を、3080 で確認する手順です。

### 準備

- NVIDIA Nsight Systems と Nsight Compute(どちらも無料)。
- 測るシーンは、遅さを感じる代表的なもの(車のシーンなど)。同じシーン・同じ spp・同じシードで測る。
- コマンドラインの 1 フレームのレンダー(`blender -b scene.blend -f 1`)で測るのが安定します。DLSS を使う設定なら、環境変数(`FALCON_DLSS_*`)も、実際の使い方と同じにします。

### 手順 A: 時間の内訳を見る(Nsight Systems)

1. `nsys profile -o glass_report --trace=cuda blender -b scene.blend -f 1`
2. 出来たレポートを Nsight Systems で開き、「CUDA GPU Kernel Summary」を見る。
3. カーネルの名前は、CUDA デバイスでは `kernel_gpu_` + カーネル名です(例: `kernel_gpu_integrator_intersect_closest`、`kernel_gpu_integrator_shade_surface`、`kernel_gpu_integrator_sort_bucket_pass`)。
   OptiX デバイスでは、交差が `optixLaunch` として現れ、名前が異なる場合があります。
4. 見るところ:
   - 交差(`intersect_*` / OptiX の launch)の合計時間と、`shade_surface` の合計時間の比。
   - ソート(`sort_*`)に、どれだけ時間を使っているか。
   - DLSS の前処理・評価(Falcon 独自のカーネル)の時間。

### 手順 B: シェーディングの分岐を見る(Nsight Compute)

1. 対象のカーネルだけを、少数回測る。
   `ncu --kernel-name regex:integrator_shade_surface -c 5 --set full -o shade_report blender -b scene.blend -f 1`
2. 見る指標(名前は Nsight のバージョンで異なる場合があるので、`ncu --query-metrics` で確認する):
   - `smsp__thread_inst_executed_per_inst_executed.ratio`: 1 命令あたりの実行スレッド数(最大 32)。これを 32 で割ったものが、**ワープの実行効率**。
   - `smsp__sass_average_branch_targets_threads_uniform.pct`: 分岐で、ワープ内のスレッドが同じ向きに進んだ割合。
   - `sm__warps_active.avg.pct_of_peak_sustained_active`: 実際の占有率。
3. 結果の読み方:

   | 結果 | 意味 | 次の手 |
   |---|---|---|
   | 交差の時間が大半 | 分岐の最適化は効かない | OptiX に切り替える(RT コア)。BVH や交差カーネルの工夫 |
   | シェーディングが大半で、ワープの実行効率が高い(約 70% 以上) | 分岐は問題ではない | メモリ帯域やレジスタ数を疑う |
   | シェーディングが大半で、ワープの実行効率が低い(約 50% 未満) | 分岐が重い | Falcon 特有の分岐の削減、ソートのキーの見直し |
   | ソートの時間が大きい | ソートのコストが効果を上回っている可能性 | パーティションの調整 |

### 記録表

| 項目 | 値 |
|---|---|
| GPU / ドライバ | |
| デバイス(CUDA / OptiX) | |
| シーン / spp | |
| 交差の合計時間(ms) | |
| `shade_surface` の合計時間(ms) | |
| ソートの合計時間(ms) | |
| `shade_surface` のワープの実行効率(%) | |
| メモ | |

## 6. まとめ

| テーマ | 結論 |
|---|---|
| NoGraphicsAPI | 採用しない。考え方は参考になるが、Blender の GPU モジュールの置き換えは非現実的 |
| NVIDIA 専用化 | 効くのは Cycles の保守(約 1.75 万行の非 NVIDIA コード)、OptiX 固有の最適化、動画の GPU 完結。NoGraphicsAPI の採否は変わらない。バックエンドは削除せず、CMake で無効化 |
| CUDA → OptiX | DLSS は変わらない。レンダリング速度が、RT コアで上がる見込み。光子・LT・SHARC が、同じ絵になるかは要確認 |
| SER(3080) | 効果なし(no-op)。3080 では追わない。Cycles には、すでにシェーダごとのソートがある |
| 次の一手 | Nsight で律速を測る → OptiX に切り替える → Falcon 特有の分岐を減らす |

## 出典

- [sebbbi/NoGraphicsAPI (GitHub)](https://github.com/sebbbi/NoGraphicsAPI)
- [No Graphics API — Sebastian Aaltonen](https://www.sebastianaaltonen.com/blog/no-graphics-api)(本文は未取得。検索結果の要約を参照)
- [sebbbi/NoGraphicsAPI — The Daily Commit](https://thedailycommit.in/story/2026-09-06/03-github-sebbbi-nographicsapi)
- [Releases · NVIDIA/optix-dev](https://github.com/NVIDIA/optix-dev/releases)
- [NVIDIA OptiX 8.1 - Programming Guide](https://raytracing-docs.nvidia.com/optix8/guide/index.html)
- [[OptiX 8.0] OPTIX_DEVICE_PROPERTY_SHADER_EXECUTION_REORDERING returns 0 - NVIDIA Developer Forums](https://forums.developer.nvidia.com/t/optix-8-0-optix-device-property-shader-execution-reordering-returns-0/263025)
