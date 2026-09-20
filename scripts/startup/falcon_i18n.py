# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Falcon Engine: the Japanese translation of the UI text Falcon adds.

The source text is English and lives in the code. This file only supplies the Japanese, as one
table (English, Japanese), registered with ``bpy.app.translations``. Blender consults the same
table for C++ ``IFACE_()`` / ``TIP_()`` / ``RPT_()`` lookups, so text built in C++ (the export
info line in the Output properties) is translated from here too.

- Every entry is registered for the default context and for "Operator", the context operator
  buttons and titles are looked up in.
- Blender's own message catalog is consulted first, so for text Blender already translates
  (e.g. "Quality") Blender's translation is shown.
- Text with ``%s`` / ``%d`` is translated before formatting (``iface_("...%d") % n``); keep the
  placeholders the same in both columns.
"""

import bpy

_CONTEXTS = ("*", "Operator")

# (English, 日本語)
JA = (
    # --- F-Cycles: DLSS (Render Properties > Sampling > Denoise) ----------------------------
    ("Upscale Quality", "アップスケール品質"),
    ("None = native resolution, best quality; lower entries are faster but coarser",
     "None=等倍が最高品質・下ほど速いが粗い"),
    ("No DLSS-capable GPU found", "DLSS対応GPUが見つからない"),
    ("Carry History", "履歴持ち越し"),
    ("Carry the DLSS temporal history from frame to frame in animation renders "
     "(the way games do). Reduces flicker and stays stable at fewer samples. "
     "Off resets the history every frame, as before",
     "アニメーションレンダーでDLSSの時間履歴をフレーム間で持ち越す"
     "(ゲームと同じ動作)。ちらつきが減り少ないサンプルで安定する。"
     "OFFで旧来のフレーム毎リセットに戻る"),
    ("Carry Navigation History", "ナビ履歴持ち越し"),
    ("Keep the DLSS temporal history across viewport navigation, aligned with "
     "motion vectors, and accumulate samples while the view is still. "
     "Removes the wobble of re-converging after every reset and lets a still image converge "
     "up to the maximum samples. "
     "When off, every update redraws a one-sample image, so edges stay sharp but grain remains. "
     "The low-resolution preview during navigation is disabled, so navigating is heavier. "
     "Volumes have no motion vectors and smear while moving "
     "(turning it off is recommended for scenes with volumes)",
     "ビューポートのナビゲーションをまたいでDLSSの時間履歴を"
     "モーションベクタで整列して持ち越し、停止時はサンプルを蓄積する。"
     "リセット毎の再収束のブレが消え、静止画像が最大サンプルまで収束する。"
     "OFFにすると更新のたびに1サンプルの絵を描き直すので、"
     "輪郭は鮮明なまま粒が残る。"
     "ナビ中の低解像度プレビューは無効になるため操作は重くなる。"
     "ボリュームはモーションベクタを持たないため移動中に尾を引く"
     "(ボリューム入りシーンはOFF推奨)"),
    ("First Frame Pre-Roll Passes", "初回蓄積レンダリング回数"),
    ("For the first frame of an animation only, re-render the same frame this "
     "many times to build up independent estimates in the DLSS temporal history before the "
     "real frame is output. "
     "Not used by default: it was replaced by a warm-up that renders and discards the two "
     "frames before the first one with the real motion "
     "(re-rendering the same frame builds a history without motion and made the first frame "
     "noisier; the warm-up gives a better first frame and is 30 seconds faster over 8 frames). "
     "To use this count, turn the warm-up off with the environment variable "
     "FALCON_DLSS_ANIM_WARMUP=0. "
     "0 disables it. A still (a single F12 frame) has no following frames, so its count is set "
     "separately with FALCON_DLSS_STILL_PREROLL "
     "(the number of passes run is shown in the render progress and in the image metadata "
     "cycles.dlss.preroll_passes)",
     "アニメーションの1枚目だけ、同じフレームをこの回数だけ焼き直して"
     "DLSSの時間履歴に独立した推定を溜めてから本番の1枚を出す。"
     "★既定では走らない: 1枚目の前の2コマを本番と同じ動きで焼いて捨てる「温め」に置き換えた"
     "(同じフレームの焼き直しは動きの無い履歴を作り、1枚目をかえって荒くしていた。"
     "温めのほうが1枚目が良く、8コマで30秒速い)。"
     "この回数を使うには環境変数 FALCON_DLSS_ANIM_WARMUP=0 で温めを切る。"
     "0で無効。静止画(F12で1枚だけ)は続くコマが無いので"
     "FALCON_DLSS_STILL_PREROLLで回数を別に決める"
     "(走った回数はレンダー中の進捗と画像のメタデータ cycles.dlss.preroll_passes に出る)"),
    ("Pre-Roll Passes at Cuts", "カット時の蓄積レンダリング回数"),
    ("At Cuts (0 = Same as First)", "カット時(0=初回と同じ)"),
    ("Re-render the frame where a cut happens this many times. "
     "The history is discarded at a cut too, so the same problem as the first frame occurs, "
     "but cuts are far more frequent, so the count can be set separately. "
     "0 uses the same count as the first frame",
     "カットが切り替わったフレームだけ、同じフレームをこの回数だけ焼き直す。"
     "カットでも履歴は捨てられるので1枚目と同じ問題が起きるが、"
     "カットの方がずっと多いため回数を別に決められる。"
     "0で「初回と同じ回数」"),
    ("Pre-Render at Camera Switches", "カメラ切り替え時の事前レンダリング"),
    ("Detect the frames where timeline markers switch the camera and rebuild the "
     "DLSS temporal history there. "
     "Motion vectors do not connect the frames across a cut, so the history cannot be "
     "carried over, but final renders did not detect the switch and the previous shot bled "
     "into the new one for about 5 frames "
     "(measured: brightness of the cut frame 0.246, expected 0.169). "
     "Off restores the behavior before 2026-09-04. "
     "No extra render time (only the denoiser state is swapped)",
     "タイムラインのマーカーでカメラが切り替わるコマを検出して、"
     "DLSSの時間履歴をそこで作り直す。"
     "カットの前後はモーションベクタで繋がらないので履歴は引き継げないが、"
     "最終レンダーではこの切り替わりが検出されておらず、"
     "前のショットが5コマほど新しいショットに滲んで残っていた"
     "(実測: カットのコマの明るさ 0.246 / 本来 0.169)。"
     "OFFで2026-09-04以前の挙動に戻る。"
     "追加のレンダー時間は無い(デノイザの状態を入れ替えるだけ)"),
    ("Warm-Up Frames", "ウォームアップ枚数"),
    ("Render and discard this many real frames before the start frame of the "
     "animation and before each cut. "
     "The DLSS history only builds up from real frames seen from other viewpoints, so this "
     "fills in the extra noise of the cold first frame and of the frames right after a cut "
     "(2 frames mostly fill it, 4 is the ceiling; re-rendering the same image or adding "
     "samples does not warm it up). "
     "Used by \"Render with Warm-Up\"",
     "アニメの開始フレームと各カットの直前に、実フレームをこの枚数だけ焼いて捨てる。"
     "DLSSの履歴は別視点の実フレームからしか育たないため、"
     "冷えた1枚目とカット直後だけノイズが多くなるのを埋める"
     "(2枚でほぼ埋まり4枚で頭打ち。同じ絵の焼き直しやサンプル増量では温まらない)。"
     "「ウォームアップ付きレンダー」で使う"),
    ("AI Frame Interpolation", "フレーム補間"),
    ("Render every other frame and create the frames in between with AI "
     "interpolation (RIFE). Takes about half the time. "
     "The frames around cuts and the last frame are always rendered. "
     "Interpolated frames look slightly soft in fast motion. Image sequence output only",
     "1コマおきにレンダーし、間のコマをAI補間(RIFE)で作る。時間は約半分。"
     "カットの前後と最後のコマは必ずレンダーする。"
     "速い動きの補間コマは少し眠くなる。連番画像の出力だけ対応"),
    ("Render every frame", "全てのコマをレンダーする"),
    ("2x (Render Every Other Frame, RIFE In Between)", "2倍(1コマおきに描き、間をRIFEで作る)"),
    ("Render every other frame and create the frames in between with RIFE",
     "1コマおきにレンダーし、間のコマをRIFEで作る"),
    ("History Motion Limit", "履歴を保つ動きの上限"),
    ("Discard the history on frames where the camera moved more than this many "
     "pixels. "
     "DLSS assumes the small per-frame motion of a game running at 60 frames per second; "
     "a path-traced viewport is slow per frame, so moving the mouse makes the motion too "
     "large, the history no longer lines up and ghosting appears. "
     "Smaller values reduce ghosting but also the smoothness while navigating "
     "(0 always resets on navigation, the same as turning Carry Navigation History off)",
     "この画素数を超えてカメラが動いたフレームでは履歴を捨てる。"
     "DLSSはゲームの毎秒60コマの小刻みな動きが前提で、"
     "パストレのビューポートは1コマが遅いためマウスを振ると動きが大きくなりすぎ、"
     "履歴が正しく重ならずゴーストになる。"
     "小さくするとゴーストは減るがナビ中の滑らかさも減る"
     "(0でナビ毎に必ずリセット=持ち越しOFF相当)"),
    ("No Depth of Field in Preview", "プレビュー中は被写界深度を切る"),
    ("Close the camera aperture during the DLSS viewport preview only. "
     "Lens depth of field is a stochastic blur where the ray direction changes every "
     "sample, which DLSS does not expect, so it turns into blotches "
     "(the reason only the camera view looks noisy). "
     "The walk view has no depth of field to begin with, so with this the camera view looks "
     "the same. "
     "Check depth of field with OIDN or a final render",
     "ビューポートのDLSSプレビュー中だけカメラの絞りを閉じる。"
     "レンズによるボケはサンプル毎にレイの向きが変わる確率的なぼかしで、"
     "DLSSの想定外のため斑点になる(カメラビューだけ荒れる原因)。"
     "ウォークビューは元からボケないので、この設定でカメラビューが同じ見え方になる。"
     "ボケの確認はOIDNか最終レンダーで"),
    # --- F-Cycles: caustics properties -----------------------------------------------------
    ("Auto Radius", "半径を自動"),
    ("On every bake, trace rays out to the surfaces the caustics land on, "
     "measure how many meters one pixel covers there and use that as the radius. "
     "The radius is the size of the blur: in meters the right value changes by orders of "
     "magnitude with the field of view and distance and has to be retuned for every scene, "
     "but in pixels there is one answer (keep it just below what the frame can show). "
     "The render resolution is taken into account too, so baking for 4K shrinks the radius "
     "and sharpens the result automatically. "
     "Off uses the value below as is",
     "ベイクのたびに、コースティクスが落ちる面まで実際にレイを飛ばし、"
     "そこで1画素が何メートルになるかを測って半径にする。"
     "半径はぼけの大きさなので、メートルで言うと画角と距離で必要な値が桁で変わり"
     "毎シーン合わせ直しになるが、画素で言えば答えは一つ(画面で見える手前に隠す)。"
     "レンダー解像度も見るので、4Kで焼けば自動で半径が縮んで鋭くなる。"
     "OFFで下の値をそのまま使う"),
    ("Radius (Pixels)", "半径(画素)"),
    ("Target used by Auto Radius: the radius given as the number of pixels of "
     "blur it amounts to. "
     "1 roughly matches the hand-tuned value. "
     "Larger values stay smooth with fewer photons, at the cost of thicker filaments",
     "自動時の目標。半径が何画素分のぼけに相当するかで指定する。"
     "1がおおよそ手で合わせ込んだ値と一致する。"
     "大きくすると少ない光子でも滑らかになる代わりにフィラメントが太る"),
    ("Caustics", "コースティクス"),
    ("Caustics from photons shot from the lights. Like Photon path tracing "
     "in Octane, it only works once enabled. While off, its settings are "
     "hidden and no photon passes run (the same image as plain Cycles)",
     "光源から光子を撃つコースティクス。Octane の Photon path "
     "tracing と同じで、入れて初めて働く。切っている間は設定も "
     "出ず、光子のパスも走らない(素の Cycles と同じ絵)"),
    # 日本語も直した: 元は「集光の出し方。<作者の依頼の引用と日付>に対応する3つ」
    ("How the caustics are produced: Auto, Accumulate or Approximate",
     "集光の出し方。自動・蓄積・疑似の3つ"),
    ("Auto", "自動"),
    ("Choose the photon count from the scene and produce the caustics "
     "in a single render. No knobs needed. The photons, 20% of the "
     "final sample count, are shared by all lights",
     "場面から光子の数を決めて1回のレンダーで出す。"
     "摘みは要らない。光子は最終サンプル数の 20% を"
     "全部の灯で分け合う"),
    ("Accumulate", "蓄積"),
    ("The same approach as LuxCore (default). Shoots a full "
     "sample count of photons per light and adds them up. "
     "The most accurate and the heaviest. Pressing Esc "
     "midway composites the layers done so far",
     "LuxCore と同じ形(既定)。灯ごとにサンプル数いっぱいの"
     "光子を撒いて足す。いちばん正確でいちばん重い。"
     "途中で Esc を押すと、そこまでの層で合成する"),
    ("Approximate", "疑似"),
    ("Looks and speed. Uses 1/8 of the photons, widens the "
     "blur and skips the visibility test. Less accurate",
     "映えと速さ。光子を 1/8 にしてぼかしを広げ、"
     "可視性の判定を切る。正確さは落ちる"),
    ("Off", "オフ"),
    ("Spread the photons evenly over the light's whole emission "
     "(previous behavior)",
     "光源の放射全体へ均等に撒く(従来)"),
    ("Improved only the tails and made the whole worse (measured "
     "2026-07-29: tails -14.3%, overall +4.4%). The split is too coarse "
     "to cut out the directions that hit nothing",
     "裾野だけ改善し全体は悪化した(実測 2026-07-29: 裾野 -14.3% / "
     "全体 +4.4%)。分割が粗すぎて空撃ち方向を切り出せないため"),
    ("Recommended. Measured 2026-07-29: overall -9.6%, tails -17.5%. "
     "5 of the 16 tiles are found to carry no transport and their budget "
     "goes to the directions that matter",
     "推奨。実測 2026-07-29 で全体 -9.6% / 裾野 -17.5%。16枚中5枚が"
     "輸送ゼロと判定され、その予算が効く方向へ回る"),
    ("Finer still, not measured yet. The probe cost grows with n^2, so "
     "only when many samples are available",
     "さらに細かいが未計測。プローブ代が n^2 で増えるのでサンプル数を"
     "多く取れる時だけ"),
    ("Automatic Caustics", "コースティクス自動化"),
    ("Make caustics (focused light patterns) possible without any recipe "
     "knowledge in scenes with glass or refractive materials and lights. "
     "Auto: when detected, a one-click entry appears at the top of the "
     "F-Cycles panel. Off: no detection (manual, with the Photon/LT panels "
     "below)",
     "ガラス/屈折マテリアルとライトのあるシーンで、レシピ知識ゼロで"
     "コースティクス(集光模様)を出せるようにする。自動=検出したら"
     "F-Cyclesパネル上部にワンクリックの導線が自動で出る。オフ=検出"
     "しない(下のPhoton/LTパネルで手動)"),
    ("When glass and a light are detected, make the caustics "
     "bakeable with one click (default)",
     "ガラス+ライトを検出したらワンクリックで焼ける状態にする(既定)"),
    ("Do not detect automatically", "自動検出しない"),
    ("Produce the caustics (focused light patterns) of glass or refraction "
     "and lights. On bakes them with proven settings and composites them into "
     "the following renders. Off removes them from the composite (the cache "
     "is kept)",
     "ガラス/屈折とライトのコースティクス(集光模様)を出す。"
     "ONで実証済みの設定で焼き、以後のレンダーに合成される。"
     "OFFで合成を外す(キャッシュは残る)"),
    ("Quality", "品質"),
    ("Quick: composite the baked photons into the render as they are. "
     "Clean: make smooth caustics converged with light tracing (a few minutes)",
     "速い=焼いたフォトンをそのままレンダーに合成する。"
     "清書=ライトトレースで収束させた滑らかな集光を作る(数分)"),
    ("Quick", "速い"),
    ("Composite the baked photons as they are (default)",
     "焼いたフォトンをそのまま合成する(既定)"),
    ("Clean (Minutes)", "清書(数分)"),
    ("Make smooth caustics converged with light tracing",
     "ライトトレースで収束させた滑らかな集光を作る"),
    # --- F-Cycles: add-on preferences --------------------------------------------------------
    ("RIFE Folder", "RIFEの場所"),
    ("Folder containing rife-ncnn-vulkan, used for frame interpolation. "
     "When empty, it is searched for in the environment variable FALCON_RIFE_DIR, "
     "in falcon/rife next to Blender, then in PATH",
     "フレーム補間に使う rife-ncnn-vulkan が置いてあるフォルダ。"
     "空なら環境変数 FALCON_RIFE_DIR・Blenderの隣の falcon/rife・PATH の順に探す"),
    ("Simple F-Cycles Panel", "F-Cycles を簡単表示にする"),
    ("Reduce the F-Cycles panel to a single Caustics checkbox and fold the "
     "other controls under \"Details\"",
     "F-Cycles パネルをコースティクスのチェックボックス1つに絞り、"
     "今までのツマミは「詳細」の下へ畳む"),
    # --- F-Cycles panel (Render Properties > F-Cycles / 3D Viewport sidebar > Falcon) --------
    ("Rendering on the CPU — set Device to GPU Compute", "CPUレンダー中 — デバイスをGPUコンピュートに"),
    ("Final denoising is not on the GPU (fix with the presets below)",
     "最終デノイズがGPUではない (下のプリセットで解決)"),
    ("Viewport denoising off", "ビューポートデノイズ OFF"),
    ("Enabled — caustics appear in renders", "有効 — レンダーに集光が出ます"),
    ("Strength", "強さ"),
    ("Glass and light detected", "ガラス+ライトを検出"),
    ("Add glass or refraction and a light to make caustics", "ガラス/屈折とライトを置くと出せます"),
    ("Clean Caustics (LT, Minutes)", "清書コースティクス (LT・数分)"),
    ("Render", "レンダー"),
    ("Faster Viewport", "ビューポート高速化"),
    ("High-Quality Still", "静止画 高品質"),
    ("For Animation", "アニメーション用"),
    ("Photons", "光子数"),
    ("Cell", "セル"),
    ("Chromatic Dispersion", "分散"),
    ("GPU (Fast)", "GPU (速い)"),
    ("Point Map", "点マップ"),
    ("Pixel Radius", "画素"),
    ("Radius (m)", "半径(m)"),
    ("Gain", "ゲイン"),
    ("Cell (m)", "セル(m)"),
    ("Spread", "広がり"),
    ("Point Cap", "点上限"),
    ("Normal Angle (Degrees)", "法線角(度)"),
    ("Caustic Smoothness", "滑らかさ"),
    ("Composite: on (point map)", "合成: 有効 (点マップ)"),
    ("Composite: on", "合成: 有効"),
    ("Blur (px)", "ぼかし(px)"),
    ("Visibility (Remove Occluded/Through-Glass)", "可視性 (遮蔽/ガラス越し除去)"),
    ("Splat Direct Floor (Brightness Calibration)", "直接光の床も撒く (明るさ較正用)"),
    ("Emission Guiding", "発射誘導"),
    ("World Photons (Caustics Inside Shadows)", "ワールド光子 (影の中の埋め込みコースティクス)"),
    ("Culled: %d", "カリング中: %d 個"),
    ("Simplify + Camera Culling is off (not in effect)", "簡略化+カメラカリングがOFF (無効状態)"),
    ("Culled objects also disappear from reflections, GI and shadows",
     "対象は反射/GI/影からも消える点に注意"),
    ("Save Material for Flicker Removal", "除去用の素材を自動保存する"),
    ("After rendering, apply with tools/falcon_temporal.py", "レンダー後 tools/falcon_temporal.py で適用"),
    ("Mode", "モード"),
    ("Blend", "ブレンド"),
    ("Auto GI Gate", "自動GIゲート"),
    ("Gate Low", "ゲート下限"),
    ("Gate High", "上限"),
    ("Keep History", "履歴保持"),
    ("Cache", "キャッシュ"),
    ("Render once at high spp, then switch to Blend", "高sppで一度レンダー後、Blendに切替"),
    ("Blend + denoising hurts final renders — Live (viewport) recommended",
     "Blend+デノイズはfinalで逆効果 — Live(ビューポート)推奨"),
    ("For GI-heavy scenes without denoising", "デノイズ無しのGI重シーン向け"),
    ("Viewport only: GI keeps converging while the camera holds still",
     "ビューポート専用: カメラ静止中にGIが収束し続ける"),
    ("Works best together with \"Faster Viewport\"", "「ビューポート高速化」併用で効果大"),
    ("Use-Case Presets", "用途プリセット"),
    ("Caustics (Photon)", "コースティクス (Photon)"),
    ("Light Tracing (LT, Final-Quality Still)", "ライトトレース (LT・FQ静止画)"),
    ("Auto Culling (Scene Slimming)", "自動カリング (シーン痩身)"),
    ("Animation Flicker Removal (Temporal)", "アニメのちらつき除去 (Temporal)"),
    ("SHARC Cache (Experimental)", "SHARC キャッシュ (実験的)"),
    ("Details", "詳細"),
    # --- F-Cycles: operators (buttons, tooltips, reports) -----------------------------------
    ("Set up the viewport for low-spp previews: OIDN GPU denoising from the first sample. Measured to be the biggest lever for near-realtime (path guiding is ineffective at low spp)",
     "ビューポートを低sppプレビュー向けに設定: OIDN GPUデノイズをサンプル1から適用。実測でnear-realtimeの最大レバー(path guidingは低sppで無効)"),
    ("Final render settings for animation: OIDN on the GPU (Accurate/High) with a noise threshold of 0.1. 240 frames pay for the threshold 240 times, so 0.1 is used and stability is recovered with the Temporal filter (measured: 0.01 takes 7.9x the time for a 7% improvement that is invisible under OIDN). Turns SHARC off",
     "アニメーション最終レンダー設定: OIDN GPU(accurate/high)+ノイズしきい値0.1。240フレームはしきい値を240回払うので0.1とし、安定性はTemporalフィルタで回収(実測: 0.01は7.9倍の時間でOIDN下では見えない7%改善)。SHARCはOFFに"),
    ("Final render settings for stills: OIDN on the GPU (Accurate/High), noise threshold 0.01, at most 1024 samples. A single frame can afford a tight threshold (1.5 to 2 minutes for a backroom-class FHD scene). For a special shot, lower it to 0.005 by hand. Turns SHARC off",
     "静止画最終レンダー設定: OIDN GPU(accurate/high)+ノイズしきい値0.01・上限1024。1枚ならタイトなしきい値が払える(backroom級FHDで1.5〜2分)。こだわりの一枚は0.005へ手動調整。SHARCはOFFに"),
    ("LT flood risk: light \"%s\" is embedded at the base of "
     "glass \"%s\" → a large diffuse surface sees the light "
     "directly through the glass (raising it to mid-height helps)",
     "LT氾濫の危険: ライト「%s」がガラス「%s」の底部に埋込 → "
     "大きな拡散面がガラス越しにライトを直視 (中腹へ上げると改善)"),
    ("Make Caustics", "コースティクスを出す"),
    ("Bake the caustics (focused light patterns) of glass and lights with proven recommended settings. No recipe settings needed. When done, they are composited into normal renders (F12 and the viewport) automatically. The strength can be adjusted later without rebaking",
     "ガラス+ライトのコースティクス(集光模様)を、実証済みの推奨設定で焼く。"
     "レシピ設定は不要。完了後は通常のレンダー(F12/ビューポート)に自動で合成される。"
     "強さはあとから焼き直し無しで調整できる。"),
    ("Needs a glass or refractive material and a light (Sun, Area, Spot or Point)",
     "ガラス/屈折マテリアルとライト(SUN/AREA/SPOT/POINT)が要ります"),
    ("Bake Caustics", "コースティクスを焼く"),
    ("Run a photon trace in this scene and bake the caustics (light focused by glass, water and mirrors) into a cache. Once done, they are composited into the following renders automatically (additive, almost no extra render time). Casters: Principled transmission (glass/water) and smooth metals. Light source: the first light (Sun supported). Note: turning off the transparent shadows of water/glass gives a physically correct composite",
     "このシーンでフォトントレースを実行し、コースティクス(ガラス/水/鏡の集光)を"
     "キャッシュに焼く。完了後のレンダーに自動で合成される(加算・レンダー時間ほぼ増なし)。"
     "対象: Principledの透過(ガラス/水)・滑らかな金属。光源は最初のライト1灯(SUN対応)。"
     "注意: 水/ガラスの「透明の影」をOFFにすると物理的に正しい合成になる"),
    ("The render engine is not Cycles", "レンダーエンジンがCYCLESではありません"),
    ("Could not measure the radius automatically (no camera, or "
     "nothing was hit). Using %.3f m as is",
     "半径の自動測定に失敗(カメラが無い/何にも当たらない)。"
     "%.3fm をそのまま使います"),
    ("Radius set automatically: %.4f m (%.1f pixels)", "半径を自動決定: %.4fm (%.1f画素相当)"),
    ("No light found (one is needed)", "ライトが見つかりません(1灯必要)"),
    ("GPU photon bake failed: %s", "GPUフォトンベイク失敗: %s"),
    ("Photon merge failed: %s", "フォトンマージ失敗: %s"),
    ("Photon trace failed: %s", "フォトントレース失敗: %s"),
    ("GPU point map", "GPU点マップ"),
    ("Caustics baked (%s, %.0f s) — active from the next render",
     "コースティクス焼き完了 (%s, %.0f秒) — 次のレンダーから有効"),
    ("Disable Caustics", "コースティクスを無効化"),
    ("Stop compositing the photon caustics (the cache is kept)",
     "フォトンコースティクスの合成を無効にする(キャッシュは残る)"),
    ("Photon caustics disabled", "フォトンコースティクス無効化"),
    ("Bake and Render Range (Separate Process)", "焼いて範囲レンダー (別プロセス)"),
    ("Bake the caustics and render the frame range (start..end) as an image sequence. Runs in a separate background process, not through the GUI, so it cannot hit the Vulkan viewport crash. By default it bakes once at the current frame and reuses the cache (static glass and lights; the camera may move). With \"Rebake Every Frame\" on, moving glass and lights work too (bake time x number of frames). The output path and format follow the normal animation output settings. The log is written next to the output",
     "コースティクスを焼いてフレーム範囲(start..end)を連番レンダーする。"
     "GUIを通さず別プロセス(background)で実行するのでVulkanビューポートの"
     "クラッシュを踏まない。既定=現フレームで1回焼いてキャッシュ使い回し"
     "(静止ガラス/ライト・カメラは動いてOK)。「毎フレーム焼き直し」ONで"
     "動くガラス/ライトにも対応(ベイク時間×フレーム数)。"
     "出力先/形式は通常のアニメ出力設定に従う。ログは出力先の隣に書く。"),
    ("Rebake Every Frame", "毎フレーム焼き直し"),
    ("Rebake on every frame, for shots where glass, water, mirrors or lights "
     "move (about N times the bake time of a single bake). Off: bake once at the "
     "current frame and reuse it for all frames",
     "ガラス/水/鏡やライトが動くショット用に各フレームでベイクし直す"
     "(1回焼きの約N倍のベイク時間)。OFF=現フレームで1回だけ焼いて全フレームで使い回す"),
    ("Renders frames %d–%d in a separate process", "フレーム %d–%d を別プロセスでレンダーします"),
    ("Output: %s", "出力: %s"),
    ("(not set)", "(未設定)"),
    ("Moving glass or lights detected → rebaking every frame by default",
     "動くガラス/ライトを検出 → 毎フレーム焼きを既定にしました"),
    ("%dM photons flicker when rebaking every frame — 64M recommended",
     "光子%dMは毎フレーム焼きでちらつきます — 64M推奨"),
    ("GPU bake is off (the CPU is about 100x slower)", "GPUベイクがOFFです (CPUは約100倍遅い)"),
    ("Invalid frame range (end < start)", "フレーム範囲が不正です (end < start)"),
    ("No animation output path is set (Output Properties)", "アニメ出力先(Output Properties)が未設定です"),
    ("Could not save a temporary copy: %s", "一時保存に失敗しました: %s"),
    ("Could not start the render process: %s", "起動に失敗しました: %s"),
    ("Range render started (%s, PID %d) — progress: %s", "範囲レンダー開始 (%s, PID %d) — 進捗: %s"),
    ("rebake every frame", "毎フレーム焼き直し"),
    ("bake once", "1回焼き"),
    ("Could not write the display image (the EXR was saved): %s", "表示画像の書き出し失敗(EXRは保存済): %s"),
    ("Light-Traced Composite Render", "ライトトレース合成レンダー"),
    ("Light-traced composite render (final-quality still). Caustics without a cache, connecting photons traced from the lights straight to the camera: they truly converge with the sample count (no point-map dots). Runs an LT pass per light plus a normal render and saves the additively composited image. Heavy: the sample count is the scene's sample count, so try low samples first. Runs with fixed sampling",
     "ライトトレース合成レンダー(FQ静止画)。光源から光子を追いカメラへ直接つなぐ"
     "キャッシュ無しコースティクス: サンプル数で本当に収束する(点マップのドット無し)。"
     "各ライトのLTパス+通常レンダーを実行し、加算合成した画像を保存する。"
     "重い: サンプル数=シーンのサンプル数。まず低サンプルで試す。固定サンプリングで実行される"),
    ("Guiding %dx%d needs more than %d samples (at least %d). "
     "Rendering with plain LT",
     "誘導%dx%d はサンプル数%dでは足りません(最低%d)。通常のLTで焼きます"),
    ("Light %d/%d (%s)", "ライト %d/%d (%s)"),
    ("%d lights in one pass", "ライト %d灯を1パス"),
    ("World photons", "ワールド光子"),
    ("Beauty Pass", "ビューティ"),
    ("%s photons %.1fM/%.1fM %.1f s", "%s 光子 %.1fM/%.1fM %.1f秒"),
    ("Caustics (photons) are not enabled "
     "(check the box in the Render Properties)",
     "コースティクス(フォトン)が有効ではありません "
     "(レンダープロパティのチェックを入れてください)"),
    ("No supported light (Sun, Area, Spot or Point)", "対応ライトがありません (SUN/AREA/SPOT/POINT)"),
    ("LT only supports up to 4096 px (splatting assumes a single tile)",
     "LTは4096px以下のみ (スプラットが1タイル前提)"),
    ("Emission windows only support Sun and Spot lights, so %s "
     "are rendered without guiding",
     "発射窓は SUN/SPOT のみ対応のため、%s は誘導なしで焼きます"),
    ("The LT layer covers %.1f%% of the frame (a caustics layer "
     "should be sparse). Compositor output or the background may "
     "have leaked into it",
     "LT層が画面の%.1f%%を覆っています(集光層は疎なはず)。"
     "コンポジター出力や背景が層に混ざっている可能性があります"),
    ("Could not get the Combined pass of the Render Result", "Render Result の Combined を取得できませんでした"),
    ("LT composite done (%s, %d lights%s%s, photons %d spp / image %d spp, "
     "%.0f s)%s → composited into the Render Result %s",
     "LT合成完了 (%s・%d灯%s%s, 光子%dspp/絵%dspp, %.0f秒)%s"
     " → Render Result に合成済 %s"),
    ("+ world", "+ワールド"),
    (", cancelled", "・中断"),
    ("⚠ flood risk", "⚠氾濫の危険あり"),
    ("LT render failed: %s", "LTレンダー失敗: %s"),
    ("%s (Esc to stop — the composite so far is kept)", "%s (Esc で中断・そこまでの合成が残ります)"),
    ("Falcon LT: stopped — rendering the beauty and compositing "
     "the layers done so far",
     "Falcon LT: 中断 — ここまでの層でビューティを焼いて合成します"),
    ("Clean Caustics (LT)", "清書コースティクス (LT)"),
    ("Clean caustics (LT). A single path for final stills that takes a few minutes. Unlike the photon point map, it produces smooth caustics that truly converge with the sample count (no dots, down to the stepped facet structure). When done, it opens in the Image Editor as a display image with color management (exposure/view transform) applied. The intended use is to settle the composition with the quick [Make Caustics] (photons) first and finish with this",
     "清書コースティクス(LT)。数分かかる最終静止画用の一本道。フォトンの点マップと違い、"
     "サンプル数で本当に収束した滑らかな集光(点々なし・段々のファセット構造まで)を出す。"
     "実行後はカラーマネジメント(露出/ビュー変換)適用済みの表示画像として画像エディタに開く。"
     "まず気軽な[コースティクスを出す](フォトン)で構図を決め、仕上げにこちらを使う想定。"),
    ("Needs a glass or refractive material and a light (Sun, Area or Spot)",
     "ガラス/屈折マテリアルとライト(SUN/AREA/SPOT)が要ります"),
    ("LT Recomposite (Gain/Blur, No Re-Render)", "LT再合成 (ゲイン/ぼかし変更・再レンダー無し)"),
    ("LT recomposite: reapply the current gain and blur to the raw passes (gain 1, blur 0) left by the last light-traced composite render and rebuild only the composite image. No re-render, a few seconds. Gain and blur are linear in the LT layer, so the result matches a re-render exactly",
     "LT再合成: 直前のLT合成レンダーが残した生パス(ゲイン1/ぼかし0)に、"
     "現在のゲイン/ぼかし値を適用し直して合成画像だけ作り直す。再レンダー無し・数秒。"
     "ゲインとぼかしはLT層に対して線形なので、レンダーし直した場合と結果は厳密に一致する。"),
    ("No raw passes. Run [Light-Traced Composite Render] first",
     "生パスがありません。先に[ライトトレース合成レンダー]を実行してください"),
    ("Recomposite failed (raw passes missing or broken?): %s", "再合成失敗 (生パス欠損/破損?): %s"),
    ("LT recomposite done (%d passes, gain %.2f, blur %.1f, %.1f s) → %s",
     "LT再合成完了 (%dパス, ゲイン%.2f ぼかし%.1f, %.1f秒) → %s"),
    ("Set up automatic saving of each frame's image and motion vectors to <render output>/temporal/ during animation renders (this button does not render by itself). Running tools/falcon_temporal.py on the saved material removes flicker (measured: 16 spp + filter is more stable than raw 64 spp)",
     "アニメレンダー中、各フレームの画像とモーションベクトルを<レンダー出力>/temporal/に自動保存する設定を組み込む(このボタン自体はレンダーしない)。保存した素材に tools/falcon_temporal.py を掛けるとちらつきを除去できる(実測: 16spp+フィルタが64spp生より安定)"),
    ("Render with Warm-Up", "ウォームアップ付きレンダー"),
    ("Warm up the DLSS history before rendering the animation. A few real frames are rendered and discarded before the start frame and before each cut, which removes the noise of the first frame and of the frames right after cuts (measured: noise metric 40.1 → 35.1). When the camera does not move before a shot, its motion is extrapolated to create parallax",
     "DLSSの履歴を温めてからアニメーションをレンダーする。開始フレームと各カットの直前に実フレームを数枚焼いて捨てるので、1枚目とカット直後のノイズが消える(実測: ノイズ指標40.1→35.1)。手前にカメラの動きが無い場合は補外して視差を作る"),
    ("Warm-Up Frames is 0", "ウォームアップ枚数が0です"),
    ("F-Cycles: rendered with %d cuts x %d warm-up frames", "F-Cycles: %d カット x %d 枚のウォームアップで焼きました"),
    ("Auto-Cull Unseen Objects", "映らない物を自動カリング"),
    ("Sweep the whole animation range and automatically enable camera culling on objects the camera never sees. Emissive and very large objects are excluded for safety. The affected objects are selected afterwards so they can be checked by eye (undoable)",
     "アニメ全レンジを掃引し、一度もカメラに映らないオブジェクトへカメラカリングを自動付与。発光体と巨大オブジェクトは安全のため除外。適用後は対象が選択状態になるので目視確認できる(Undo可)"),
    ("Sweep Step (Frames)", "掃引ステップ(フレーム)"),
    ("Test the view frustum every this many frames. Smaller is more accurate "
     "and slower",
     "何フレームおきに視錐台を判定するか。小さいほど正確で遅い"),
    ("Frustum Margin", "マージン"),
    ("Safety band outside the view frustum (fraction of the frame width), for "
     "things like motion blur spilling over",
     "視錐台の外側に取る安全帯(画面幅比)。モーションブラー等のはみ出し対策"),
    ("Large Object Ratio", "巨大物の除外比"),
    ("Objects whose bounding box diagonal exceeds this fraction of the scene "
     "diagonal are assumed to contribute a lot of GI and are excluded",
     "バウンディングボックス対角がシーン対角のこの比を超える物はGI寄与が大きいとみなし除外"),
    ("Panoramic cameras do not support culling (a Cycles limitation)",
     "パノラマカメラはカリング非対応 (Cycles側の制限)"),
    ("No objects to process", "対象オブジェクトがない"),
    ("Culled %d (excluded: %d emissive, %d large; out of %d) — the selected "
     "objects are the culled ones",
     "カリング %d 個 (発光除外 %d / 巨大除外 %d / 対象%d中) — 選択中の物が対象"),
    ("Verify Culling (Detect Wrongly Culled)", "カリング検証 (誤殺を自動検出)"),
    ("Detect side effects of culling (light leaks, missing shadows, missing reflections) with low-resolution test renders, find the responsible objects by bisection and remove them from culling automatically",
     "低解像度の検証レンダーでカリングの副作用(光漏れ/影消え/映り込み消え)を検出し、原因オブジェクトを二分探索で特定してカリングから自動除外する"),
    ("Tolerance (RMSE)", "許容差(RMSE)"),
    ("Treat a pixel difference with/without culling above this as a side effect",
     "有り/無しの画素差がこれを超えたら副作用ありとみなす"),
    ("No culled objects", "カリング中のオブジェクトがない"),
    ("No side effects (difference at most %.4f) — %d kept as is", "副作用なし (差 %.4f 以下) — %d 個そのまま"),
    ("Removed %d wrongly culled (%s%s) — %d remain, difference %.4f",
     "誤殺 %d 個を除外 (%s%s) — 残り %d 個で差 %.4f"),
    ("Clear All Culling", "カリング指定を全解除"),
    ("Clear camera culling on all objects (to redo the automatic culling)",
     "全オブジェクトのカメラカリング指定を解除する(自動カリングのやり直し用)"),
    ("Cleared %d", "解除 %d 個"),
    # --- VSE bridge (Output Properties, Sequencer, Image Editor, engine "VSE") ---------------
    ("Name the render output and share it with the VSE directly",
     "レンダーの出力先に名前を付けて、そのまま VSE に共有する"),
    ("Fit to Strips", "ストリップに合わせる"),
    ("Fit the frame range to the strips in the sequencer", "コマ範囲を、置いてあるストリップの総和へ合わせる"),
    ("No sequencer scene", "Sequencer のシーンがありません"),
    ("No strips", "ストリップがありません"),
    ("Edit in VSE", "VSE で編集"),
    ("Add what was rendered to the output path to the Sequencer and go there",
     "出力先に出来た物を Sequencer へ足して、そこへ移る"),
    ("Go to VSE", "VSE へ移る"),
    ("Switch to the Video Editing workspace", "Video Editing のワークスペースへ移る"),
    ("No files at the output path", "出力先にファイルがありません"),
    ("Render Output", "レンダー出力"),
    ("With VSE, the output settings are in the Render tab", "VSE では出力の項目もレンダーの欄にまとめています"),
    ("Cache growth stops below %d MB free "
     "(FALCON_VSE_MEM_SOFT_MB)",
     "太らせない下限: %d MB (FALCON_VSE_MEM_SOFT_MB)"),
    ("Cache growth limit: off (FALCON_VSE_MEM_SOFT_MB=0)", "太らせない下限: 切 (FALCON_VSE_MEM_SOFT_MB=0)"),
    ("Cut-only fast path: on", "切っただけの経路: 入"),
    ("Cut-only fast path: off (FALCON_VSE_FASTPATH=0)", "切っただけの経路: 切 (FALCON_VSE_FASTPATH=0)"),
    ("Output Name", "出力名"),
    ("Name of the files the render writes. "
     "When empty, the output path is used as is",
     "レンダーで書き出すファイルの名前。"
     "空なら出力先の経路をそのまま使う"),
    ("Share Output with VSE", "出力先を VSE に共有"),
    ("When a render finishes, add what was written to the output path to the "
     "Sequencer",
     "レンダーが終わったら、出力先に出来た物を Sequencer へ足す"),
    ("Browser Follows Output", "ブラウザを出力先に合わせる"),
    ("When Video Editing opens, point its File Browser at the render output folder. "
     "Off leaves Blender's default",
     "Video Editing を開いた時に、その画面のファイルブラウザを"
     "レンダーの出力先のフォルダにする。切ると Blender のまま"),
    ("Engine Before VSE", "VSE の前のエンジン"),
    ("Engine to return to when leaving a screen with a Sequencer",
     "Sequencer のある画面から出た時に戻すエンジン"),
    ("In VSE Screen", "VSE の画面に居る"),
    ("Whether the engine was switched to VSE because a screen with a Sequencer "
     "is shown",
     "Sequencer のある画面に出ているとして、エンジンを VSE にしているか"),
    # --- Output Properties > Encoding > Video ------------------------------------------------
    ("GPU Encoding", "GPUで書き出す"),
    # 元から英語だった説明に日本語を足した
    ("Encode H.264 and HEVC on the GPU (NVIDIA NVENC). Much faster than the "
     "CPU encoder; falls back to the CPU when no supported GPU is present",
     "H.264 と HEVC を GPU(NVIDIA NVENC)で符号化する。CPU の符号化器よりずっと速い。"
     "対応する GPU が無い時は CPU に戻る"),
    # --- export info line (C++: blenkernel/intern/falcon_export_info.cc, render/pipeline.cc) ---
    ("Fast path: used", "fast path: 使った"),
    ("Fast path: not used", "fast path: 使わない"),
    ("Encoder:", "符号化:"),
    ("stream copy", "そのままコピー"),
    ("not a movie export", "動画の書き出しではない"),
    # --- VSE: Render Properties > Source Media (falcon_vse_bridge/media_info.py) --------------
    ("Source Media", "素材の情報"),
    ("Select a strip in the Sequencer", "Sequencer でストリップを選んでください"),
    ("This strip has no source file", "このストリップには素材のファイルがありません"),
    ("File Count", "ファイル数"),
    ("Project Frame Rate", "プロジェクトのフレームレート"),
    ("Differs from the project frame rate", "プロジェクトのフレームレートと違います"),
    ("%d frames, %.2f s", "%d コマ・%.2f 秒"),
    ("Present", "あり"),
    ("%s (%d files)", "%s(%d ファイルの合計)"),
    ("Bit Depth", "ビット深度"),
    ("%d-bit", "%d ビット"),
    ("ffprobe not found: codec and bit depth are not shown",
     "ffprobe が見つからないので、コーデックとビット深度は出しません"),
    # --- File Browser: numbered image sequences as one item (makesrna rna_space.cc,
    #     bl_ui/space_filebrowser.py: the toggle in the top bar and the Filter popover) --------
    ("Group Image Sequences", "連番画像をまとめる"),
    ("Show a numbered image sequence as a single movie-like item instead of one item per frame",
     "連番画像を1コマずつ並べず、動画のような1つの項目として表示する"),
    # --- Language Switch (scripts/addons_core/falcon_language) ------------------------------
    ("Language Switch", "言語の切り替え"),
    ("Switch the interface language between English and a second language",
     "インターフェースの言語を英語と第2言語で切り替える"),
    ("Set Interface Language", "インターフェースの言語を設定"),
    ("Set the interface language", "インターフェースの言語を設定する"),
    ("Language identifier, e.g. en_US or ja_JP", "言語の識別子(例: en_US・ja_JP)"),
    ("Switch the interface to %s", "インターフェースを %s にする"),
    ("Language not available: %s", "この言語は使えません: %s"),
    ("Toggle Interface Language", "インターフェースの言語を切り替え"),
    ("Switch the interface language between English and the second language",
     "インターフェースの言語を英語と第2言語で切り替える"),
)


def translations():
    ja = {}
    for en, tr in JA:
        for ctxt in _CONTEXTS:
            ja.setdefault((ctxt, en), tr)
    return {"ja_JP": ja}


def register():
    try:
        bpy.app.translations.unregister(__name__)
    except Exception:
        pass
    bpy.app.translations.register(__name__, translations())


def unregister():
    bpy.app.translations.unregister(__name__)
