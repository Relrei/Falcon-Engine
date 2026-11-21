# Falcon Engine v0.7.2 — 機能と使い方

> 以下に記載している項目名は Blender の画面上に表示されるラベル名です。*（既知の不具合）* と付いている項目は現在対応中の課題です。

Falcon Engine は Blender 5.3 をベースにした個人カスタムビルドです（Blender Foundation とは関係ありません）。
標準の Cycles を **Cycles-expansion**（Falcon の独自機能を加えた Cycles。Bidirectional をオフにして Mode を Expansion にすると標準の Cycles と同様に動作します）へ置き換えています。EEVEE には手を加えていません。
なお、ビルド時に `WITH_FALCON_OFFICIAL_CYCLES=ON` を指定すると、公式の Cycles も並べてビルドできます（[BUILD.md](BUILD.md) を参照）。

## 1. 有効化の手順

1. `Properties → Render → Render Engine` で **Cycles-expansion** を選びます。
2. `Edit → Preferences → Add-ons → Cycles-expansion` で **OptiX**（推奨）または CUDA を選び、使用する GPU にチェックを入れます。
3. `Render → Device` を **GPU Compute** にします。
4. `Render → Mode` で以下のどちらかを選びます。

| モード | 内容 |
|---|---|
| **Expansion**（既定） | Falcon の拡張機能（双方向パストレーシング等）が入った Cycles です。サンプリングとデノイズは通常の Cycles と同様に動作します。 |
| **Neural Rendering** | DLSS Ray Reconstruction を基盤にしたモードです。少ないサンプル数で描画し、ガラス部分へ追加サンプルを割り当て、DLSS のフレーム履歴で画を安定させます。最終レンダー・ビューポートともに常に DLSS でデノイズします。NVIDIA RTX GPU と DLSS ランタイムが必要です（第 6 節を参照）。 |

## 2. コースティクス（Caustics パネル）

`Properties → Render → Caustics` から設定します。

| 設定 | 内容 |
|---|---|
| **Bidirectional** | 双方向パストレーシング（光源側からの光路）を使います。オフの間は、下の Mode・Photon Mapping・Override はグレー表示になり操作できません。 |
| **Device** | レンダリングに使う装置です（**GPU Compute** / **CPU** / **GPU + CPU**）。 |
| **Method** | カメラ側のコースティクスの手法です。**SMS**（Specular Manifold Sampling、既定）は反射・屈折・波紋・分散に対応します。**MNEE** は標準の Cycles と同じ Shadow Caustics です。Bidirectional と併用できます。 |
| **Mode** | 双方向の手法です（下の表）。 |

| Mode | 内容 |
|---|---|
| **Automatic**（既定） | Light Tracing と Photon Mapping を使い、1 サンプル目の結果から寄与がほぼ無い手法を停止します。コースティクスの無いシーンでも速度低下はわずかに抑えられます（テスト環境では 10% 以内）。 |
| **Light Tracing + Photon Mapping** | 常に両方を使います。 |
| **Light Tracing** | 光源側から光路を伸ばし、カメラやカメラ側パスへ接続します。拡散面上のコースティクス（テーブル上のグラスの集光など）や、霧の中の光の筋（ライトシャフト）を描きます。 |
| **Photon Mapping** | 光源からのフォトンをカメラ側パスで集めます。ガラスや水を通して見えるコースティクス（プールの底やボトル内部の集光など）向けです。 |

パネルの中の折りたたみ項目：

| 項目 | 内容 |
|---|---|
| **SMS / MNEE** | Method ごとの詳細設定（反復回数など）と、Filter Glossy・Reflective・Refractive です。 |
| **Photon Mapping → Photons** | 各フレーム開始時のフォトンパスで飛ばす光路数です。増やすほど滑らかで鮮明になりますが、1 本あたり 48 バイトのメモリを使います。既定は 1,000,000 です。 |
| **Photon Mapping → Light Bounces** | 光源側パス（フォトンとライトトレーシング）の最大バウンス数です（既定 8）。 |
| **Override** | フォトンマッピングのコースティクスの見た目を、物理的な正しさより見た目を優先して調整します（Mode が Photon Mapping を使う時のみ）。**Strength**（明るさの倍率）、**Line Boost**（光の集まる筋だけを明るく）、**Sharpness**（筋を細く。ノイズは増えます）、**Glow** と **Glow Radius**（筋をぼかさずに周りへ薄い広がりを足す）、**Dispersion**（マテリアルの分散とは別に、コースティクスを赤・緑・青に分ける）。すべて 0（Strength は 1）が物理的に正しい値です。 |

注意点：
- Automatic と Light Tracing + Photon Mapping では、拡散面上のコースティクスは Light Tracing で描くため、Override が効くのはガラス越しに見えるコースティクスのみです。床などへの集光に Override を効かせたい場合は Mode を Photon Mapping にしてください。
- フォトンマップは、カメラだけが動いている間は作り直さずに使い続けます（物体・ライト・マテリアルが変わると自動で作り直します）。最終レンダーでは 8 サンプルごとにフォトンを打ち直し、集める半径を少しずつ縮めます。
- 光源側パスは霧（ボリューム）も通過するため、スポットライトやプリズムを通した光の筋が霧の中に現れます。不均一な霧はステップ分割で計算します（`Volumes → Steps`、既定 32）。*（既知の不具合：Bidirectional をオンにすると、オフの時と比べて霧が約 25% 暗くレンダリングされます）*
- **Manifold Walks** は未実装です。

### マテリアルごとの設定

- **Principled BSDF → Caustics**：Bidirectional がオフの間は Fast Caustics（FFC）の項目、オンの間は双方向用の **BD Caustics Weight**（そのマテリアルのコースティクスの倍率）を表示します。レンダー側の Bidirectional を切り替えると自動で切り替わります。
- **Fast Caustics**（`Material → Fast Caustics`）：ガラスの影の中に、解析的に計算した擬似的なコースティクス（分散・折り返し線・波紋など）を描きます。物理的な計算ではありませんが軽く、ビューポートと最終レンダー、GPU と CPU のどれでも動作します。
- 専用のノードグループ **Fake Fast Caustics (FFC)** と **Bidirectional Caustics (BD)** も使えます。

## 3. Neural Rendering — 最終レンダー

`Render → Sampling → Render`（Neural Rendering モード時）の設定です。

| 設定 | 内容 |
|---|---|
| **Samples** | 全ピクセルの基本サンプル数です（ノイズしきい値は使いません）。 |
| **Glass** | カメラからの最初の光線がガラスに当たったピクセルのサンプル数です。Samples 以下の値を指定すると無効になります。アップスケーリングありの DLSS 使用時は動作しません。 |
| **Halve Samples Each Frame** | アニメーション向けの設定です。2 フレーム目以降（およびカット切り替え後）は DLSS の履歴を活用し、ガラス以外のピクセルのサンプル数を前フレームの半分（下限 1：8 → 4 → 2 → 1 …）へ減らします。ガラスのピクセルは Glass のサンプル数を維持します。 |
| **Time Limit** | 通常の Cycles と同じ時間制限です。 |

**Model** は DLSS Ray Reconstruction のモデルです（**F**（既定）/ **E** / **D**）。Expansion モードで Denoiser を DLSS にした時と、Neural Rendering モードで、Upscale Mode の下に表示されます。

アニメーションの最終レンダーでは、フレームの間で DLSS の履歴とシーンのデータを引き継ぎ、描き終わると解放します（`Performance → Final Render` の Persistent Data の設定は使いません）。カメラが切り替わった時とフレームが飛んだ時は、履歴を消してから描きます。

**Glass Through Guides**（既定は **Albedo and Normal**）は、ガラス越しに見える奥の面の情報を DLSS へ渡す設定です。ガラスの奥がにじまず鮮明に保たれます。オフにすると通常の Cycles が書き出すガイドのみを使います。

Bidirectional は Neural Rendering と併用できます（コースティクスが DLSS へ渡されます）。
*（既知の不具合・Neural Rendering のみ：分散（Dispersion）のあるガラスやプリズムで光が虹色に分かれ、白い壁など明るい面に当たった場合、虹の周囲に薄い青色のにじみが出ることがあります）*

## 4. Neural Rendering — ビューポート

`Render → Sampling → Viewport`（Neural Rendering モード時）の設定です。

| 設定 | 内容 |
|---|---|
| **Samples per Frame** | ビューポート 1 フレームあたりのサンプル数です（既定 3）。視点を止めている間は Max Samples まで蓄積が続きます。 |
| **While Moving** | 視点を動かしている間の 1 フレームあたりのサンプル数です。 |
| **Glass** | ガラス部分の 1 フレームあたりのサンプル数です。History オフ時やアップスケーリングありの DLSS 使用時は動作しません。 |
| **Max Samples** | 視点静止時に描画を止める上限サンプル数です。 |
| **History → Carry Over Frames** | DLSS の履歴をフレーム間で引き継ぎます。オフにすると毎フレーム履歴を消去するため移動後の残像は出なくなりますが、ノイズやちらつきが増え、Glass 設定も使われなくなります。 |

`Render → Light Paths → Neural Rendering Viewport` では、**Scene Light Paths** がオフ（既定）のとき、ビューポートだけ Falcon が選んだバウンス数（パネルに表示されます）を使用します。最終レンダーでは常にシーン側の Light Paths 設定が使われます。

## 5. その他の追加機能

| 機能 | 場所 | 内容 |
|---|---|---|
| **Unlimited** | `Sampling → Render`（Expansion モード） | 最終レンダーで止めるまでサンプルを足し続けます（Esc で停止しても画は残ります）。デノイズ無効時のみ使えます。 |
| **Light Mixer** | `View Layer → Light Mixer`（Light Groups を使用） | レンダリング後に再レンダリングせず、コンポジター上でライトグループごとの強さと色を変更できます。 |
| **Shadow Caustics** | Light、および `Object → Shadow Caustics` | 標準の Cycles と同じ、屈折物体の影に近似コースティクスを出す機能です（ライト・投射側・受光側の設定が必要です）。Caustics パネルの Method が MNEE の時に使います。 |
| **停止して仕上げる** | レンダリング中のステータスバー（停止ボタンの横） | 今のサンプル数でレンダリングを終えます（デノイズがオンならデノイズしてから）。 |

## 6. DLSS ランタイム

DLSS Ray Reconstruction は NVIDIA の著作物であり、Falcon Engine には同梱されていません。
Falcon Engine は実行時にこれを読み込みます。ビルドに必要なのは DLSS SDK のヘッダーのみです（[BUILD.md](BUILD.md) を参照）。

- ランタイム（Linux では `libnvidia-ngx-dlssd.so.*`）を任意のフォルダへ置き、`Preferences → Add-ons → Cycles-expansion → DLSS Runtime Folder` に指定してから Blender を再起動してください。
- フォルダのパスに日本語などの非 ASCII 文字が含まれていても利用できます。

## 7. 動作要件

- NVIDIA GeForce RTX 20 シリーズ以降（RTX 30 シリーズ以降を推奨）、ドライバー 580 以降。
- 現時点では Linux のみ対応です。これまでに動作確認したのは RTX 3080 と Arch Linux の組み合わせですが、他のディストリビューションでも動作する想定です。Windows と macOS は未対応です。
- NVIDIA GPU が無い環境でも、Cycles-expansion は CPU でレンダリングでき、EEVEE も通常通り使えます。AMD（HIP）および Intel（oneAPI）の GPU レンダリングは既定ではビルドしておらず未検証です。

## 8. その他の既知の不具合

- 公式の Cycles 向けに作られたアドオン（`cycles` の設定を読む BlendLuxCore など）は、Falcon Engine が公式 Cycles を置き換えているため読み込みに失敗します。レンダリング自体には影響しません。
- Video Sequencer のストリップがあるシーンをレンダリングすると落ちることがあります。`Output Properties → Post Processing → Sequencer` をオフにするとレンダリングできます。Falcon 側でこの箇所は変更しておらず、公式の Blender 5.3 でも同様に起きるか確認中です。
- 不具合は公式の Blender プロジェクトではなく、[Issues](https://github.com/Relrei/Falcon-Engine/issues) へご報告ください。
