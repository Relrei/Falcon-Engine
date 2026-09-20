# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Video Editing では窓のシーンを Sequencer のシーンに揃える (2026-09-20)。

作者 2026-09-20「シーケンサーは 300 フレームに設定してるのに、フレームレンダーが連携できてない」
「ストリップに合わせるでも変わらない」「フレーム再生速度変更しても無視される」。

★正体: 9-12 に「ストリップは専用シーン `VSE` へ置く」にした時、Video Editing の
  `workspace.sequencer_scene` だけを `VSE` に向け、**窓のシーンは 3D のまま**にした。
  Blender 5.x では
    - Sequencer・その下の再生の欄(Start/End)・Sequencer から始めた再生(fps)= `sequencer_scene`
    - プロパティ(範囲・Frame Rate・出力)・F12 / Ctrl+F12・Render メニューの Render Animation = 窓のシーン
  (`render_internal.cc` は `use_sequencer_scene` が立った時だけ sequencer scene を焼く・
   `screen_ops.cc` の `start_playback` は Sequencer からだと sequencer scene で回す)。
  さらに自動切り替えが 3D のシーンのエンジンを VSE にするので、Video Editing で Ctrl+F12 を
  押すと **3D のシーンを空の VSE エンジンで 250 コマ焼く(真っ黒)**、自動共有がそれを
  編集の上の段へ足す(実測 2026-09-20)。
★本家の既定の流れ: Sequencer のヘッダの「New」(`scene.new_sequencer_scene`)は新しいシーンを
  作って**窓のシーンもそれにする**(5.0 のリリースノート「the active scene … will update to match」・
  素の 5.2.0 で確かめた)。プロパティの出力の項目が sequencer scene に効くのはこの形の時だけで、
  別のシーンにしてある時は「Render Sequencer Animation」(Ctrl+Alt+F12)で分けて焼く設計。
  ⇒ Video Editing(Sequencer があって 3D ビューの無い画面)では、窓のシーンを sequencer scene に
     揃える。入ったら揃え、出たら入る前のシーンへ戻す。3D の画面(Layout など)は今までどおり。

- 揃えるのは「Sequencer があって 3D ビューが無い」画面だけ。Layout の一角を Sequencer にした
  画面は 3D ビューが 3D のシーンを見ているので触らない(本家の「別にする」使い方)
- 1 つのエリアを最大化している間は、そのワークスペースの元の画面で決める(出た扱いにしない)
- 入っている間に作者が窓のシーンを替えたら(Sync Scene Time を含む)、出入りするまで触らない
- 出る時に戻すのは、こちらが揃えた窓で、まだ揃えたシーンのままの時だけ
- 入る前のシーンの名前は揃えた先のシーンに置く(保存される = 開き直しても戻れる)
- レンダーの最中と再生の最中は替えない(終わってから見回りが拾う)
- -b(背景)では何もしない
- 自動切り替え(`FALCON_VSE_AUTO_ENGINE`)は「Sequencer が見ているシーンを窓に出している時」だけ
  VSE にする(`shows_own_scene`)。3D のシーンを VSE エンジンにしない = Ctrl+F12 が黒くならない

`FALCON_VSE_FOLLOW_SEQUENCER_SCENE=0` で前の挙動(窓のシーンは替えない・自動切り替えも前の判定)。
"""

import os

import bpy

ENV = "FALCON_VSE_FOLLOW_SEQUENCER_SCENE"

# 揃えた先のシーンに置く: 入る前の窓のシーンの名前(RNA ではない ID プロパティ・保存される)。
RETURN_KEY = "falcon_vse_return_scene"

# 見回りの間隔(秒)。窓の形が変わった時だけ突き合わせる。
_WATCH_INTERVAL = 0.25

# window.as_pointer() -> {"state": "followed" | "user", "scene": 揃えた先の名前, "ret": 戻す先の名前}
_windows = {}
_last_sig = None
_dirty = True
_msgbus_owner = object()


def enabled():
    """呼ばれるたびに環境変数を読む(門が両側を 1 つのプロセスで測れるように)。"""
    return os.environ.get(ENV, "1").strip().lower() not in ("", "0", "off", "false", "no")


# -----------------------------------------------------------------------------
# 判定
# -----------------------------------------------------------------------------

def _areas(screen):
    return {a.type for a in (getattr(screen, "areas", None) or ())}


def _screen_follows(screen):
    types = _areas(screen)
    return 'SEQUENCE_EDITOR' in types and 'VIEW_3D' not in types


def window_follows(window):
    """その窓で窓のシーンを Sequencer のシーンに揃えるか。数えない窓(仮の窓)は None。"""
    screen = getattr(window, "screen", None)
    if screen is None or getattr(screen, "is_temporary", False):
        return None
    if _screen_follows(screen):
        return True
    if getattr(screen, "show_fullscreen", False):
        # 最大化の間は元の画面で決める(`_window_in_vse` と同じ考え)。
        workspace = getattr(window, "workspace", None)
        for other in (getattr(workspace, "screens", None) or ()):
            if other != screen and _screen_follows(other):
                return True
    return False


def _sequencer_scene(window):
    workspace = getattr(window, "workspace", None)
    return getattr(workspace, "sequencer_scene", None) if workspace is not None else None


def shows_own_scene(window):
    """窓のシーンが、その窓の Sequencer が見ているシーンか(自動切り替えの判定に使う)。

    `FALCON_VSE_FOLLOW_SEQUENCER_SCENE=0` の時は常に True(前の判定)。
    sequencer_scene がまだ空の時も True(本家の Sequencer は何も出さない・前の判定のまま)。
    """
    if not enabled():
        return True
    seq = _sequencer_scene(window)
    return seq is None or seq == getattr(window, "scene", None)


def source_scene(scene):
    """揃えた先(Sequencer のシーン)なら、入る前の窓のシーン(= レンダーの出力を持つ側)。

    Video Editing の中から「レンダー出力を足す」「ブラウザを出力先へ」を呼ぶと、窓のシーンは
    もう Sequencer のシーンなので、そのままだと**書き出し(編集結果)を素材として足してしまう**。
    """
    if scene is None or not enabled():
        return scene
    try:
        name = scene.get(RETURN_KEY)
    except Exception:  # noqa: BLE001
        return scene
    if not name:
        return scene
    other = bpy.data.scenes.get(name)
    return other if (other is not None and other != scene) else scene


# -----------------------------------------------------------------------------
# 揃える / 戻す
# -----------------------------------------------------------------------------

def _busy():
    try:
        if bpy.app.is_job_running('RENDER'):
            return True
    except Exception:  # noqa: BLE001
        pass
    manager = getattr(bpy.context, "window_manager", None)
    for window in (getattr(manager, "windows", None) or ()):
        screen = getattr(window, "screen", None)
        if screen is not None and getattr(screen, "is_animation_playing", False):
            return True
    return False


def _remember_return(seq, name):
    """入る前のシーンの名前を揃えた先へ置く(開き直しても戻れるように)。"""
    if not name or seq.get(RETURN_KEY) == name:
        return
    try:
        seq[RETURN_KEY] = name
    except Exception as ex:  # noqa: BLE001  書けないシーン(リンク)でも揃えはする
        print("falcon_vse_bridge:", ex)


def _set_scene(window, scene):
    if window.scene != scene:
        window.scene = scene


def apply(windows):
    """窓ごとに揃える / 戻す。行った事の一覧 [(窓, 'follow' | 'return', 前, 後)] を返す。

    ★窓を引数で受ける(門が作り物の窓で確かめられるように)。背景かは見ない。
    """
    done = []
    alive = set()
    for window in windows:
        key = window.as_pointer()
        alive.add(key)
        inside = window_follows(window)
        if inside is None:
            continue  # 仮の窓(レンダー結果・設定)は数えない
        entry = _windows.get(key)
        scene = getattr(window, "scene", None)
        if inside:
            seq = _sequencer_scene(window)
            if seq is None:
                continue  # まだ空(`ensure_sequencer_scene` が埋めてから)
            if entry is None:
                # 入った。
                if scene == seq:
                    # 最初から揃っている(Video Editing で保存したファイル・本家の New の流れ)。
                    ret = seq.get(RETURN_KEY) or None
                    _windows[key] = {"state": "followed", "scene": seq.name, "ret": ret}
                    continue
                prev = scene.name if scene is not None else None
                _remember_return(seq, prev)
                _set_scene(window, seq)
                _windows[key] = {"state": "followed", "scene": seq.name, "ret": prev}
                done.append((window, "follow", prev, seq.name))
            elif entry["state"] == "followed" and scene is not None:
                if scene.name == entry["scene"]:
                    if seq != scene:
                        # Sequencer のヘッダで見るシーンを替えた。窓のシーンも付いていく。
                        _remember_return(seq, entry["ret"])
                        _set_scene(window, seq)
                        entry["scene"] = seq.name
                        done.append((window, "follow", scene.name, seq.name))
                elif seq == scene:
                    # 両方替わった(Sequencer のヘッダの New は窓のシーンも替える)。
                    entry["scene"] = seq.name
                else:
                    # 作者が窓のシーンを替えた(Sync Scene Time を含む)。出入りするまで触らない。
                    entry["state"] = "user"
            continue
        # 出た。
        if entry is None:
            continue
        del _windows[key]
        if entry["state"] != "followed" or scene is None or scene.name != entry["scene"]:
            continue
        ret = bpy.data.scenes.get(entry["ret"]) if entry["ret"] else None
        if ret is None or ret == scene:
            continue
        _set_scene(window, ret)
        done.append((window, "return", scene.name, ret.name))
    for key in list(_windows):
        if key not in alive:
            del _windows[key]
    return done


def _signature(windows):
    sig = []
    for window in windows:
        screen = getattr(window, "screen", None)
        scene = getattr(window, "scene", None)
        seq = _sequencer_scene(window)
        sig.append((
            window.as_pointer(),
            screen.as_pointer() if screen is not None else 0,
            scene.as_pointer() if scene is not None else 0,
            seq.as_pointer() if seq is not None else 0,
            len(getattr(screen, "areas", ()) or ()),
        ))
    return tuple(sig)


def _windows_now():
    manager = getattr(bpy.context, "window_manager", None)
    return list(getattr(manager, "windows", ()) or ())


def run():
    """GUI の時だけ。今の窓と突き合わせる。"""
    global _last_sig, _dirty
    if bpy.app.background or not enabled():
        return []
    if _busy():
        _dirty = True
        return []
    windows = _windows_now()
    done = apply(windows)
    _last_sig = _signature(_windows_now())
    _dirty = False
    if done:
        # エンジンの自動切り替えにも、窓のシーンが替わったことを突き合わせさせる。
        try:
            from . import _auto_engine_poke
            _auto_engine_poke()
        except Exception as ex:  # noqa: BLE001
            print("falcon_vse_bridge:", ex)
    return done


def _deferred():
    try:
        run()
    except Exception as ex:  # noqa: BLE001  UI を止めない
        print("falcon_vse_bridge:", ex)
    return None


def poke(*_args):
    """次の拍で突き合わせる(msgbus・読み込み・Ctrl+Z から)。★通知の最中に窓を替えない。"""
    global _dirty
    if bpy.app.background:
        return
    _dirty = True
    if not bpy.app.timers.is_registered(_deferred):
        bpy.app.timers.register(_deferred, first_interval=0.0)


def _watch():
    """見回り。窓の形が変わった時(Ctrl+PageUp/Down・「+」から足した・窓を閉じた等)だけ突き合わせる。"""
    try:
        if not bpy.app.background and enabled():
            if _dirty or _signature(_windows_now()) != _last_sig:
                run()
    except Exception as ex:  # noqa: BLE001  見回りは止めない
        print("falcon_vse_bridge:", ex)
    return _WATCH_INTERVAL


def _subscribe():
    if bpy.app.background:
        return
    try:
        bpy.msgbus.clear_by_owner(_msgbus_owner)
        for key in ((bpy.types.Window, "workspace"),
                    (bpy.types.Window, "scene"),
                    (bpy.types.WorkSpace, "sequencer_scene"),
                    (bpy.types.Area, "type"),
                    (bpy.types.Area, "ui_type")):
            bpy.msgbus.subscribe_rna(key=key, owner=_msgbus_owner, args=(), notify=poke)
    except Exception as ex:  # noqa: BLE001
        print("falcon_vse_bridge:", ex)
    if not bpy.app.timers.is_registered(_watch):
        bpy.app.timers.register(_watch, first_interval=_WATCH_INTERVAL, persistent=True)
    poke()


@bpy.app.handlers.persistent
def _on_load_post(*_args):
    # ★購読はファイルを読み込むと消える。覚えも窓ごと作り直す。
    _windows.clear()
    _subscribe()


@bpy.app.handlers.persistent
def _on_undo(*_args):
    poke()


_handlers = (
    ("load_post", _on_load_post),
    ("undo_post", _on_undo),
    ("redo_post", _on_undo),
)


def register():
    for name, function in _handlers:
        handler = getattr(bpy.app.handlers, name)
        if function not in handler:
            handler.append(function)
    _subscribe()


def unregister():
    global _last_sig, _dirty
    if not bpy.app.background:
        try:
            bpy.msgbus.clear_by_owner(_msgbus_owner)
        except Exception as ex:  # noqa: BLE001
            print("falcon_vse_bridge:", ex)
    for function in (_deferred, _watch):
        if bpy.app.timers.is_registered(function):
            bpy.app.timers.unregister(function)
    for name, function in _handlers:
        handler = getattr(bpy.app.handlers, name)
        if function in handler:
            handler.remove(function)
    _windows.clear()
    _last_sig, _dirty = None, True
