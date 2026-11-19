日本語 | [English](../translations/en/docs/USAGE.md)

# Falcon Engine v0.7.1 — 機能と使い方

> 以下の項目名は Blender の画面に表示されるラベルです。*(既知の不具合)* と書かれた項目は現在調査・対応中です。

Falcon Engine は Blender 5.3 をもとにしたカスタムビルドです（Blender Foundation とは関係がありません）。
Cycles をもとに拡張・カスタムしたレンダーエンジン **Cycles-expansion** が入っています（Bidirectional を切り、Mode を Expansion にすると公式の Cycles と同じ描き方になります）。EEVEE はそのままです。`WITH_FALCON_OFFICIAL_CYCLES=ON` を付けてビルドすると、Cycles-expansion と並べて公式の Cycles もビルドできます（[BUILD.md](BUILD.md) 参照）。

## 1. 有効にする

1. Properties → Render → Render Engine: **Cycles-expansion** を選びます。
2. Edit → Preferences → Add-ons → Cycles-expansion: **OptiX**（推奨）または CUDA を選び、お使いの GPU にチェックを入れます。
3. Render → Device: **GPU Compute** を選びます。
4. Render → **Mode**:

| モード | 内容 |
|---|---|
| **Expansion**（既定） | Cycles に Falcon の拡張（双方向の光追跡など）を足したモードです。サンプリングやデノイズは Cycles と同じように動きます。 |
| **Neural Rendering** | DLSS Ray Reconstruction をもとにしたモードです。フレームあたり少ないサンプル数とガラスへの追加サンプルを、DLSS の履歴で蓄積して描きます。最終レンダリング・ビューポートとも常に DLSS でデノイズします。NVIDIA RTX GPU と DLSS ランタイムが必要です（6 節参照）。 |

## 2. コースティクスと双方向光輸送 (Bidirectional)

Render → Light Paths → **Bidirectional**（見出しのチェックを入れて有効にします）。

| 設定項目 | 内容 |
|---|---|
| **Automatic**（既定 ON） | シーンに合う手法を自動で選び、1 サンプル目の寄与がほとんど無い手法はそのフレームで止めることで、コースティクスの無いシーンで遅くなるのを小さく抑えます（測った範囲では 1 割以内）。発光メッシュだけで照らすシーンでは即座に止まります。 |
| **Photons** | 各フレームの最初に撃つフォトンパスの数（ガラスや水を通して見えるコースティクス用）。増やすほど滑らかで鋭くなります（1 個あたり 48 バイト、既定 1,000,000）。 |
| **Caustic Strength** | ライトトレーシングとフォトンマッピングによるコースティクスの強さの倍率です。1 が物理的に正しい値です。 |

**Automatic** のチェックを外すと、手法を手動で選べます:

| 手法 (Strategy) | 内容 |
|---|---|
| **Light Tracing** | 光源からパスを伸ばし、カメラやカメラパスとつなぎます。拡散面の上のコースティクス（机の上のグラスの集光など）や、霧の中の光の筋を描きます。 |
| **Photon Mapping** | 光源から放ったフォトンをカメラパス上で集めます。ガラスや水を通して見るコースティクス（プールの底、瓶の中の集光など）に向いています。 |
| **Manifold Walks** | まだ未実装です（項目は出ますが、チェックしても使わずに描きます）。 |
| **Light Bounces** | ライトパスの最大バウンス数（既定 8）。 |

補足:
- ライトパスは霧（ボリューム）も通るため、スポットライトやプリズムを通った光の筋が霧の中に出ます。不均一な霧はステップ刻みで評価します（Volumes → Steps、既定 32）。*(既知の不具合: Bidirectional を入れると、切った時と比べて霧が 25 % ほど暗く描かれます)*
- Bidirectional は GPU 1 枚で動きます。
- ビューポートでは Bidirectional は効きません。最終レンダリング（F12）で確かめてください。

## 3. Neural Rendering — 最終レンダリング

Render → Sampling → Render（Neural Rendering モード時）:

| 設定項目 | 内容 |
|---|---|
| **Samples** | 全ピクセルのサンプル数（ノイズ閾値はありません）。 |
| **Glass** | カメラからの最初のレイがガラスに当たったピクセルのサンプル数です。Samples 以下にすると無効になります。アップスケーリングを伴う DLSS では使われません。 |
| **Halve Samples Each Frame** | アニメーション用: 1 フレーム目（およびカット切り替え直後）の後のフレームは DLSS の履歴に頼り、ガラス以外のピクセルのサンプル数を前フレームの半分ずつ（8, 4, 2, 1… と下限 1 まで）減らします。ガラスのピクセルは Glass のサンプル数を保ちます。 |
| **Time Limit** | Cycles と同じ時間制限です。 |

**Glass Through Guides**（既定 **Albedo and Normal**）: ガラスの向こうに見える面のアルベドと法線を DLSS に伝えることで、ガラス越しの背景が滲まずくっきり保たれます。Off にすると Cycles 標準のガイドのままになります。

Bidirectional は Neural Rendering と併用できます（コースティクスも DLSS に渡されます）。
*(既知の不具合・Neural Rendering のみ: プリズムのように光を虹色に分ける分散ガラスで白い壁などの明るい面に虹を落とすと、虹の周りに水色の縁が出ることがあります)*

## 4. Neural Rendering — ビューポート

Render → Sampling → Viewport（Neural Rendering モード時）:

| 設定項目 | 内容 |
|---|---|
| **Samples per Frame** | ビューポート 1 フレームあたりのピクセルサンプル数（既定 3）。視点が止まっている間、Max Samples までフレームを重ねます。 |
| **While Moving** | 視点を動かしている間のフレームあたりサンプル数です。 |
| **Glass** | ガラスピクセルのフレームあたりサンプル数です。History が Off の時やアップスケーリング DLSS の時は使われません。 |
| **Max Samples** | 視点が止まっている時に蓄積を止めるサンプル数です。 |
| **History → Carry Over Frames** | フレーム間で DLSS の履歴を持ち越します。Off にすると毎フレーム履歴を消すため、移動後の残像（ゴースト）は出なくなりますが粒ノイズやちらつきが増え、Glass も使われません。 |

Render → Light Paths → **Neural Rendering Viewport**: **Scene Light Paths** が Off（既定）の間、ビューポートは Falcon が決めたバウンス数（パネル内に表示）を使います。最終レンダリングでは常にシーンの Light Paths を使います。

## 5. その他の追加機能

| 設定項目 | 場所 | 内容 |
|---|---|---|
| **Unlimited** | Sampling → Render（Expansion モード） | 止めるまで最終レンダリングのサンプルを無制限に足し続けます（Esc で止めるとその時点の絵が残ります）。デノイズ Off の時のみ使えます。 |
| **Light Mixer** | View Layer → Light Mixer（Light Groups を使用） | レンダリングし直さずに、コンポジター上で各ライトグループの強さと色を後から変更できます。 |
| **Shadow Caustics** | Light、および Object → Shadow Caustics | Cycles と同じ、屈折物体の影の中の近似コースティクスです（ライト・投射側・受光側の設定が必要）。 |

## 6. DLSS ランタイム

DLSS Ray Reconstruction は NVIDIA の著作物であり、Falcon Engine には同梱されていません。
Falcon Engine は実行時にこれを読み込みます。ビルドに必要なのは DLSS SDK のヘッダーのみです（[BUILD.md](BUILD.md) 参照）。
- ランタイム（Linux では `libnvidia-ngx-dlssd.so.*`）を置いたフォルダを、`Preferences → Add-ons → Cycles-expansion → DLSS Runtime Folder` に指定して Blender を再起動してください。
- フォルダのパスに日本語などの非 ASCII 文字が入っていても使えます。

## 7. 動作要件

- NVIDIA GeForce RTX 20 シリーズ以上（RTX 30 シリーズ以上を推奨）、ドライバー 580 以上。
- 現時点では Linux のみです。これまでに実機で確認したのは RTX 3080・Arch Linux の組み合わせですが、他のディストリビューションでも動作します。Windows と macOS はまだ未対応です。
- NVIDIA GPU が無くても Cycles-expansion は CPU でレンダリングでき、EEVEE も通常通り使えます。AMD (HIP) と Intel (oneAPI) の GPU レンダリングは既定ではビルドされず、未検証です。

## 8. その他の既知の不具合

- 公式の Cycles が入っていないため、公式の Cycles の設定（`cycles`）を読むアドオン（BlendLuxCore など）は読み込みに失敗します。レンダリング自体には影響しません。
- 動画編集（Video Sequencer）のストリップがあるシーンをレンダリングすると、落ちることがあります。`Output Properties → Post Processing → Sequencer` のチェックを外すとレンダリングできます。ここは Falcon が手を入れていない部分で、公式の Blender 5.3 でも起きるかを確かめています。
- 不具合は公式の Blender ではなく [Issues](https://github.com/Relrei/Falcon-Engine/issues) へご報告ください。
