# SPDX-FileCopyrightText: 2026 Falcon Render
#
# SPDX-License-Identifier: Apache-2.0
#
# ★フレーム補間(x2)の Python 側 — 2026-09-13。
#
# 焼くのは 1 コマおき(飛ばすのは C++ の RE_RenderAnim・pipeline.cc の
# falcon_frame_interp_plan)。ここは**焼き終わった後に、抜けたコマを RIFE で作る**所。
#
# 根拠 [[測定/2026-09-12_1コマおき+補間は0910で成り立つか]]:
#   0910render/1 の本物 1351 コマで奇数コマを隠して RIFE v4.6(x2)で作り直すと、
#   草の最悪帯でも真値との PSNR 中央 22.69dB = 隣り合う本物コマ同士の差(14.60dB)より
#   8.1dB 真値に近い。沸きは増えない。代金はぼけ(速い帯で細部が 3 割落ちる)。
#   時間は 1.85 倍速。x4 は線上なので v1 では出さない。
#
# ★カットをまたいで補間しない。カットの正本は「カメラに束縛されたマーカーでカメラが
#   変わる所」で、C++ と同じ規則をここでも独立に組み立てて突き合わせる
#   ([[測定/2026-09-02_frucは動き補償でありRIFEと0.3dB差だがベンチにカットが入っていた]])。
#
# ★RIFE のバイナリはこの木にも Blender にも入っていない(MIT・Vulkan・別配布)。
#   場所は ①アドオン設定 ②$FALCON_RIFE_DIR ③<blender>/falcon/rife ④PATH の順に探す。
#   見つからない時は**レンダーが始まる前に**警告して補間を切る(終わってから気づくのが
#   一番高い)。

from __future__ import annotations

import os
import struct
import subprocess
import sys
import time
import zlib

import bpy

MODE_ENV = "FALCON_FRAME_INTERP"
RIFE_DIR_ENV = "FALCON_RIFE_DIR"
RIFE_EXE = "rife-ncnn-vulkan"
RIFE_MODEL = "rife-v4.6"
INTERP_TAG = "rife-x2"

_FACTOR = {'OFF': 1, 'X2': 2}

# 起動時に環境変数が在ったか。在れば以後ずっと env が勝つ(0 も 2 も)。
_ENV_AT_START = os.environ.get(MODE_ENV)

# このレンダーで実際に焼かれたコマ(render_pre が積む)。
_rendered_frames = []
# render_init が決めた、このレンダーの実効倍率(0 = このレンダーでは補間しない)。
_active_factor = 1


def _log(msg):
    print("[frame interp] %s" % msg, flush=True)


# ---------------------------------------------------------------- 倍率

def scene_factor(scene):
    cscene = getattr(scene, "cycles", None)
    if cscene is None:
        return 1
    return _FACTOR.get(getattr(cscene, "falcon_frame_interp", 'OFF'), 1)


def effective_factor(scene):
    """env が在れば env が勝つ(0 も 2 も)。無ければ場面の設定。"""
    if _ENV_AT_START is not None:
        try:
            n = int(_ENV_AT_START)
        except ValueError:
            n = 1
        return n if n >= 2 else 1
    return scene_factor(scene)


# ---------------------------------------------------------------- RIFE を探す

def _rife_in(dirpath):
    if not dirpath:
        return None
    exe = os.path.join(dirpath, RIFE_EXE)
    if os.path.isfile(exe) and os.access(exe, os.X_OK):
        return exe
    return None


def find_rife():
    """(exe, model_dir) を返す。見つからなければ (None, 理由)。"""
    tried = []

    prefs = None
    try:
        prefs = bpy.context.preferences.addons[__package__].preferences
    except Exception:
        pass
    user_dir = getattr(prefs, "falcon_rife_dir", "") if prefs else ""
    if user_dir:
        user_dir = bpy.path.abspath(user_dir)

    for d in (user_dir, os.environ.get(RIFE_DIR_ENV, ""),
              os.path.join(os.path.dirname(bpy.app.binary_path), "falcon", "rife")):
        if not d:
            continue
        tried.append(d)
        exe = _rife_in(d)
        if exe:
            return exe, _model_dir(os.path.dirname(exe))

    import shutil
    exe = shutil.which(RIFE_EXE)
    if exe:
        return exe, _model_dir(os.path.dirname(os.path.realpath(exe)))
    tried.append("PATH")

    return None, "%s が見つからない (探した所: %s)" % (RIFE_EXE, ", ".join(tried))


def _model_dir(base):
    """rife-v4.6 の重みの置き場。無ければ同じ所の rife-* を 1 つ。"""
    d = os.path.join(base, RIFE_MODEL)
    if os.path.isdir(d):
        return d
    try:
        for name in sorted(os.listdir(base)):
            if name.startswith("rife-") and os.path.isdir(os.path.join(base, name)):
                return os.path.join(base, name)
    except OSError:
        pass
    return None


# ---------------------------------------------------------------- カットと焼くコマ

def camera_at(scene, frame):
    """BKE_scene_camera_switch_find と同じ規則(scene.cc:2396)。"""
    cam = None
    best = None
    first_cam = None
    first_frame = None
    for m in scene.timeline_markers:
        if m.camera is None or m.camera.hide_render:
            continue
        if m.frame <= frame and (best is None or m.frame > best):
            cam = m.camera
            best = m.frame
        if first_frame is None or m.frame < first_frame:
            first_frame = m.frame
            first_cam = m.camera
    return cam if cam is not None else first_cam


def frame_plan(scene, sfra, efra, step, factor):
    """(全コマ, 焼くコマ, カットのコマ) — C++ の falcon_frame_interp_plan と同じ規則。"""
    allf = []
    keep = set()
    cut = set()
    next_keep = sfra
    prev_cam = None
    last = sfra
    f = sfra
    while f <= efra:
        cam = camera_at(scene, f)
        if f > sfra and cam is not prev_cam:
            keep.add(f - step)
            cut.add(f - step)
            cut.add(f)
            next_keep = f
        prev_cam = cam
        if f >= next_keep:
            keep.add(f)
            next_keep = f + factor * step
        allf.append(f)
        last = f
        f += step
    keep.add(last)
    return allf, keep, cut


def interp_pairs(allf, keep, cut, step):
    """[(a, b, f, t)] — a と b は本物・f は作るコマ・t は 0..1 の時刻。"""
    ks = sorted(keep)
    pairs = []
    for a, b in zip(ks, ks[1:]):
        if b - a <= step:
            continue
        # カットを挟む対は作らない(この規則では起きないはずなので、起きたら止める)
        for c in cut:
            if a < c <= b:
                raise RuntimeError(
                    "カットのコマ %d が本物コマ %d..%d の間にある = 規則が壊れている" % (c, a, b))
        f = a + step
        while f < b:
            pairs.append((a, b, f, (f - a) / float(b - a)))
            f += step
    return pairs


# ---------------------------------------------------------------- PNG のメタデータ

def png_add_text(path, key, value):
    """PNG に tEXt を 1 つ足す(PIL はこの Python に入っていない)。"""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return False
    payload = key.encode("latin-1") + b"\x00" + value.encode("latin-1")
    chunk = (struct.pack(">I", len(payload)) + b"tEXt" + payload +
             struct.pack(">I", zlib.crc32(b"tEXt" + payload) & 0xFFFFFFFF))
    # IHDR(8 + 4 + 4 + 13 + 4 = 33 バイト)の直後へ入れる
    pos = 8 + 4 + 4 + struct.unpack(">I", data[8:12])[0] + 4
    with open(path, "wb") as fh:
        fh.write(data[:pos] + chunk + data[pos:])
    return True


# ---------------------------------------------------------------- ハンドラ

def _gate(scene):
    """このレンダーで補間を走らせてよいか。走らせない理由は 1 行出す。"""
    factor = effective_factor(scene)
    if factor < 2:
        return 1

    r = scene.render
    if r.is_movie_format:
        _log("切る: 動画の書き出しには対応していない (連番画像のみ)")
        return 1
    if r.image_settings.file_format != 'PNG':
        _log("切る: 出力が %s。v1 は PNG の連番だけ" % r.image_settings.file_format)
        return 1
    if not r.use_file_extension:
        _log("切る: 「ファイル拡張子」が切られていると出力先を辿れない")
        return 1

    exe, model = find_rife()
    if exe is None:
        _log("切る: %s" % model)
        return 1
    if model is None:
        _log("切る: %s の重み (%s) が %s の隣に無い" % (RIFE_EXE, RIFE_MODEL, os.path.dirname(exe)))
        return 1
    return factor


@bpy.app.handlers.persistent
def render_init(scene, _depsgraph=None):
    global _rendered_frames, _active_factor
    _rendered_frames = []
    _active_factor = _gate(scene)

    # C++ (RE_RenderAnim) はこの環境変数しか見ない。env が在る時は触らない
    # (利用者が置いた env が勝つ)。
    if _ENV_AT_START is None:
        os.environ[MODE_ENV] = str(_active_factor)


@bpy.app.handlers.persistent
def render_pre(scene, _depsgraph=None):
    _rendered_frames.append(scene.frame_current)


@bpy.app.handlers.persistent
def render_complete(scene, _depsgraph=None):
    if _active_factor < 2:
        return

    r = scene.render
    sfra, efra = scene.frame_start, scene.frame_end
    step = max(scene.frame_step, 1)
    factor = _active_factor

    try:
        allf, keep, cut = frame_plan(scene, sfra, efra, step, factor)
        pairs = interp_pairs(allf, keep, cut, step)
    except RuntimeError as e:
        _log("止める: %s" % e)
        return

    # 温めコマ(sfra より前)を落として、焼かれたコマと突き合わせる。
    real = sorted(set(f for f in _rendered_frames if f >= sfra))
    if real != sorted(keep):
        _log("何もしない: 焼かれたコマ %d 個が予定 %d 個と違う "
             "(1 枚だけのレンダーか、途中で止めたか)" % (len(real), len(keep)))
        return

    exe, model = find_rife()
    if exe is None:
        _log("止める: %s" % model)
        return

    # 本物であるべきコマが揃っているか。欠けていたら黙って補間しない。
    paths = {f: r.frame_path(frame=f) for f in allf}
    missing = [f for f in sorted(keep) if not os.path.isfile(paths[f])]
    if missing:
        _log("止める: 本物のコマが %d 枚無い (最初の 5 つ: %s)" %
             (len(missing), missing[:5]))
        return

    _log("x%d: %d 枚を %s で作る" % (factor, len(pairs), os.path.basename(exe)))
    t0 = time.time()
    made = 0
    for a, b, f, t in pairs:
        out = paths[f]
        cmd = [exe, "-m", model, "-0", paths[a], "-1", paths[b],
               "-o", out, "-s", "%.6f" % t]
        p = subprocess.run(cmd, capture_output=True)
        if p.returncode != 0 or not os.path.isfile(out):
            _log("止める: rife rc=%d\n%s" % (p.returncode, p.stderr.decode(errors="replace")[-600:]))
            return
        png_add_text(out, "falcon.interp", INTERP_TAG)
        png_add_text(out, "falcon.interp.from", "%d,%d @ %.6f" % (a, b, t))
        made += 1
        print("[frame interp] %04d <- %d,%d (t=%.3f)" % (f, a, b, t), flush=True)
    _log("x%d: %d 枚できた (%.1f 秒・%.3f 秒/枚)" %
         (factor, made, time.time() - t0, (time.time() - t0) / max(made, 1)))


_handlers = (
    (bpy.app.handlers.render_init, render_init),
    (bpy.app.handlers.render_pre, render_pre),
    (bpy.app.handlers.render_complete, render_complete),
)


def register():
    for lst, fn in _handlers:
        if fn not in lst:
            lst.append(fn)


def unregister():
    for lst, fn in _handlers:
        if fn in lst:
            lst.remove(fn)
