# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""出力先の「出力名」と、(既定では切ってある)VSE への受け渡し。

★2026-09-10 作者指示で **「レンダー結果を VSE へ送る」一式は既定で出さない**。
  「なぜかレンダリングしてもいないのにあったり古いのを参照したりで使い物にならない」。
  正体は `output_target()` が **出力先に在るファイルを見ているだけ**で、
  「今回のレンダーが書いた物か」を区別していない点 — 前のレンダーの残りや
  他所から置いたファイルが、そのまま「レンダー結果」として出ていた。

  ★2026-09-10(同日・後):作者指示で**手で押す口も既定 ON へ戻した**。
  古い物を掴む問題は「自動の側だけ時刻で落とす」で解けたので、
  **押した時だけ動く口を隠す理由が無くなった**:

    自動(レンダー完了で足す)  今回のレンダーが書いたコマだけ  `FALCON_VSE_AUTO_SHARE=0` で止まる
    手で押す口(ボタン・追加)   新旧おかまいなし               `FALCON_VSE_BRIDGE=0` で消える

  ★2026-09-12:**ストリップはレンダーしたシーンに置かない**。専用のシーン `VSE` へ置く。
  `render.use_sequencer` は既定 ON なので、ストリップが入ったシーンの次の F12 は
  **Cycles を走らせず Sequencer(= さっき焼いた PNG)を焼き直す**
  (作者 2026-09-10「レンダリングしてもいないのにある・古いのを参照する」の後半)。
  Blender 5.x の Sequencer が読むのは `workspace.sequencer_scene` なので、
  置き場を分けても VSE 側の見え方は変わらない。`FALCON_VSE_SHARE_SCENE=0` で前の挙動。

既定でも残る物(VSE とは無関係なので落とさない):
- 「出力名」に名前を入れると、出力先の経路のディレクトリ部分 + その名前へ書く。
  連番の番号と拡張子は Blender の規則どおり。
- 書き出しで何が効いたかの1行(`render.falcon_last_export_info`)。

bpy の公開 API だけで書いてある(素の Blender 5.2 でも動く)。
"""

bl_info = {
    "name": "Falcon VSE Bridge",
    "author": "Falcon Render",
    "version": (1, 0, 0),
    "blender": (5, 2, 0),
    "location": "Properties > Output > Output / Image Editor Header",
    "description": "Name the render output and share it with the VSE directly",
    "category": "Sequencer",
}

import contextlib
import os
import time

import bpy
from bpy.app.handlers import persistent
from bpy.app.translations import pgettext_iface as iface_
from bpy.props import BoolProperty, EnumProperty, StringProperty
from bpy.types import Operator, Panel, RenderEngine


# -----------------------------------------------------------------------------
# 出力名
# -----------------------------------------------------------------------------
#
# 「出力名」が入っていたら、レンダーの間だけ `render.filepath` を
#   <出力先のディレクトリ部分> + <出力名>
# に差し替える。番号と拡張子は Blender が今までどおり付ける。
#
# ★差し替えたものは必ず戻す(render_complete / render_cancel / アドオンの解除)。
#   .blend に合成後の値を残さない。

# 差し替える前の `render.filepath`。{シーン名: 元の値}
_saved_filepath = {}

# 画像の拡張子(既に付いていたら「足す」でなく「差し替える」側に回るもの)。
_IMAGE_EXTS = (
    ".png", ".jpg", ".jpeg", ".exr", ".tif", ".tiff", ".tga", ".bmp", ".hdr",
    ".cin", ".dpx", ".jp2", ".j2c", ".webp", ".rgb", ".sgi", ".psd",
)

# 同じ形式の別綴り。Blender は一覧の**どれか**に一致すれば足さない。
_EXT_ALIASES = {
    ".jpg": (".jpeg",),
    ".jpeg": (".jpg",),
    ".tif": (".tiff",),
    ".tiff": (".tif",),
    ".jp2": (".j2c",),
    ".j2c": (".jp2",),
    ".mpg": (".mpeg",),
    ".mpeg": (".mpg",),
    ".dvd": (".vob", ".mpg", ".mpeg"),
    ".ogv": (".ogg",),
    ".ogg": (".ogv",),
}


def bridge_enabled():
    """「レンダー結果を VSE へ送る」一式を出すか。**既定は出さない**(2026-09-10 作者)。

    ★呼ばれるたびに環境変数を読む(取り込んだ時に1回だけ読まない)。
      門が走っている最中に `os.environ` を倒して両側を測れるようにするため。
    """
    return os.environ.get("FALCON_VSE_BRIDGE", "1").strip().lower() not in (
        "", "0", "off", "false", "no",
    )


def auto_share_enabled():
    """レンダーが終わったら連番を **1 本のストリップ** にして足すか。既定 ON。

    ★UI 一式(`bridge_enabled()`)とは別の口。作者 2026-09-10:
      「連番として大量に並べず 1 本の動画として自動でまとめたい」。
      ボタンや共有チェックは出さないまま、**まとめる所だけ**を既定で効かせる。
      古い物を掴む欠点は `_render_started` の時刻で落とす。
    """
    return os.environ.get("FALCON_VSE_AUTO_SHARE", "1").strip().lower() not in (
        "", "0", "off", "false", "no",
    )


def share_scene_enabled():
    """ストリップを**専用のシーン**へ置くか。既定 ON。

    ★2026-09-12。レンダーしたシーン自身に置くと、`render.use_sequencer`(既定 ON)の
      せいで**次の F12 が Cycles を走らせず、さっき焼いた PNG を焼き直す**。
      `FALCON_VSE_SHARE_SCENE=0` で前の挙動(同じシーンへ置く)に戻る。
    """
    return os.environ.get("FALCON_VSE_SHARE_SCENE", "1").strip().lower() not in (
        "", "0", "off", "false", "no",
    )


# ストリップを置く専用シーンの名前。
SHARE_SCENE_NAME = "VSE"

# 専用シーンへ写す `render` の項目。**再生に関わる物だけ**(出力先や engine は写さない
# — 写すと「VSE のシーンをうっかりレンダーしたら本番と同じ所へ書く」になる)。
_SHARE_SCENE_RENDER_ATTRS = (
    "fps", "fps_base", "resolution_x", "resolution_y", "resolution_percentage",
)


def _copy_playback_settings(src_scene, dst_scene):
    for attr in _SHARE_SCENE_RENDER_ATTRS:
        try:
            setattr(dst_scene.render, attr, getattr(src_scene.render, attr))
        except Exception:  # noqa: BLE001  写せない項目があっても止めない
            pass


def share_scene(scene, create=True):
    """ストリップを置くシーン。既定は専用の `VSE`(無ければ作る)。

    `FALCON_VSE_SHARE_SCENE=0` のときは `scene` をそのまま返す(前の挙動)。
    `create=False` なら、まだ無い時に作らず None を返す。
    """
    if scene is None:
        return None
    if not share_scene_enabled():
        return scene
    existing = bpy.data.scenes.get(SHARE_SCENE_NAME)
    if existing is not None:
        return existing
    if not create:
        return None
    try:
        made = bpy.data.scenes.new(SHARE_SCENE_NAME)
    except Exception as ex:  # noqa: BLE001  作れなければ前の挙動へ落とす
        print("falcon_vse_bridge:", ex)
        return scene
    # ★fps と解像度だけ写す。出力先を写すと、本番の出力を上書きする道が出来る。
    _copy_playback_settings(scene, made)
    # ★シーケンサを先に作る。無いまま画像をドロップすると素の 5.2.1 でも落ちる
    #   (all_strips_from_context が ed == nullptr のまま query_all_strips へ渡す)。
    try:
        made.sequence_editor_create()
    except Exception as ex:  # noqa: BLE001  作れなくてもシーンは返す
        print("falcon_vse_bridge:", ex)
    return made


def output_name(scene):
    """「出力名」。空(あるいは未登録)なら ""。"""
    return (getattr(scene, "falcon_output_name", "") or "").strip()


def _dir_part(path):
    """経路のディレクトリ部分(区切りまで)。無ければ ""。"""
    index = max(path.rfind("/"), path.rfind("\\"))
    return path[:index + 1] if index >= 0 else ""


def composed_filepath(scene):
    """出力名を入れた `render.filepath`。出力名が空なら None。"""
    name = output_name(scene)
    if not name:
        return None
    return _dir_part(scene.render.filepath) + name


# --- 番号と拡張子(BLI_path_frame / do_ensure_image_extension と同じ規則)-----

def _ensure_digits(name, digits):
    """ファイル名の側に `#` が1つも無ければ digits 個足す(ensure_digits)。"""
    base = name[max(name.rfind("/"), name.rfind("\\")) + 1:]
    if "#" in base:
        return name
    return name + "#" * digits


def _hash_span(name):
    """ファイル名の側の**最後の** `#` の並び (start, end)。無ければ None。"""
    start = end = None
    index = 0
    length = len(name)
    while index < length:
        char = name[index]
        if char in "/\\":
            start = end = None
        elif char == "#":
            start = index
            stop = index + 1
            while stop < length and name[stop] == "#":
                stop += 1
            end = stop
            index = stop - 1
        index += 1
    if end is None:
        return None
    return (start, end)


def _path_frame(name, frame, digits=4):
    name = _ensure_digits(name, digits)
    span = _hash_span(name)
    if span is None:
        return name
    start, end = span
    return name[:start] + "%0*d" % (end - start, frame) + name[end:]


def _path_frame_range(name, start_frame, end_frame, digits=4):
    name = _ensure_digits(name, digits)
    span = _hash_span(name)
    if span is None:
        return name
    start, end = span
    width = end - start
    return (name[:start]
            + "%0*d-%0*d" % (width, start_frame, width, end_frame)
            + name[end:])


def _has_ext(name, ext):
    if not ext:
        return True
    lower = name.lower()
    if lower.endswith(ext.lower()):
        return True
    return any(lower.endswith(alias) for alias in _EXT_ALIASES.get(ext.lower(), ()))


def _with_ext(name, ext):
    """拡張子を足す。既に別の画像の拡張子が付いていたら差し替える。"""
    if _has_ext(name, ext):
        return name
    lower = name.lower()
    for known in _IMAGE_EXTS:
        if lower.endswith(known):
            return name[:len(name) - len(known)] + ext
    return name + ext


def preview_path(scene, frame=None):
    """実際に書かれる経路。出力名が空なら `render.frame_path()` そのまま。

    ★描画中は `render.filepath` を書けないので、`frame_path()` の結果から
      ディレクトリ(テンプレート展開・絶対化まで済んでいる)だけ借りて、
      ファイル名の側を同じ規則で自分で組む。門で実物と突き合わせてある。
    """
    render = scene.render
    if frame is None:
        frame = int(scene.frame_start)
    try:
        real = render.frame_path(frame=frame)
    except Exception:
        return None
    real = os.path.normpath(real)

    name = output_name(scene)
    if not name:
        return real

    directory = os.path.dirname(real)
    use_ext = bool(render.use_file_extension)

    if render.is_movie_format:
        # ★`render.file_extension` は動画では静止画側の拡張子を返す(実測)。
        #   容れ物の拡張子は `frame_path()` の結果から取る。
        ext = os.path.splitext(os.path.basename(real))[1] if use_ext else ""
        base = name
        if use_ext:
            if not _has_ext(base, ext):
                base = _path_frame_range(
                    base, int(scene.frame_start), int(scene.frame_end))
                base += ext
        elif _hash_span(base) is not None:
            base = _path_frame_range(
                base, int(scene.frame_start), int(scene.frame_end))
    else:
        ext = render.file_extension or ""
        base = _path_frame(name, int(frame))
        if use_ext:
            base = _with_ext(base, ext)

    return os.path.normpath(os.path.join(directory, base))


# --- 差し替えと、戻し ---------------------------------------------------------

def apply_output_name(scene):
    """`render.filepath` を出力名を入れた値へ差し替える。差し替えたら True。"""
    if scene is None:
        return False
    composed = composed_filepath(scene)
    if composed is None:
        return False
    if scene.name in _saved_filepath:
        # 既に掛かっている(まだ戻していない)。二重に掛けない。
        return False
    original = scene.render.filepath
    _saved_filepath[scene.name] = original
    try:
        scene.render.filepath = composed
    except Exception as ex:  # noqa: BLE001
        _saved_filepath.pop(scene.name, None)
        print("falcon_vse_bridge:", ex)
        return False
    return True


def restore_output_name(scene):
    """差し替えた `render.filepath` を元へ戻す。"""
    if scene is None:
        return
    original = _saved_filepath.pop(scene.name, None)
    if original is None:
        return
    try:
        scene.render.filepath = original
    except Exception as ex:  # noqa: BLE001
        print("falcon_vse_bridge:", ex)


@contextlib.contextmanager
def output_name_applied(scene):
    """この中だけ `render.filepath` を出力名を入れた値にする。"""
    mine = apply_output_name(scene)
    try:
        yield
    finally:
        if mine:
            restore_output_name(scene)


# -----------------------------------------------------------------------------
# 出力先の判定
# -----------------------------------------------------------------------------

# レンダー中にディスクへ書かれたコマ数。{シーン名: 本数}
#
# ★コマ番号は取らない。Falcon の非同期保存では render_write が1コマ遅れて呼ばれ、
#   その時点の `frame_current` は既に次のコマを指している(素の Blender では一致する)。
#   「書かれたか」だけを見て、どのコマかは出力先の実物で決める。
_written = {}

# レンダーが始まった時刻。{シーン名: epoch 秒}
#
# ★これが「今回のレンダーが書いた物か」を決める唯一の材料。作者 2026-09-10
#   「レンダリングしてもいないのにあったり古いのを参照したりで使い物にならない」
#   の正体は、出力先に**在る**ファイルを見ているだけだったこと。
_render_started = {}

# 時計のずれとファイルシステムの粒度のぶんの余裕(秒)。
_MTIME_SLACK = 2.0


def _abspath(path):
    try:
        return os.path.normpath(bpy.path.abspath(path))
    except Exception:
        return os.path.normpath(path)


def _frame_path(render, frame):
    """そのコマの出力ファイル。取れなければ None。"""
    try:
        return _abspath(render.frame_path(frame=frame))
    except Exception:
        return None


def _written_by_this_render(scene, path):
    """そのファイルが今回のレンダーで書かれた物か。開始時刻が無ければ全部通す。"""
    started = _render_started.get(scene.name)
    if started is None:
        return True
    try:
        return os.path.getmtime(path) >= started - _MTIME_SLACK
    except OSError:
        return False


def output_target(scene):
    """出力先に「出来ている物」を返す。

    戻り値 = (kind, paths, frame_start) / 何も無ければ None。
      kind = 'MOVIE' なら paths は動画1本、'IMAGE' なら連番のファイル列。
    """
    rd = scene.render

    if rd.is_movie_format:
        # 動画は 1 本。名前にコマ範囲が入る(0001-0003.mp4)。
        path = _frame_path(rd, scene.frame_start)
        if path and os.path.isfile(path) and _written_by_this_render(scene, path):
            return ('MOVIE', [path], int(scene.frame_start))
        return None

    # どのコマが出来ているかは、実物を見て決める。
    frames = range(int(scene.frame_start), int(scene.frame_end) + 1)

    paths = []
    first = None
    for f in frames:
        path = _frame_path(rd, f)
        if path and os.path.isfile(path) and _written_by_this_render(scene, path):
            if first is None:
                first = f
            paths.append(path)
    if not paths:
        return None
    return ('IMAGE', paths, int(first))


# -----------------------------------------------------------------------------
# ストリップを足す / 差し替える
# -----------------------------------------------------------------------------

def _strip_source(strip):
    """ストリップが指しているファイル(先頭)の絶対パス。"""
    try:
        if strip.type == 'MOVIE':
            return _abspath(strip.filepath)
        if strip.type == 'IMAGE':
            elements = strip.elements
            if len(elements) == 0:
                return None
            return os.path.normpath(
                os.path.join(_abspath(strip.directory), elements[0].filename)
            )
    except Exception:
        pass
    return None


def _free_channel(sequence_editor):
    channel = 1
    for strip in sequence_editor.strips_all:
        channel = max(channel, int(strip.channel) + 1)
    return channel


def share_output(scene, target=None):
    """出力先の物を Sequencer へ足す。足した(あるいは差し替えた)ストリップを返す。

    ★`target` は「レンダーが終わった時点」で判定した物を渡せる。GUI では
      Sequencer へ足すのが1拍あと = その時には `render.filepath` を既に
      元へ戻しているので、判定をやり直すと出力名の分を見失う。
    """
    if target is None:
        target = output_target(scene)
    if target is None:
        return None
    kind, paths, frame_start = target

    # ★置き場はレンダーしたシーンではなく専用の `VSE`(2026-09-12)。
    #   ここに置くと `use_sequencer` が次の F12 を Sequencer へ流してしまう。
    into = share_scene(scene) or scene
    sequence_editor = into.sequence_editor
    if sequence_editor is None:
        sequence_editor = into.sequence_editor_create()

    # 同じ経路の物が既にあれば取り除く(二重に増やさない)。段は引き継ぐ。
    channel = None
    for strip in list(sequence_editor.strips):
        if _strip_source(strip) == paths[0]:
            if channel is None:
                channel = int(strip.channel)
            else:
                channel = min(channel, int(strip.channel))
            sequence_editor.strips.remove(strip)
    if channel is None:
        channel = _free_channel(sequence_editor)

    name = os.path.basename(paths[0])
    if kind == 'MOVIE':
        strip = sequence_editor.strips.new_movie(
            name=name, filepath=paths[0], channel=channel, frame_start=frame_start,
        )
    else:
        strip = sequence_editor.strips.new_image(
            name=name, filepath=paths[0], channel=channel, frame_start=frame_start,
        )
        for path in paths[1:]:
            strip.elements.append(os.path.basename(path))
    if into is not scene:
        _fit_frame_range(into)
    return strip


def _fit_frame_range(vse_scene):
    """そのシーンのコマ範囲を、置いてあるストリップの総和へ合わせる(2026-09-12)。

    ★既定の 1-250 のままだと `view_all` が 250 コマ幅で描き、3 コマのストリップは
      画面幅の 1% にしかならない(置き場を分けた副作用)。レンダーしたシーンの
      範囲は触らない(そちらの F12 に影響させない)。

    合わせたら `(start, end)` を返す。ストリップが1本も無ければ **何もせず** None。
    """
    if vse_scene is None:
        return None
    ed = vse_scene.sequence_editor
    if ed is None or not ed.strips:
        return None
    start = min(int(s.frame_final_start) for s in ed.strips)
    end = max(int(s.frame_final_end) for s in ed.strips) - 1
    end = max(start, end)
    vse_scene.frame_start = start
    vse_scene.frame_end = end
    return (start, end)


def sequencer_scene(context):
    """Sequencer が実際に見ているシーン。

    ★**`context.scene` ではない。** Blender 5.x の Sequencer が読むのは
      `CTX_data_sequencer_scene()` = `workspace.sequencer_scene` で、
      `context.scene` を Sequencer 向けに読み替えているのは `tool_settings` だけ
      (`rna_context.cc` の `rna_Context_tool_settings_get`)。
      ここを取り違えると、**Sequencer で押したのにレンダー側のシーンの
      コマ範囲が動く**(2026-09-10 の「レンダー開始が固定される」と同じ型の事故)。

    順に: 画面の `sequencer_scene` → ワークスペースの物 → 置き場の `VSE` →
    最後の手段として `context.scene`(`-b` にはどれも無い)。
    """
    scene = getattr(context, "sequencer_scene", None)
    if scene is not None:
        return scene
    workspace = getattr(context, "workspace", None)
    if workspace is not None:
        scene = getattr(workspace, "sequencer_scene", None)
        if scene is not None:
            return scene
    own = getattr(context, "scene", None)
    into = share_scene(own, create=False)
    if into is not None:
        return into
    return own


# -----------------------------------------------------------------------------
# ファイルブラウザをレンダー出力先へ向ける (2026-09-12)
# -----------------------------------------------------------------------------
#
# 作者 2026-09-12「VSE の時にフォルダ(ファイルブラウザ)が左上にあるはずだから
# 連携できる? 通常は C: だけど**レンダリングの出力で指定したフォルダ**に
# なるようにしたい。チェック項目で ON/OFF・OFF は Blender 既存のまま」。
#
# 向けるのは **VSE が居る画面のファイルブラウザだけ**。作者が別の用で開いている
# ブラウザまで飛ばすと、直せない邪魔になる。

# `space.params` がまだ出来ていない時の再試行の残り回数。
_BROWSER_RETRY_MAX = 20
_browser_retry_left = 0


def browser_follow_enabled(scene):
    """「ブラウザを出力先に合わせる」が入っているか。既定 ON・OFF なら一切触らない。"""
    return bool(getattr(scene, "falcon_vse_browser_follow_output", False))


def output_directory(scene):
    """レンダー出力先の**フォルダ**(末尾に区切り付き)。決められなければ None。

    `render.filepath` は「フォルダ + ファイル名の頭」なので、
    区切りで終わっているか、実在のフォルダを指している時だけそのまま使い、
    それ以外はディレクトリ部分を取る。`//` は `bpy.path.abspath` で解く。
    Windows でも同じ道を通る(`os.path` しか使わない)。
    """
    if scene is None:
        return None
    render = getattr(scene, "render", None)
    if render is None:
        return None
    raw = getattr(render, "filepath", "") or ""
    if not raw:
        return None
    ends_with_sep = raw.endswith(("/", "\\"))
    try:
        path = bpy.path.abspath(raw)
    except Exception:  # noqa: BLE001
        path = raw
    path = os.path.normpath(path)
    if not (ends_with_sep or os.path.isdir(path)):
        path = os.path.dirname(path)
    if not path or path == ".":
        return None
    return os.path.join(path, "")


def browser_areas(screen):
    """その画面で向けてよいファイルブラウザ。**VSE が居る画面だけ**。"""
    areas = list(getattr(screen, "areas", ()) or ())
    if not any(a.type == 'SEQUENCE_EDITOR' for a in areas):
        return []
    return [a for a in areas if a.type == 'FILE_BROWSER']


def _set_browser_directory(area, directory):
    """向ける。まだ `space.params` が無ければ False(呼んだ側が1拍おく)。"""
    space = area.spaces.active
    params = getattr(space, "params", None)
    if params is None:
        # ★area が一度も描かれていないと `params` は None。
        return False
    try:
        params.directory = os.fsencode(directory)
    except Exception as ex:  # noqa: BLE001  UI を止めない
        print("falcon_vse_bridge:", ex)
        return False
    area.tag_redraw()
    return True


def _point_pass(scene, windows=None):
    """1回ぶん。(向けた area, まだ描かれていない物があったか) を返す。"""
    from . import scene_follow
    scene = scene_follow.source_scene(scene)  # Video Editing で窓のシーンを VSE に揃えた時も 3D の出力先
    if not browser_follow_enabled(scene):
        return ([], False)
    directory = output_directory(scene)
    if directory is None:
        return ([], False)
    if windows is None:
        manager = getattr(bpy.context, "window_manager", None)
        windows = list(getattr(manager, "windows", ()) or ())
    done = []
    pending = False
    for window in windows:
        for area in browser_areas(getattr(window, "screen", None)):
            if _set_browser_directory(area, directory):
                done.append(area)
            else:
                pending = True
    return (done, pending)


def _browser_retry():
    global _browser_retry_left
    _browser_retry_left -= 1
    try:
        _, pending = _point_pass(getattr(bpy.context, "scene", None))
    except Exception as ex:  # noqa: BLE001
        print("falcon_vse_bridge:", ex)
        return None
    if not pending or _browser_retry_left <= 0:
        return None
    return 0.1


def point_file_browsers(scene=None):
    """VSE の画面のファイルブラウザを、レンダー出力先のフォルダへ向ける。

    向けた area の一覧を返す。まだ描かれていない物があれば1拍おいて追いかける。
    """
    global _browser_retry_left
    if scene is None:
        scene = getattr(bpy.context, "scene", None)
    done, pending = _point_pass(scene)
    if pending and not bpy.app.background:
        _browser_retry_left = _BROWSER_RETRY_MAX
        if not bpy.app.timers.is_registered(_browser_retry):
            bpy.app.timers.register(_browser_retry, first_interval=0.1)
    return done


# --- ワークスペースを切り替えた時 --------------------------------------------
#
# ★`bpy.app.handlers` に「ワークスペースが変わった」は無い。`window.workspace` は
#   RNA の更新を通るので、メッセージバスで受ける(`rna_access.cc` の
#   `RNA_property_update` が `WM_msg_publish_rna` を呼ぶ)。
#   ★購読はファイルを読み込むと消えるので `load_post` で張り直す。

_msgbus_owner = object()

# 既に一度向けたファイルブラウザ(`area.as_pointer()`)。
#
# ★ワークスペースを「+」から**作って開いた**時(`WORKSPACE_OT_append_activate`)は
#   `window.workspace` の RNA を通らないのでメッセージバスは鳴らない。
#   ⇒ **初めて見るブラウザだけ**1回向ける。2 回目以降は作者が動かした先を尊重する。
_browser_seen = set()


def _on_browser_header_draw(self, context):
    """ファイルブラウザが描かれた時。初めて見る area だけ1回向ける。"""
    if bpy.app.background:
        return
    area = getattr(context, "area", None)
    if area is None or area.type != 'FILE_BROWSER':
        return
    if not browser_areas(getattr(context, "screen", None)):
        return  # VSE が居ない画面のブラウザは触らない
    key = area.as_pointer()
    if key in _browser_seen:
        return
    _browser_seen.add(key)
    if not browser_follow_enabled(getattr(context, "scene", None)):
        return
    # ★描いている最中にデータを書かない。1拍おく(`_browser_retry` が向ける)。
    global _browser_retry_left
    _browser_retry_left = _BROWSER_RETRY_MAX
    if not bpy.app.timers.is_registered(_browser_retry):
        bpy.app.timers.register(_browser_retry, first_interval=0.0)


def _on_workspace_changed(*args):
    try:
        point_file_browsers()
    except Exception as ex:  # noqa: BLE001  UI を止めない
        print("falcon_vse_bridge:", ex)
    try:
        ensure_sequencer_scene()
    except Exception as ex:  # noqa: BLE001  UI を止めない
        print("falcon_vse_bridge:", ex)
    # Sequencer のある画面に入った / 出た → エンジン(1拍おいて)。
    _auto_engine_poke()


def _subscribe_workspace():
    if bpy.app.background:
        return
    try:
        bpy.msgbus.clear_by_owner(_msgbus_owner)
        bpy.msgbus.subscribe_rna(
            key=(bpy.types.Window, "workspace"),
            owner=_msgbus_owner,
            args=(),
            notify=_on_workspace_changed,
        )
        bpy.msgbus.subscribe_rna(
            key=(bpy.types.RenderSettings, "engine"),
            owner=_msgbus_owner,
            args=(),
            notify=_on_engine_changed,
        )
        # 窓のシーンを替えた / エディタの種類を替えた(Layout の一角を Sequencer にした等)。
        for key in ((bpy.types.Window, "scene"),
                    (bpy.types.Area, "ui_type"),
                    (bpy.types.Area, "type")):
            bpy.msgbus.subscribe_rna(
                key=key,
                owner=_msgbus_owner,
                args=(),
                notify=_on_view_changed,
            )
    except Exception as ex:  # noqa: BLE001
        print("falcon_vse_bridge:", ex)
    # ★`register()` の中では `bpy.data` が `_RestrictData`(`scenes` が無い)なので、
    #   ここで直に覚えると register が例外で落ちる(2026-09-19)。1拍おいて覚える。
    #   `load_post` もここを通るので、ファイルを読み込むたびに覚え直す。
    if not bpy.app.timers.is_registered(_remember_engines_deferred):
        bpy.app.timers.register(_remember_engines_deferred, first_interval=0.0)
    # エンジンの自動切り替え: 見回り(ファイルを読み込んでも消えない)と、1拍おいた突き合わせ。
    if not bpy.app.timers.is_registered(_auto_engine_watch):
        bpy.app.timers.register(
            _auto_engine_watch, first_interval=_AUTO_WATCH_INTERVAL, persistent=True)
    _auto_engine_poke()


# --- レンダーエンジンを VSE にした時 (2026-09-17 作者) -------------------------
#
# 作者「レンダーエンジン押したら VSE の項目に移動するようにしてほしい」。
# ⇒ エンジンの一覧で VSE を選んだら、その窓を Video Editing のワークスペースへ移す。
#   ★移すのは「VSE **に変わった**」時だけ。読み込んだファイルが最初から VSE でも、
#     作者が Layout などへ自分で戻った後も、勝手には動かさない
#     (シーンごとの前の値を覚えておき、変わった瞬間だけ拾う)。
#   `FALCON_VSE_ENGINE_JUMP=0` で移らない(エンジンだけ変わる = 前の挙動)。

# scene.as_pointer() → 最後に見たエンジン
_engine_seen = {}


def engine_jump_enabled():
    """VSE を選んだ時に Video Editing へ移るか。呼ばれるたびに環境変数を読む。"""
    return os.environ.get("FALCON_VSE_ENGINE_JUMP", "1").strip().lower() not in (
        "", "0", "off", "false", "no",
    )


def _remember_engines():
    _engine_seen.clear()
    for scene in bpy.data.scenes:
        _engine_seen[scene.as_pointer()] = scene.render.engine


def _remember_engines_deferred():
    try:
        _remember_engines()
    except Exception as ex:  # noqa: BLE001  UI を止めない
        print("falcon_vse_bridge:", ex)
    return None


def _screen_has_sequencer(screen):
    return screen is not None and any(
        area.type == 'SEQUENCE_EDITOR' for area in screen.areas)


def _video_editing_workspace():
    """Sequencer を持つワークスペース。名前が Video Editing の物を先に選ぶ。"""
    found = None
    for workspace in bpy.data.workspaces:
        if not any(_screen_has_sequencer(screen) for screen in workspace.screens):
            continue
        if workspace.name.startswith("Video Editing"):
            return workspace
        found = found or workspace
    return found


def _append_video_editing_workspace(window):
    """ファイルに無ければ、テンプレートから足して開く(「+」メニューと同じ操作)。"""
    path = os.path.join(
        bpy.utils.system_resource('SCRIPTS'), "startup", "bl_app_templates_system",
        "Video_Editing", "startup.blend")
    if not os.path.isfile(path):
        return False
    with bpy.context.temp_override(window=window, screen=window.screen):
        bpy.ops.workspace.append_activate(idname="Video Editing", filepath=path)
    # ★append_activate は `window.workspace` の RNA を通らないので、
    #   `_on_workspace_changed` が鳴らない。sequencer_scene はここで埋める。
    _defer_ensure_sequencer_scene()
    return True


def _jump_to_video_editing():
    try:
        wm = bpy.context.window_manager
        changed = set()
        for scene in bpy.data.scenes:
            key = scene.as_pointer()
            engine = scene.render.engine
            seen = _engine_seen.get(key)
            if seen is not None and seen != engine:
                # ★こちらが替えた分は `_auto_set_engine` が先に書いているので、ここへは来ない。
                #   来るのは作者(や Python)が選んだ時だけ = 戻す先を更新する。
                _auto_note_choice(scene, seen, engine)
            if engine == FALCON_VSE_ENGINE and seen != FALCON_VSE_ENGINE:
                changed.add(key)
            _engine_seen[key] = engine
        if not changed or not engine_jump_enabled():
            return None
        for window in wm.windows:
            if window.scene is None or window.scene.as_pointer() not in changed:
                continue
            if _screen_has_sequencer(window.screen):
                continue  # もう Sequencer が見えている
            target = _video_editing_workspace()
            if target is not None:
                window.workspace = target
            else:
                _append_video_editing_workspace(window)
    except Exception as ex:  # noqa: BLE001  UI を止めない
        print("falcon_vse_bridge:", ex)
    return None


def _on_engine_changed(*args):
    if bpy.app.background:
        return
    # ★通知の最中に窓の中身を替えない。1拍おく。
    if not bpy.app.timers.is_registered(_jump_to_video_editing):
        bpy.app.timers.register(_jump_to_video_editing, first_interval=0.0)


# --- Sequencer のある画面に入ったらエンジンを VSE に (2026-09-19 作者) -----------
#
# 作者「VSE のときに VSE に自動で切り替えにしたい」。
# ⇒ Sequencer のある画面(Video Editing など)に**入ったら**、その窓のシーンのエンジンを
#   VSE にする。Sequencer の無い画面へ**出たら**、入る前のエンジンへ戻す
#   (戻さないと、3D の画面で一覧に無い VSE が選ばれたままになる)。
#   `FALCON_VSE_AUTO_ENGINE=0` で切り替えない(前の挙動)。
#
# ★「入った / 出た」は**シーンごと**に決める。どれか 1 つの窓でもそのシーンを Sequencer の
#   ある画面に出していれば「入っている」(片方の窓が Layout・片方が Video Editing なら VSE)。
#   窓のフォーカスでは行き来させない(エンジンを替えるたびに本体がビューポートの
#   レンダーを止めて作り直すので)。
# ★覚え(入る前のエンジン・入っているか)は**シーンの中**に置く = 保存される・Ctrl+Z で
#   エンジンと一緒に戻る。その覚えと今の画面を突き合わせて、**食い違った時だけ**切り替える:
#     - 画面を移った / 窓を閉じた / 窓のシーンを替えた
#     - 開いたファイルや Ctrl+Z で戻った状態が、今の画面と食い違う
#       (Video Editing で最初の編集を Ctrl+Z すると、入る前の Cycles の状態に戻る → VSE に直す。
#        Layout で Ctrl+Z して、Video Editing に居た時の VSE に戻る → 入る前へ戻す)
#   入っている間に作者が VSE 以外を選んだら、それを尊重する(出入りするまで触らない)。
#   戻す先も、その選んだエンジンへ更新する(`_auto_note_choice`)。
# ★ループの門: こちらがエンジンを替える時は `_engine_seen` に先に書く(`_auto_set_engine`)。
#   「VSE を選んだら Video Editing へ移る」(`_jump_to_video_editing`)は `_engine_seen` と
#   違う時だけ動くので、こちらの書き換えでは移らない(Layout の窓が引っ張られない)。
# ★-b(背景)では何もしない。窓が無いので、Video Editing で保存したファイルを -b で
#   書き出すと、全部のシーンが「出た」ことになってしまう。
# 取っ掛かり: ワークスペース・窓のシーン・エディタの種類の変更(msgbus)/ Sequencer と
#   上の帯のヘッダの描画(Ctrl+PageUp/Down と「+」から足した時は msgbus が鳴らない)/
#   読み込み / Ctrl+Z / 0.5 秒おきの見回り(窓を閉じた時など、どこからも通知が来ない所)。

# シーンに置く覚え(RNA の項目としても登録する = 開発者向けの表示の時しか出ない)。
AUTO_ENGINE_BEFORE = "falcon_vse_engine_before"
AUTO_ENGINE_ACTIVE = "falcon_vse_engine_auto"

# 見回りの間隔(秒)。窓の形が変わっていなければ何もしない。
_AUTO_WATCH_INTERVAL = 0.5

# 前回突き合わせた時の窓の形(`_auto_window_signature`)。
_auto_last_sig = None
# 次の見回りで必ず突き合わせる(読み込み・Ctrl+Z・レンダーの後)。
_auto_dirty = True
# 次の突き合わせのきっかけ。'load' の時は、切り替えたシーンを端末に 1 行出す。
_auto_reason = None


def auto_engine_enabled():
    """Sequencer のある画面に出入りした時にエンジンを切り替えるか。呼ばれるたびに環境変数を読む。"""
    return os.environ.get("FALCON_VSE_AUTO_ENGINE", "1").strip().lower() not in (
        "", "0", "off", "false", "no",
    )


def _window_in_vse(window):
    """その窓が Sequencer のある画面を出しているか。数えない窓は None。

    - 仮の窓(レンダー結果・プリファレンスなど `is_temporary`)は数えない
    - 1 つのエリアを最大化している間(`show_fullscreen`)は、そのワークスペースの
      元の画面で決める(Video Editing でプロパティを最大化しても「出た」にしない)。
      ★元の画面にも `show_fullscreen` が立つ(`screen_state_to_nonnormal` が
        `oldscreen->state` も書き換える)ので、それでは除かない。除くのは今の画面だけ
    """
    screen = getattr(window, "screen", None)
    if screen is None or getattr(screen, "is_temporary", False):
        return None
    from . import scene_follow
    if not scene_follow.shows_own_scene(window):
        return False  # 窓のシーンは Sequencer のシーンではない(3D のシーンを VSE エンジンにしない)
    if _screen_has_sequencer(screen):
        return True
    if getattr(screen, "show_fullscreen", False):
        workspace = getattr(window, "workspace", None)
        for other in (getattr(workspace, "screens", None) or ()):
            if other != screen and _screen_has_sequencer(other):
                return True
    return False


def _auto_scene_modes(windows):
    """{scene.as_pointer(): (scene, 入っているか)}。数える窓が 1 つも無ければ None。"""
    modes = {}
    counted = False
    for window in windows:
        inside = _window_in_vse(window)
        if inside is None:
            continue
        counted = True
        scene = getattr(window, "scene", None)
        if scene is None:
            continue
        key = scene.as_pointer()
        known = modes.get(key)
        modes[key] = (scene, bool(inside) or (known is not None and known[1]))
    return modes if counted else None


def _auto_active(scene):
    """覚え: このシーンは今 Sequencer のある画面に入っているか。"""
    try:
        return bool(scene.get(AUTO_ENGINE_ACTIVE, False))
    except Exception:  # noqa: BLE001
        return False


def _auto_store(scene, key, value):
    """覚えをシーンへ書く。★RNA を通さない(依存グラフの更新と通知を出さない)。"""
    try:
        if scene.get(key) != value:
            scene[key] = value
    except Exception as ex:  # noqa: BLE001  書けないシーン(リンク)など
        print("falcon_vse_bridge:", ex)


def _auto_set_engine(scene, idname):
    """こちらからエンジンを替える。★先に `_engine_seen` へ書く(ループの門)。"""
    key = scene.as_pointer()
    seen = _engine_seen.get(key)
    _engine_seen[key] = idname
    try:
        scene.render.engine = idname
    except Exception:
        if seen is None:
            _engine_seen.pop(key, None)
        else:
            _engine_seen[key] = seen
        raise


def _auto_note_choice(scene, old, new):
    """作者(や Python)がエンジンを替えた時。戻す先を更新する。

    - VSE を選んだ: その前のエンジンを戻す先にする(Layout で選んで移った時のため)
    - 入っている間に VSE 以外を選んだ: それを戻す先にする(次に出入りした時にそこへ戻る)
    入っていない時の VSE 以外の選択は書かない(入る時に、その時のエンジンを覚える)。
    """
    if not auto_engine_enabled():
        return
    if new == FALCON_VSE_ENGINE:
        if old and old != FALCON_VSE_ENGINE:
            _auto_store(scene, AUTO_ENGINE_BEFORE, old)
    elif _auto_active(scene):
        _auto_store(scene, AUTO_ENGINE_BEFORE, new)


def _auto_enter(scene):
    """入った。VSE にして、入る前のエンジンを覚える。"""
    engine = scene.render.engine
    _auto_store(scene, AUTO_ENGINE_ACTIVE, True)
    if engine != FALCON_VSE_ENGINE:
        _auto_store(scene, AUTO_ENGINE_BEFORE, engine)
        _auto_set_engine(scene, FALCON_VSE_ENGINE)


def _auto_leave(scene):
    """出た。VSE のままなら入る前のエンジンへ戻す(作者が他を選んでいたらそのまま)。"""
    _auto_store(scene, AUTO_ENGINE_ACTIVE, False)
    if scene.render.engine != FALCON_VSE_ENGINE:
        return
    before = scene.get(AUTO_ENGINE_BEFORE) or ""
    if before and before != FALCON_VSE_ENGINE:
        try:
            _auto_set_engine(scene, before)
            return
        except Exception:  # noqa: BLE001  もう無いエンジン(アドオンを切った等)
            pass
    # ★覚えが無い(この機能より前に VSE で保存したファイル等)。VSE のまま 3D の画面に
    #   残すと一覧に無い値が選ばれたままになるので、一覧の先頭へ戻して 1 行残す。
    for idname, _name in render_engines():
        if idname == FALCON_VSE_ENGINE:
            continue
        try:
            _auto_set_engine(scene, idname)
        except Exception:  # noqa: BLE001
            continue
        print("falcon_vse_bridge: %s: the engine to return to (%r) is unknown, set it to %s"
              % (scene.name, before, idname))
        return


def _auto_engine_apply(windows, scenes):
    """画面と覚えを突き合わせて、食い違ったシーンだけ切り替える。

    戻り値 = [(シーン名, 'enter' | 'leave', 前のエンジン, 後のエンジン)]。
    ★窓とシーンを引数で受ける(門が作り物の窓で確かめられるように)。背景かどうかは
      ここでは見ない(呼ぶ側 `_auto_engine_run` が見る)。
    """
    modes = _auto_scene_modes(windows)
    if modes is None:
        return []  # 数える窓が無い = 判断しない(全部を「出た」にしない)
    done = []
    for scene in scenes:
        if getattr(scene, "library", None) is not None:
            continue  # リンクしたシーンは書けない
        entry = modes.get(scene.as_pointer())
        inside = bool(entry is not None and entry[1])
        if inside == _auto_active(scene):
            continue
        engine = scene.render.engine
        try:
            if inside:
                _auto_enter(scene)
            else:
                _auto_leave(scene)
        except Exception as ex:  # noqa: BLE001  UI を止めない
            print("falcon_vse_bridge:", ex)
            continue
        done.append((scene.name, "enter" if inside else "leave",
                     engine, scene.render.engine))
    return done


def _auto_windows():
    manager = getattr(bpy.context, "window_manager", None)
    return list(getattr(manager, "windows", ()) or ())


def _auto_window_signature(windows):
    """窓の形(窓・画面・シーン・Sequencer の有無)。変わった時だけ突き合わせる。"""
    signature = []
    for window in windows:
        screen = getattr(window, "screen", None)
        scene = getattr(window, "scene", None)
        signature.append((
            window.as_pointer(),
            screen.as_pointer() if screen is not None else 0,
            scene.as_pointer() if scene is not None else 0,
            _window_in_vse(window),
        ))
    return tuple(signature)


def _render_job_running():
    try:
        return bool(bpy.app.is_job_running('RENDER'))
    except Exception:  # noqa: BLE001
        return False


def _auto_engine_run():
    """今の画面と覚えを突き合わせる(GUI だけ)。切り替えた物の一覧を返す。"""
    global _auto_last_sig, _auto_dirty, _auto_reason
    if bpy.app.background or not auto_engine_enabled():
        _auto_reason = None  # 切っている間のきっかけは持ち越さない
        return []
    if _render_job_running():
        # ★レンダーの最中にエンジンを替えない。終わってから見回りが拾う。
        _auto_dirty = True
        return []
    windows = _auto_windows()
    signature = _auto_window_signature(windows)
    done = _auto_engine_apply(windows, bpy.data.scenes)
    reason, _auto_reason = _auto_reason, None
    _auto_last_sig = signature
    _auto_dirty = False
    if reason == "undo":
        # ★Ctrl+Z はエンジンを通知なしで戻す。覚え直さないと、次の通知で
        #   Ctrl+Z の分まで「作者が選んだ」と読んでしまう。
        _remember_engines()
    if reason == "load":
        # ★黙って倒さない。開いた時に替えた物は端末に 1 行残す。
        for name, kind, old, new in done:
            print("falcon_vse_bridge: set the engine of %s to %s (%s). "
                  "FALCON_VSE_AUTO_ENGINE=0 turns this off"
                  % (name, new, "opened in a screen with a Sequencer" if kind == "enter"
                     else "not shown in a screen with a Sequencer"))
    return done


def _auto_engine_deferred():
    try:
        _auto_engine_run()
    except Exception as ex:  # noqa: BLE001  UI を止めない
        print("falcon_vse_bridge:", ex)
    return None


def _auto_engine_poke(reason=None):
    """次の拍で突き合わせる(通知・描画・読み込み・Ctrl+Z から呼ぶ)。"""
    global _auto_dirty, _auto_reason
    if bpy.app.background:
        return
    _auto_dirty = True
    if reason:
        _auto_reason = reason
    if not bpy.app.timers.is_registered(_auto_engine_deferred):
        bpy.app.timers.register(_auto_engine_deferred, first_interval=0.0)


def _auto_engine_watch():
    """見回り。窓の形が変わった(窓を閉じた・エディタの種類を替えた等)時だけ突き合わせる。"""
    try:
        if not bpy.app.background and auto_engine_enabled():
            if _auto_dirty or _auto_window_signature(_auto_windows()) != _auto_last_sig:
                _auto_engine_run()
    except Exception as ex:  # noqa: BLE001  見回りは止めない
        print("falcon_vse_bridge:", ex)
    return _AUTO_WATCH_INTERVAL


def _auto_engine_notice(context):
    """描画から呼ぶ。窓の形が変わっていたら 1 拍おいて突き合わせる(★描画中は書かない)。"""
    if bpy.app.background or not auto_engine_enabled():
        return
    try:
        changed = _auto_window_signature(_auto_windows()) != _auto_last_sig
    except Exception:  # noqa: BLE001
        return
    if changed:
        _auto_engine_poke()


def _draw_topbar_notice(self, context):
    """上の帯(ワークスペースのタブ)が描かれた時。★何も描かない。

    Ctrl+PageUp/Down でワークスペースを替えると msgbus は鳴らない
    (`SCREEN_OT_workspace_cycle` は RNA を通さない)が、上の帯は描き直される。
    """
    _auto_engine_notice(context)


def _on_view_changed(*args):
    """窓のシーン・エディタの種類が替わった(msgbus)。"""
    _auto_engine_poke()


# -----------------------------------------------------------------------------
# Video Editing を開いた時に `sequencer_scene` を埋める (2026-09-12)
# -----------------------------------------------------------------------------
#
# ★`point_empty_workspaces()` は **レンダーが終わった時にしか呼ばれない**ので、
#   先に Video Editing を開くと `workspace.sequencer_scene` が空のままになり、
#   Sequencer のヘッダに「Add new scene to be used by the sequencer: New」が出る。
#   そこで「New」を押されると `SCENE_OT_new_sequencer_scene` が
#   **別のシーンを作って `WM_window_set_active_scene` まで呼ぶ**(`scene_edit.cc`)ので、
#   本番のシーンから離れてしまう。開いた時点で置き場の `VSE` を指しておく。
#
# 取っ掛かりは 2 つ。どちらも「Video Editing を開いた」で必ず通る:
#   - ワークスペースのタブを押す → msgbus (`_on_workspace_changed`)
#   - Sequencer が描かれた       → `_draw_sequencer_header`
#     (「+」から作って開く `WORKSPACE_OT_append_activate` は RNA を通らないので
#      msgbus が鳴らない。そちらはこの draw で拾う)

# 一度でも失敗したら、描くたびにタイマーを積まない(空回りの番)。
_ensure_seq_scene_failed = False


def _share_scene_for(scene):
    """そのシーン向けの置き場(`VSE`)。作れなければ None。

    `share_scene()` は作れなかった時と `FALCON_VSE_SHARE_SCENE=0` の時に
    **`scene` をそのまま返す**ので、ここで「本当に `VSE` か」まで見る。
    """
    if scene is None or not share_scene_enabled():
        return None
    into = share_scene(scene, create=True)
    if into is None or into is not bpy.data.scenes.get(SHARE_SCENE_NAME):
        return None
    return into


def ensure_sequencer_scene(windows=None):
    """Sequencer が居る画面で、まだ空の `sequencer_scene` を置き場へ向ける。

    向けたワークスペースの一覧を返す。

    - **Sequencer が居る画面だけ**が対象。居ない所で `VSE` シーンを作ると、
      VSE を一度も開いていない .blend にシーンが増える。
    - 既に何かを指している所は**作者の選択**なので触らない。
    - ★`FALCON_VSE_SHARE_SCENE=0` のときは**何もしない**(前の挙動 = 空のまま)。
      あの口は「置き場を分けない」という指示なので、ここで別のシーンを作ると
      口の意味が変わる。
    """
    global _ensure_seq_scene_failed
    if not share_scene_enabled():
        return []
    if windows is None:
        manager = getattr(bpy.context, "window_manager", None)
        windows = list(getattr(manager, "windows", ()) or ())
    filled = []
    for window in windows:
        workspace = getattr(window, "workspace", None)
        if workspace is None or not hasattr(workspace, "sequencer_scene"):
            continue
        if workspace.sequencer_scene is not None:
            continue
        screen = getattr(window, "screen", None)
        if not any(a.type == 'SEQUENCE_EDITOR'
                   for a in (getattr(screen, "areas", ()) or ())):
            continue
        into = _share_scene_for(getattr(window, "scene", None))
        if into is None:
            _ensure_seq_scene_failed = True
            continue
        set_sequencer_scene(workspace, into)
        if workspace.sequencer_scene is into:
            filled.append(workspace)
        else:
            _ensure_seq_scene_failed = True
    return filled


def _ensure_seq_scene_deferred():
    try:
        ensure_sequencer_scene()
    except Exception as ex:  # noqa: BLE001  UI を止めない
        global _ensure_seq_scene_failed
        _ensure_seq_scene_failed = True
        print("falcon_vse_bridge:", ex)
    return None


def _defer_ensure_sequencer_scene():
    """描いている最中にデータを書かない。1拍おいて埋める。"""
    if bpy.app.background or _ensure_seq_scene_failed:
        return
    if not bpy.app.timers.is_registered(_ensure_seq_scene_deferred):
        bpy.app.timers.register(_ensure_seq_scene_deferred, first_interval=0.0)


class FALCON_VSE_OT_fit_frame_range(Operator):
    """Fit the frame range to the strips in the sequencer"""
    bl_idname = "falcon_vse.fit_frame_range"
    bl_label = "Fit to Strips"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        scene = sequencer_scene(context)
        if scene is None:
            self.report({'WARNING'}, "No sequencer scene")
            return {'CANCELLED'}
        fitted = _fit_frame_range(scene)
        if fitted is None:
            # ★空なら何もしない(範囲を 1-1 に潰さない)。
            self.report({'WARNING'}, "No strips")
            return {'CANCELLED'}
        self.report({'INFO'}, "%d - %d" % fitted)
        return {'FINISHED'}


# -----------------------------------------------------------------------------
# レンダーが終わった時
# -----------------------------------------------------------------------------

# レンダーが終わって、まだ Sequencer へ足していないシーン。
_pending = []


def flush_pending():
    """溜まっている分を Sequencer へ足す(タイマーから1拍おいて呼ばれる)。

    ★`FALCON_VSE_AUTO_SHARE=0` はここでも効かせる(2026-09-12)。入口だけの検査だと、
      倒す前に溜まった分が後から足されて「止めたのに増える」になる。
    """
    if not auto_share_enabled():
        _pending.clear()
        return None
    while _pending:
        scene_name, target = _pending.pop(0)
        scene = bpy.data.scenes.get(scene_name)
        if scene is None:
            continue
        try:
            share_output(scene, target)
            # ★出来た物の在り処 = レンダー出力先。ブラウザもそこへ向ける。
            point_file_browsers(scene)
            # ★足しただけでは Video Editing を開いても何も出ない。
            #   Blender 5.x の Sequencer は `workspace.sequencer_scene` を見るので、
            #   まだ何も指していないワークスペースにこのシーンを指しておく。
            #   (既に別のシーンを指している所は作者の選択なので触らない)
            point_empty_workspaces(scene)
        except Exception as ex:  # noqa: BLE001  UI を止めない
            print("falcon_vse_bridge:", ex)
    return None


def point_empty_workspaces(scene):
    """`sequencer_scene` がまだ空のワークスペースを、このシーンへ向ける。

    ★レンダーが終わって自動で足した時に要る。ストリップは `scene.sequence_editor`
      に入るが、Sequencer が読むのは `workspace.sequencer_scene` なので、
      ここが空のままだと **Video Editing を開いても空っぽに見える**。
    """
    if scene is None:
        return
    # ★指す先はストリップが実際に入ったシーン(既定では専用の `VSE`)。
    into = share_scene(scene, create=False)
    if into is None:
        return
    for workspace in bpy.data.workspaces:
        if not hasattr(workspace, "sequencer_scene"):
            continue
        if workspace.sequencer_scene is None:
            set_sequencer_scene(workspace, into)


def clear_stuck_preview_range(scenes):
    """立ってしまった `use_preview_range` を落とす。落としたシーン名を返す。

    ★2026-09-10 作者「落として構わない、たぶんそっちが出したのが生きてるだけ」。

    この木で `use_preview_range` を立てていたのは `FALCON_OT_vse_edit` の 1 箇所だけで、
    そこは 92b41f93065 で立てないようにした。**が、既に立った .blend の旗は
    ファイルの中に残る**ので、開き直しても「開始フレームが効かない」ままになる。
    ⇒ 読み込んだ時に落とす。

    旗が立っていると、タイムラインの「開始/終了」は `psfra/pefra` を編集するだけになり、
    F12 のアニメーションレンダーが見る `sfra/efra` は動かない。
    `frame_preview_start/end` の値そのものは残すので、作者が入れ直せば元に戻る。
    """
    cleared = []
    for scene in scenes:
        if getattr(scene, "use_preview_range", False):
            scene.use_preview_range = False
            cleared.append(scene.name)
    return cleared


def clear_stuck_sequencer_on_load():
    """9-12 より前の .blend に残った `use_sequencer` を落とすか。既定 ON。

    `FALCON_VSE_CLEAR_STUCK_SEQUENCER=0` で止まる。
    """
    return os.environ.get(
        "FALCON_VSE_CLEAR_STUCK_SEQUENCER", "1"
    ).strip().lower() not in ("", "0", "off", "false", "no")


def _scene_strips_are_all_ours(scene):
    """そのシーンの Sequencer が、**このアドオンが置いた物だけ**で出来ているか。

    判定は「ストリップが指す先が、そのシーンのレンダー出力先の下にあるか」。
    1 本でも外の素材が混ざっていたら False(作者が自分で組んだ編集なので触らない)。
    ストリップが 1 本も無ければ False。
    """
    ed = getattr(scene, "sequence_editor", None)
    if ed is None:
        return False
    strips = list(getattr(ed, "strips", None) or [])
    if not strips:
        return False
    out_dir = output_directory(scene)
    if not out_dir:
        return False
    out_dir = os.path.normpath(out_dir)
    for strip in strips:
        src = _strip_source(strip)
        if not src:
            return False
        if os.path.normpath(os.path.dirname(src)) != out_dir:
            return False
    return True


def clear_stuck_sequencer(scenes):
    """★(internal tracker) の残り(2026-09-16)。

    9-12 に「ストリップは専用シーン `VSE` へ置く」で**新しく作る分**は塞いだが、
    **それより前に保存した .blend には、本シーンの Sequencer にストリップが残っている**。
    `render.use_sequencer` は Blender の既定で ON なので、そういうファイルを開くと
    **F12 が今でも Cycles を走らせず、さっき焼いた PNG を焼き直す**
    (実測 2026-09-16: 開き直した 1 回目から `cycles.*` の metadata が付かない)。

    ⇒ 読み込んだ時に、**このアドオンが置いた物だけで出来ているシーン**の
       `use_sequencer` を落とす。ストリップ自体は消さない(作者の物かもしれない
       素材を捨てない・`workspace.sequencer_scene` が `VSE` を指すので見え方も変わらない)。
       落としたシーン名を返す。
    """
    cleared = []
    if not share_scene_enabled():
        # 置き場を分けていない設定なら、この掃除は筋が通らない。
        return cleared
    share_name = SHARE_SCENE_NAME
    for scene in scenes:
        if scene.name == share_name:
            continue
        if not getattr(scene.render, "use_sequencer", False):
            continue
        if not _scene_strips_are_all_ours(scene):
            continue
        scene.render.use_sequencer = False
        cleared.append(scene.name)
    return cleared


def clear_preview_range_on_load():
    """読み込み時に落とすか。`FALCON_VSE_CLEAR_PREVIEW_RANGE=0` で止まる。"""
    return os.environ.get("FALCON_VSE_CLEAR_PREVIEW_RANGE", "1").strip().lower() not in (
        "", "0", "off", "false", "no",
    )


@persistent
def _on_load_post(*args):
    # ★メッセージバスの購読はファイルを読み込むと消える。張り直す。
    _subscribe_workspace()
    # 開いたファイルの覚え(Video Editing に入っていたか)と今の画面を突き合わせる。
    _auto_engine_poke("load")
    _browser_seen.clear()
    global _ensure_seq_scene_failed
    _ensure_seq_scene_failed = False
    if clear_stuck_sequencer_on_load():
        try:
            stuck = clear_stuck_sequencer(bpy.data.scenes)
        except Exception as ex:  # noqa: BLE001  読み込みを止めない
            print("falcon_vse_bridge:", ex)
            stuck = []
        if stuck:
            # ★黙って倒さない。何をしたかは端末に1行残す。
            print("falcon_vse_bridge: turned off use_sequencer left in an old .blend (%s). "
                  "FALCON_VSE_CLEAR_STUCK_SEQUENCER=0 turns this off" % ", ".join(stuck))
    if not clear_preview_range_on_load():
        return
    try:
        cleared = clear_stuck_preview_range(bpy.data.scenes)
    except Exception as ex:  # noqa: BLE001  読み込みを止めない
        print("falcon_vse_bridge:", ex)
        return
    if cleared:
        # ★黙って倒さない。何をしたかは端末に1行残す。
        print("falcon_vse_bridge: turned off the preview range (%s). "
              "FALCON_VSE_CLEAR_PREVIEW_RANGE=0 turns this off" % ", ".join(cleared))


@persistent
def _on_render_init(scene, *args):
    if scene is None:
        return
    _written[scene.name] = 0
    _render_started[scene.name] = time.time()
    # ★ここは `RE_RenderAnim` が `scene->r` を複製する前に呼ばれる
    #   (「so user can alter the render settings prior to copying」)。
    apply_output_name(scene)


@persistent
def _on_render_write(scene, *args):
    if scene is None:
        return
    _written[scene.name] = _written.get(scene.name, 0) + 1


@persistent
def _on_render_complete(scene, *args):
    _auto_after_render()
    if scene is None:
        return
    try:
        _render_complete(scene)
    finally:
        # ★何があっても `render.filepath` は元へ戻す。
        restore_output_name(scene)


def _auto_after_render():
    """レンダーの間は画面が替わってもエンジンを替えない。終わったら見回りに突き合わせさせる。

    ★ここはレンダーのジョブの中から呼ばれるので、旗を立てるだけ(タイマーは足さない)。
    """
    global _auto_dirty
    if not bpy.app.background:
        _auto_dirty = True


@persistent
def _on_undo_redo(*args):
    # ★Ctrl+Z はエンジンと覚えを「その時」の値へ戻す(画面は戻らない)。突き合わせ直す。
    _auto_engine_poke("undo")


def _render_complete(scene):
    written = _written.pop(scene.name, 0)
    if not written:
        # ディスクに何も書かれていない(F12 の静止画など)。
        return
    # ★自動で足すかは `FALCON_VSE_AUTO_SHARE` だけで決める(2026-09-12)。
    #   `bridge_enabled() or ...` にしていたので、`FALCON_VSE_BRIDGE` の既定 1 が
    #   or で生き残り、**自動共有だけを止める口が実質存在しなかった**。
    #   手で押す口(`FALCON_VSE_BRIDGE`)はボタン側の判定に残っている。
    if not auto_share_enabled():
        return
    if not getattr(scene, "falcon_vse_share_output", False):
        return
    if not scene.render.save_output:
        return
    # ★置き場の `VSE` 自身の書き出しは戻さない(2026-09-13 起動テスト)。戻すと最上段に
    #   載って、次の書き出しが前の mp4 を焼き直すだけになる。
    if share_scene_enabled() and scene.name == SHARE_SCENE_NAME:
        return
    if _is_sequencer_export(scene):
        return
    # ★判定は「戻す前」に済ませる(出力名の分は今の filepath にしか無い)。
    _pending.append((scene.name, output_target(scene)))
    if bpy.app.background:
        # -b では タイマーが回らない。止める UI も無いのでその場で足す。
        flush_pending()
    elif not bpy.app.timers.is_registered(flush_pending):
        # レンダー中の状態を触らないよう1拍おく。
        bpy.app.timers.register(flush_pending, first_interval=0.0)


def share_skip_sequencer_enabled():
    """VSE の書き出し(エンジン VSE・Sequencer を焼く書き出し)を VSE に戻さないか。既定 ON。

    `FALCON_VSE_SHARE_SKIP_SEQUENCER=0` で前の挙動(置き場の `VSE` 以外は全部戻す)。
    """
    return os.environ.get(
        "FALCON_VSE_SHARE_SKIP_SEQUENCER", "1"
    ).strip().lower() not in ("", "0", "off", "false", "no")


def _is_sequencer_export(scene):
    """このレンダーが「VSE の書き出し」か(2026-09-20 作者)。

    作者「レンダリング後なぜかシーケンサーにレンダリングした png が配置される」。
    9-13 の門は置き場の `VSE` の名前しか見ていなかったので、Video Editing で Ctrl+F12 を
    押した時(= 窓のシーン。エンジンは自動で VSE)の書き出しが素通りして段 5 に載った。
    戻すのは **3D のレンダーの結果だけ**。次のどちらかなら VSE の書き出し = 戻さない:
      - エンジンが VSE(Render タブを VSE の書き出しとして使っている)
      - そのシーン自身の Sequencer を焼く書き出し(`use_sequencer` とストリップ)
    """
    if not share_skip_sequencer_enabled():
        return False
    try:
        if scene.render.engine == FALCON_VSE_ENGINE:
            return True
        ed = scene.sequence_editor
        return bool(scene.render.use_sequencer and ed is not None and len(ed.strips_all))
    except Exception:  # noqa: BLE001  判定できなければ前の挙動
        return False


@persistent
def _on_render_cancel(scene, *args):
    _auto_after_render()
    if scene is None:
        return
    _written.pop(scene.name, None)
    _render_started.pop(scene.name, None)
    restore_output_name(scene)


# -----------------------------------------------------------------------------
# 「VSE で編集」
# -----------------------------------------------------------------------------

def _sequencer_area(window):
    screen = window.screen
    for area in screen.areas:
        if area.type == 'SEQUENCE_EDITOR':
            return area
    # 無ければ、いちばん広い所を Sequencer にする。
    candidates = [a for a in screen.areas if a.type != 'PROPERTIES']
    if not candidates:
        candidates = list(screen.areas)
    if not candidates:
        return None
    area = max(candidates, key=lambda a: a.width * a.height)
    area.type = 'SEQUENCE_EDITOR'
    space = area.spaces.active
    if hasattr(space, "view_type"):
        space.view_type = 'SEQUENCER'
    return area


def set_sequencer_scene(workspace, scene):
    """VSE が読むシーンをワークスペースに指す。

    ★Blender 5.x では Sequencer は `workspace.sequencer_scene` を見る。
      ここが空だと、Sequencer を開いてもストリップは出ず操作もできない
      (`ED_operator_sequencer_active` が通らない)。
    """
    if workspace is None or scene is None:
        return
    if getattr(workspace, "sequencer_scene", None) is not scene:
        try:
            workspace.sequencer_scene = scene
        except Exception:
            pass


def _go_to_vse(context):
    window = getattr(context, "window", None)
    if window is None:
        return
    # ★VSE が読むのはストリップが入ったシーン(既定では専用の `VSE`)。
    scene = share_scene(context.scene) or context.scene
    workspace = bpy.data.workspaces.get("Video Editing")
    if workspace is not None:
        window.workspace = workspace
    else:
        _sequencer_area(window)
        workspace = window.workspace
    set_sequencer_scene(workspace, scene)
    # ★飛んだ先のファイルブラウザを出力先へ向ける(チェックが入っている時だけ)。
    point_file_browsers(getattr(context, "scene", None))


class FALCON_OT_vse_edit(Operator):
    """Add what was rendered to the output path to the Sequencer and go there"""
    bl_idname = "falcon.vse_edit"
    bl_label = "Edit in VSE"
    bl_options = {'REGISTER', 'UNDO'}

    # Sequencer の「追加」から呼ぶ時は、既にそこに居るので移らない。
    switch_workspace: BoolProperty(
        name="Go to VSE",
        description="Switch to the Video Editing workspace",
        default=True,
        options={'HIDDEN', 'SKIP_SAVE'},
    )

    def execute(self, context):
        from . import scene_follow
        scene = scene_follow.source_scene(context.scene)  # VSE に揃えた窓でも 3D のレンダーの出力
        with output_name_applied(scene):
            target = output_target(scene)
        if target is None:
            self.report({'WARNING'}, "No files at the output path")
            return {'CANCELLED'}

        strip = share_output(scene, target)
        if strip is None:
            return {'CANCELLED'}

        # ★足した先は専用の `VSE`(`FALCON_VSE_SHARE_SCENE=0` なら今のシーン)。
        sequence_editor = (share_scene(scene) or scene).sequence_editor
        for other in sequence_editor.strips_all:
            other.select = False
        strip.select = True
        sequence_editor.active_strip = strip

        # ★`use_preview_range` は立てない(2026-09-10)。
        #
        #   作者「レンダリング開始が 0 に固定されている影響で、他のフレームから
        #   レンダリングが開始できない」の正体がここだった。この旗が立つと
        #   タイムラインの「開始/終了」は `psfra/pefra` を編集するだけになり、
        #   F12 のアニメーションレンダーが見る `sfra/efra` は動かない
        #   (`render_internal.cc`)。押した作者には**開始フレームを変えたのに
        #   効かない**としか見えず、しかもこの旗は .blend に残るので、
        #   一度押したファイルはずっとその状態のままになる。
        #
        #   ⇒ 範囲には触らない。既に立っている所の見え方も変えない。
        #   足したストリップは下で選択済みなので、寄せたければ作者が押せばよい。
        if getattr(scene, "use_preview_range", False):
            # 既に作者が使っている時だけ、足した物へ範囲を合わせる。
            scene.frame_preview_start = int(strip.frame_final_start)
            scene.frame_preview_end = max(
                int(strip.frame_final_start), int(strip.frame_final_end) - 1
            )

        if self.switch_workspace:
            _go_to_vse(context)
        else:
            # 移らない時も、今の所が読むシーンだけは合わせる。
            set_sequencer_scene(getattr(context, "workspace", None),
                                share_scene(scene) or scene)
        return {'FINISHED'}


# -----------------------------------------------------------------------------
# UI
# -----------------------------------------------------------------------------

def in_sequencer_context(context):
    """今の画面に既に Sequencer が居るか。

    ★居るなら「VSE で編集」の**ボタンは出さない** — 作者 (2026-09-04)
      「VSE に居るのに VSE に共有する項目がなぜかある」。
      移り先が今居る所と同じなので、押しても何も起きないように見える。
      足す口そのものは Sequencer の「追加 > レンダー出力」に在る。
    """
    screen = getattr(context, "screen", None)
    if screen is not None:
        for area in screen.areas:
            if area.type == 'SEQUENCE_EDITOR':
                return True
    workspace = getattr(context, "workspace", None)
    if workspace is not None and workspace.name == "Video Editing":
        return True
    return False


def last_export_info(scene):
    """直近の書き出しで何が効いたか(RNA が C++ 側から読む1行)。

    素の Blender には無いので、その時は空。
    """
    render = getattr(scene, "render", None)
    if render is None:
        return ""
    return (getattr(render, "falcon_last_export_info", "") or "").strip()


def output_rows(context):
    """`_draw_output` が出す物の一覧。

    ★描画そのものは門から見えないので、**出す物の決め方をここ1箇所に寄せる**。
      `("prop", 名前)` / `("label", 文)` / `("separator", None)` /
      `("operator", bl_idname)`。
    """
    scene = context.scene
    rows = [("prop", "falcon_output_name")]

    path = preview_path(scene)
    if path:
        rows.append(("label", path))

    if bridge_enabled():
        rows.append(("separator", None))
        rows.append(("prop", "falcon_vse_share_output"))
        rows.append(("prop", "falcon_vse_browser_follow_output"))

    info = last_export_info(scene)
    if info:
        rows.append(("label", info))

    if bridge_enabled() and not in_sequencer_context(context):
        rows.append(("operator", "falcon.vse_edit"))
    return rows


def _draw_output(self, context):
    layout = self.layout
    layout.separator()
    layout.use_property_split = False
    column = layout.column()
    scene = context.scene
    for kind, value in output_rows(context):
        if kind == "prop":
            column.prop(scene, value)
        elif kind == "label":
            row = column.row()
            row.active = False
            row.label(text=value)
        elif kind == "separator":
            column.separator()
        elif kind == "operator":
            column.operator(value, icon='SEQUENCE')


def _draw_sequencer_add(self, context):
    """Sequencer の「追加」に、レンダー出力を足す口を出す。

    ★これが無いと、Video Editing のワークスペースを開いた側からは
      このアドオンの存在がまったく見えない(出力プロパティと画像エディタの
      ヘッダにしか出ていなかった)。
    """
    if not bridge_enabled():
        return
    layout = self.layout
    layout.separator()
    props = layout.operator(
        "falcon.vse_edit", text="Render Output", icon='RENDER_RESULT',
    )
    props.switch_workspace = False


def _draw_image_header(self, context):
    if not bridge_enabled():
        return
    space = context.space_data
    image = getattr(space, "image", None)
    if image is None or image.type != 'RENDER_RESULT':
        return
    self.layout.operator("falcon.vse_edit", icon='SEQUENCE')


def _draw_sequencer_header(self, context):
    """Sequencer のヘッダに「ストリップに合わせる」を出す。

    ★サイドバー (N) ではなくヘッダに置く。Video Editing のワークスペースは
      **Sequencer のサイドバーが既定で畳まれている**
      (`versioning_defaults.cc` が `RGN_FLAG_HIDDEN` を立てる)ので、
      そこに置くと「ワンクリック」にならない。

    ★ついでに、ここが「Video Editing を開いた」の取っ掛かりになる。
      `sequencer_scene` が空のままだと同じヘッダに「New」が出るので、
      1拍おいて置き場を指す(2026-09-12)。
    """
    workspace = getattr(context, "workspace", None)
    if workspace is not None and getattr(workspace, "sequencer_scene", 1) is None:
        _defer_ensure_sequencer_scene()
    # ★「+」から足して開いた時は msgbus が鳴らない。ここで入ったことに気づく(エンジン)。
    _auto_engine_notice(context)
    space = context.space_data
    if getattr(space, "view_type", 'SEQUENCER') == 'PREVIEW':
        # プレビュー側にはストリップが無いので出さない。
        return
    self.layout.operator("falcon_vse.fit_frame_range", icon='ARROW_LEFTRIGHT')


# -----------------------------------------------------------------------------
# レンダー項目「VSE」 (2026-09-12)
# -----------------------------------------------------------------------------
#
# 作者 2026-09-12「VSE 専用のレンダリング項目を作りたい」「名前は VSE にして」
# 「レンダープロパティと出力プロパティの変更などをしたい」
# 「レンダーと出力プロパティは1つに統合する」。
#
# ★絵は描かない。`do_render_full_pipeline()` は
#     if (RE_engine_render(re, true)) { /* 外部エンジンが全部持っていく */ }
#     else if (RE_seq_render_active(...)) { do_render_sequencer(re); }
#   の順で、`RE_engine_render(re, true)` は **`bl_use_postprocess` が立っていない
#   エンジンには false を返す**(`engine.cc:1016`)。
#   ⇒ この項目を選んでも **Sequencer の書き出しは素の Blender とまったく同じ道**を通る。
#   ストリップが1本も無い時だけ下の `render()` が呼ばれるので、空の結果を返す。
#
# ★**既存のパネルは1行も書き換えない。**出す場所を移すために必要なのは
#   「同じ `draw` を別の場所から呼ぶ」ことだけなので、**クラスは継がず関数だけ借りる**。
#   登録済みの `Panel` を継ぐと、継がれた側の `draw`/`poll` が RNA から外れて
#   **元の場所から黙って消える**((internal notes))。門でも「借り元が生きていること」を見る。

FALCON_VSE_ENGINE = 'FALCON_VSE'

# 既存パネルのうち、`COMPAT_ENGINES` に足すだけで出る物。
# ★ここに載るのは **もともと Render タブ (`bl_context='render'`) に居る物だけ**。
#   出力タブの物は「足す」のでなく、下の `FALCON_VSE_PT_*` として Render タブへ出す。
_COMPAT_PANELS = (
    "RENDER_PT_color_management",
    "RENDER_PT_color_management_working_space",
    "RENDER_PT_color_management_advanced",
    "RENDER_PT_color_management_curves",
    "RENDER_PT_color_management_white_balance",
)

# 門が「出さない物が混ざっていないか」を舐める先。
_ENGINE_UI_MODULES = ("bl_ui.properties_render", "bl_ui.properties_output")

# 出力タブに出す1行(Render タブへまとめた事を知らせるだけ)。
OUTPUT_MOVED_TEXT = "With VSE, the output settings are in the Render tab"


class FALCON_VSE_RenderEngine(RenderEngine):
    """VSE (settings only; the Sequencer renders the image)"""
    bl_idname = FALCON_VSE_ENGINE
    bl_label = "VSE"
    bl_use_preview = False

    def render(self, depsgraph):
        # ★ここへ来るのは「ストリップが1本も無い」時だけ。空の結果を返す。
        scene = depsgraph.scene
        scale = scene.render.resolution_percentage / 100.0
        width = max(1, int(scene.render.resolution_x * scale))
        height = max(1, int(scene.render.resolution_y * scale))
        result = self.begin_result(0, 0, width, height)
        try:
            result.layers[0].passes["Combined"].rect = (
                [[0.0, 0.0, 0.0, 0.0]] * (width * height))
        except Exception as ex:  # noqa: BLE001  書き出しを止めない
            print("falcon_vse_bridge:", ex)
        self.end_result(result)


# --- エンジンの一覧: VSE は VSE を開いている時だけ (2026-09-19 作者) -----------
#
# 作者「専用 VSE のレンダーエンジンは VSE 開いているときのみにしよう、
#       通常のレンダリングエンジンに出てるとややこしいから」。
# ⇒ Render タブの「Render Engine」の一覧から、Sequencer の見えていない窓では VSE を外す。
#   ★クラスの登録を外す形は採らない。外すと
#     - 本体が `ED_render_engine_changed` で全部のビューポートのレンダーを止め、
#       `RE_FreeAllPersistentData` を呼ぶ(出入りのたびに Rendered のビューポートが止まる)
#     - VSE を選んだシーンが、一覧では先頭の EEVEE を選んでいるように見え
#       (`rna_RenderSettings_engine_get` は見つからないと 0)、F12 も EEVEE に落ちる
#       (`RE_engines_find`)。Ctrl+Z で VSE に戻った時も同じ形になる
#     - レンダー中に外すと、ジョブが握っているエンジンの型を解放してしまう
#   ⇒ エンジンは登録したまま、**一覧の見せ方だけ**を替える。本体の一覧
#     (`RenderSettings.engine`)は Python から絞れないので、同じ値を読み書きする写しの項目
#     (`Scene.falcon_vse_render_engine`・保存しない)を置き、`RENDER_PT_context` の描画で
#     本体の項目の代わりに出す。見た目と言葉は本体と同じ(「Render Engine」)。
#   - VSE を出すのは: その窓に Sequencer がある時 / そのシーンが今 VSE を選んでいる時
#     (選ばれている値は一覧に要る)
#   - 背景(-b)の書き出しと、Python からの `scene.render.engine = ...` には何も効かない
#   `FALCON_VSE_ENGINE_ONLY_IN_VSE=0` で本体の一覧のまま(前の挙動)。

# C で登録される本体のエンジン(`DRW_engines_register` の順・アドオンより先)。
_BUILTIN_ENGINES = (
    ("BLENDER_EEVEE", "EEVEE"),
    ("BLENDER_WORKBENCH", "Workbench"),
)

# ★EnumProperty の items は参照を持っておかないと名前が化ける。
_engine_menu_items_cache = []

# 差し替える前の `RENDER_PT_context.draw`(外す時に戻す)。
_orig_render_context_draw = None


def engine_only_in_vse_enabled():
    """VSE を開いていない窓の一覧から VSE を外すか。呼ばれるたびに環境変数を読む。"""
    return os.environ.get("FALCON_VSE_ENGINE_ONLY_IN_VSE", "1").strip().lower() not in (
        "", "0", "off", "false", "no",
    )


def _engine_classes(base=None):
    base = base or bpy.types.RenderEngine
    for cls in base.__subclasses__():
        yield cls
        yield from _engine_classes(cls)


def render_engines():
    """登録されているエンジンの (idname, 名前)。本体の一覧(`R_engines`)の順。

    ★アドオンのエンジンはクラスの定義順 = 登録順(アドオンを入れ切りし直した時だけ
      本体の並びとずれることがある。値の読み書きは idname で合わせるので壊れない)。
    """
    engines = list(_BUILTIN_ENGINES)
    seen = {idname for idname, _name in engines}
    for cls in _engine_classes():
        idname = getattr(cls, "bl_idname", None)
        if not idname or idname in seen or not getattr(cls, "is_registered", False):
            continue
        seen.add(idname)
        engines.append((idname, getattr(cls, "bl_label", "") or idname))
    return engines


def vse_engine_listed(screen, scene):
    """その窓の一覧に VSE を出すか。"""
    if not engine_only_in_vse_enabled():
        return True
    if scene is not None and scene.render.engine == FALCON_VSE_ENGINE:
        return True  # 選ばれている値は一覧に要る
    if screen is None:
        return True  # 窓が分からない時は絞らない
    return _screen_has_sequencer(screen)


def _engine_menu_numbered(scene):
    """(番号, idname, 名前)。番号は絞る前の並びの位置で、絞っても変わらない。"""
    engines = render_engines()
    current = scene.render.engine if scene is not None else None
    if current and current not in {idname for idname, _name in engines}:
        engines.append((current, current))  # 数え漏れたエンジンでも今の値は出す
    return [(number, idname, name) for number, (idname, name) in enumerate(engines)]


def _engine_menu_items(self, context):
    listed = vse_engine_listed(getattr(context, "screen", None) if context else None, self)
    items = [(idname, name, "", number)
             for number, idname, name in _engine_menu_numbered(self)
             if listed or idname != FALCON_VSE_ENGINE]
    _engine_menu_items_cache[:] = items
    return items


def _engine_menu_get(self):
    current = self.render.engine
    for number, idname, _name in _engine_menu_numbered(self):
        if idname == current:
            return number
    return 0


def _engine_menu_set(self, value):
    for number, idname, _name in _engine_menu_numbered(self):
        if number == value:
            if self.render.engine != idname:
                self.render.engine = idname
            return


def _draw_render_context(self, context):
    """`RENDER_PT_context.draw` の代わり。一覧の VSE の出し分けの他は本体と同じ。"""
    if not engine_only_in_vse_enabled() or _orig_render_context_draw is None:
        if _orig_render_context_draw is not None:
            _orig_render_context_draw(self, context)
        return
    layout = self.layout
    layout.use_property_split = True
    layout.use_property_decorate = False

    scene = context.scene
    rd = scene.render

    if rd.has_multiple_engines:
        layout.prop(scene, "falcon_vse_render_engine", text="Render Engine")


def _is_orig_render_context_draw(func):
    return (getattr(func, "__qualname__", "") == "RENDER_PT_context.draw"
            and getattr(func, "__module__", "") == "bl_ui.properties_render")


def _install_render_context_draw():
    """本体の描画を差し替える。★`append` は使わない(持ち主の絞り込みの印が付く)。

    Cycles が `RENDER_PT_context.append(draw_device)` で足しているので、`draw` は
    `_draw_funcs` の並びになっている。その先頭(本体の描画)だけを入れ替える。
    """
    global _orig_render_context_draw
    from bl_ui.properties_render import RENDER_PT_context
    funcs = RENDER_PT_context._dyn_ui_initialize()
    for index, func in enumerate(funcs):
        if _is_orig_render_context_draw(func):
            _orig_render_context_draw = func
            funcs[index] = _draw_render_context
            return True
    print("falcon_vse_bridge: RENDER_PT_context draw function not found (the engine list stays as is)")
    return False


def _remove_render_context_draw():
    global _orig_render_context_draw
    from bl_ui.properties_render import RENDER_PT_context
    funcs = getattr(RENDER_PT_context.draw, "_draw_funcs", None)
    if funcs is not None and _orig_render_context_draw is not None:
        for index, func in enumerate(funcs):
            if func is _draw_render_context:
                funcs[index] = _orig_render_context_draw
    _orig_render_context_draw = None


def compat_panels():
    """`COMPAT_ENGINES` に足すだけで出す既存パネル。"""
    import importlib
    found = []
    try:
        module = importlib.import_module("bl_ui.properties_render")
    except Exception as ex:  # noqa: BLE001
        print("falcon_vse_bridge:", ex)
        return found
    for name in _COMPAT_PANELS:
        cls = getattr(module, name, None)
        if cls is not None and isinstance(getattr(cls, "COMPAT_ENGINES", None), set):
            found.append(cls)
    return found


def engine_ui_panels():
    """Blender 側の「エンジンごとに出し分けるパネル」を全部集める(門用)。"""
    import importlib
    found = []
    for name in _ENGINE_UI_MODULES:
        try:
            module = importlib.import_module(name)
        except Exception as ex:  # noqa: BLE001
            print("falcon_vse_bridge:", ex)
            continue
        for attr in dir(module):
            cls = getattr(module, attr)
            if isinstance(getattr(cls, "COMPAT_ENGINES", None), set):
                found.append(cls)
    return found


# -----------------------------------------------------------------------------
# 環境変数の状態(表示だけ。ここからは倒さない)
# -----------------------------------------------------------------------------

def _env_int(name, default, empty):
    """C++ 側の `atoi` と同じ読み方。読めない文字は 0。"""
    value = os.environ.get(name)
    if value is None:
        return default
    if value == "":
        return empty
    text = value.strip()
    sign, digits = 1, ""
    if text[:1] in "+-":
        sign, text = (-1 if text[0] == "-" else 1), text[1:]
    for ch in text:
        if not ch.isdigit():
            break
        digits += ch
    return sign * int(digits) if digits else 0


def fastpath_enabled():
    """切っただけの経路(`FALCON_VSE_FASTPATH`)。既定 ON。

    ★C++ 側は**最初の1回だけ**読んで固める(`strip_fastpath.cc`)。
      ここは「今のプロセスが起動時に何を読んだか」を出すだけ。
    """
    return _env_int("FALCON_VSE_FASTPATH", 1, 1) != 0


def cache_soft_limit_mb():
    """キャッシュを太らせない下限 (MB)。`FALCON_VSE_MEM_SOFT_MB`・既定 4096。

    `strip_relations.cc` は `env != nullptr` なら `max(0, atoi(env))` なので、
    空文字は 0 (= 切) になる。同じ読み方に合わせてある。
    """
    return max(0, _env_int("FALCON_VSE_MEM_SOFT_MB", 4096, 0))


def sequencer_rows(scene):
    """「Sequencer」パネルが出す物の一覧。

    ★描画そのものは門から見えないので、**出す物の決め方をここ1箇所に寄せる**
      (`output_rows()` と同じ作法)。`("prop", 名前)` は `scene.render` の物。
    """
    return [
        ("prop", "use_sequencer"),
        ("prop", "use_sequencer_override_scene_strip"),
        ("prop", "sequencer_gl_preview"),
    ]


def sequencer_cache_rows(scene):
    """「Cache Settings」が出す物の一覧。

    `("ed", 名前)` は `scene.sequence_editor` の物。ストリップを一度も
    置いていないシーンには `sequence_editor` が無いので、その時は摘みを出さない。
    """
    rows = []
    editor = getattr(scene, "sequence_editor", None)
    if editor is not None:
        rows.append(("ed", "use_prefetch"))
        rows.append(("ed", "use_cache_raw"))
        rows.append(("ed", "use_cache_final"))
    limit = cache_soft_limit_mb()
    if limit:
        rows.append(("label", iface_("Cache growth stops below %d MB free "
                                     "(FALCON_VSE_MEM_SOFT_MB)") % limit))
    else:
        rows.append(("label", "Cache growth limit: off (FALCON_VSE_MEM_SOFT_MB=0)"))
    return rows


def encoding_status_rows(scene):
    """「Encoding」の下に出す状態の行。

    ★`FALCON_VSE_FASTPATH` は環境変数なのでここからは倒せない。**状態の表示だけ**。
      「この書き出しで効いたか」の1行 (`falcon_last_export_info`) は
      出力 (Output) の欄に既に出ているので、ここでは重ねない。
    """
    if fastpath_enabled():
        return [("label", "Cut-only fast path: on")]
    return [("label", "Cut-only fast path: off (FALCON_VSE_FASTPATH=0)")]


# -----------------------------------------------------------------------------
# Render タブへまとめたパネル
# -----------------------------------------------------------------------------

class _VSERenderPanel:
    """★登録済みの `Panel` ではない素の mixin。継承の乗っ取りは起きない。"""
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "render"
    COMPAT_ENGINES = {FALCON_VSE_ENGINE}

    @classmethod
    def poll(cls, context):
        return context.engine in cls.COMPAT_ENGINES


def _ffmpeg_format(context):
    return context.scene.render.image_settings.file_format in {
        'FFMPEG', 'XVID', 'H264', 'THEORA'}


def _draw_rows(layout, scene, rows):
    for kind, value in rows:
        if kind == "prop":
            layout.prop(scene.render, value)
        elif kind == "ed":
            editor = getattr(scene, "sequence_editor", None)
            if editor is not None:
                layout.prop(editor, value)
        elif kind == "label":
            row = layout.row()
            row.active = False
            row.label(text=value)


class FALCON_VSE_PT_format(_VSERenderPanel, Panel):
    bl_label = "Format"
    bl_order = 10

    def draw_header_preset(self, _context):
        # プリセット(1080p / 1440p / 4K UHD / 縦…)は既存の物。
        from bl_ui.properties_output import RENDER_PT_format_presets
        RENDER_PT_format_presets.draw_panel_header(self.layout)

    @staticmethod
    def draw_framerate(layout, rd):
        # 借り元の `draw` が `self.draw_framerate` を呼ぶので、口だけ用意する。
        from bl_ui.properties_output import RENDER_PT_format
        RENDER_PT_format.draw_framerate(layout, rd)

    def draw(self, context):
        from bl_ui.properties_output import RENDER_PT_format
        RENDER_PT_format.draw(self, context)


class FALCON_VSE_PT_frame_range(_VSERenderPanel, Panel):
    bl_label = "Frame Range"
    bl_order = 11

    def draw(self, context):
        from bl_ui.properties_output import RENDER_PT_frame_range
        RENDER_PT_frame_range.draw(self, context)
        if bridge_enabled():
            # 2026-09-12 (fix3) のボタン。ここは VSE の時だけ出る新しいパネルなので、
            # 既存の `RENDER_PT_frame_range` の描画は1行も変わらない。
            column = self.layout.column()
            column.operator("falcon_vse.fit_frame_range", icon='ARROW_LEFTRIGHT')


class FALCON_VSE_PT_sequencer(_VSERenderPanel, Panel):
    bl_label = "Sequencer"
    bl_order = 12

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        _draw_rows(layout, context.scene, sequencer_rows(context.scene))


class FALCON_VSE_PT_sequencer_cache(_VSERenderPanel, Panel):
    bl_label = "Cache Settings"
    bl_parent_id = "FALCON_VSE_PT_sequencer"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        _draw_rows(layout, context.scene, sequencer_cache_rows(context.scene))


class FALCON_VSE_PT_motion_blur(_VSERenderPanel, Panel):
    bl_label = "Motion Blur"
    bl_options = {'DEFAULT_CLOSED'}
    bl_order = 13

    def draw_header(self, context):
        self.layout.prop(context.scene.render, "use_motion_blur", text="")

    def draw(self, context):
        # ★EEVEE の欄をそのまま借りない。あちらは `scene.eevee` の 3 つ
        #   (depth_scale / max / steps) も出すが、あれは EEVEE で焼く側の摘みで
        #   VSE の欄に出しても効かない。シャッターだけを出す。
        layout = self.layout
        layout.use_property_split = True
        rd = context.scene.render
        layout.active = rd.use_motion_blur
        col = layout.column()
        col.prop(rd, "motion_blur_position", text="Position")
        col.prop(rd, "motion_blur_shutter")


class FALCON_VSE_PT_motion_blur_curve(_VSERenderPanel, Panel):
    bl_label = "Shutter Curve"
    bl_parent_id = "FALCON_VSE_PT_motion_blur"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        from bl_ui.properties_render import RENDER_PT_eevee_motion_blur_curve
        RENDER_PT_eevee_motion_blur_curve.draw(self, context)


class FALCON_VSE_PT_output(_VSERenderPanel, Panel):
    bl_label = "Output"
    bl_order = 20

    def draw_header(self, context):
        from bl_ui.properties_output import RENDER_PT_output
        RENDER_PT_output.draw_header(self, context)

    def draw(self, context):
        # ★`RENDER_PT_output.draw` は `append()` された後は包みの `draw_ls` なので、
        #   このアドオンが足している VSE の行 (`_draw_output`) もここで一緒に出る。
        from bl_ui.properties_output import RENDER_PT_output
        RENDER_PT_output.draw(self, context)


class FALCON_VSE_PT_encoding(_VSERenderPanel, Panel):
    bl_label = "Encoding"
    bl_parent_id = "FALCON_VSE_PT_output"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return _VSERenderPanel.poll.__func__(cls, context) and _ffmpeg_format(context)

    def draw_header_preset(self, _context):
        from bl_ui.properties_output import RENDER_PT_ffmpeg_presets
        RENDER_PT_ffmpeg_presets.draw_panel_header(self.layout)

    def draw(self, context):
        from bl_ui.properties_output import RENDER_PT_encoding
        RENDER_PT_encoding.draw(self, context)
        _draw_rows(self.layout, context.scene, encoding_status_rows(context.scene))


class FALCON_VSE_PT_encoding_video(_VSERenderPanel, Panel):
    bl_label = "Video"
    bl_parent_id = "FALCON_VSE_PT_encoding"

    @classmethod
    def poll(cls, context):
        return _VSERenderPanel.poll.__func__(cls, context) and _ffmpeg_format(context)

    def draw_vcodec(self, context):
        from bl_ui.properties_output import RENDER_PT_encoding_video
        RENDER_PT_encoding_video.draw_vcodec(self, context)

    def draw(self, context):
        from bl_ui.properties_output import RENDER_PT_encoding_video
        RENDER_PT_encoding_video.draw(self, context)


class FALCON_VSE_PT_encoding_audio(_VSERenderPanel, Panel):
    bl_label = "Audio"
    bl_parent_id = "FALCON_VSE_PT_encoding"

    @classmethod
    def poll(cls, context):
        return _VSERenderPanel.poll.__func__(cls, context) and _ffmpeg_format(context)

    def draw(self, context):
        from bl_ui.properties_output import RENDER_PT_encoding_audio
        RENDER_PT_encoding_audio.draw(self, context)


class FALCON_VSE_PT_metadata(_VSERenderPanel, Panel):
    bl_label = "Metadata"
    bl_options = {'DEFAULT_CLOSED'}
    bl_order = 30

    def draw(self, context):
        from bl_ui.properties_output import RENDER_PT_stamp
        RENDER_PT_stamp.draw(self, context)


class FALCON_VSE_PT_metadata_note(_VSERenderPanel, Panel):
    bl_label = "Note"
    bl_parent_id = "FALCON_VSE_PT_metadata"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, context):
        from bl_ui.properties_output import RENDER_PT_stamp_note
        RENDER_PT_stamp_note.draw_header(self, context)

    def draw(self, context):
        from bl_ui.properties_output import RENDER_PT_stamp_note
        RENDER_PT_stamp_note.draw(self, context)


class FALCON_VSE_PT_metadata_burn(_VSERenderPanel, Panel):
    bl_label = "Burn Into Image"
    bl_parent_id = "FALCON_VSE_PT_metadata"
    bl_options = {'DEFAULT_CLOSED'}

    def draw_header(self, context):
        from bl_ui.properties_output import RENDER_PT_stamp_burn
        RENDER_PT_stamp_burn.draw_header(self, context)

    def draw(self, context):
        from bl_ui.properties_output import RENDER_PT_stamp_burn
        RENDER_PT_stamp_burn.draw(self, context)


class FALCON_VSE_PT_output_moved(Panel):
    # 出力タブに1行だけ。「壊れている」と読まれないように(docstring にすると tooltip になる)。
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "output"
    bl_label = "Output"
    bl_options = {'HIDE_HEADER'}
    COMPAT_ENGINES = {FALCON_VSE_ENGINE}

    @classmethod
    def poll(cls, context):
        return context.engine in cls.COMPAT_ENGINES

    def draw(self, context):
        row = self.layout.row()
        row.active = False
        row.label(text=OUTPUT_MOVED_TEXT, icon='INFO')


# ★門が読む表。ここが「出す物の一覧」の正本。
VSE_PANELS = (
    FALCON_VSE_PT_format,
    FALCON_VSE_PT_frame_range,
    FALCON_VSE_PT_sequencer,
    FALCON_VSE_PT_sequencer_cache,
    FALCON_VSE_PT_motion_blur,
    FALCON_VSE_PT_motion_blur_curve,
    FALCON_VSE_PT_output,
    FALCON_VSE_PT_encoding,
    FALCON_VSE_PT_encoding_video,
    FALCON_VSE_PT_encoding_audio,
    FALCON_VSE_PT_metadata,
    FALCON_VSE_PT_metadata_note,
    FALCON_VSE_PT_metadata_burn,
    FALCON_VSE_PT_output_moved,
)

# 借り元。**この一覧の物が RNA から外れていないこと**を門で見る
# ((internal notes))。
BORROWED_PANELS = (
    ("bl_ui.properties_output", "RENDER_PT_format"),
    ("bl_ui.properties_output", "RENDER_PT_frame_range"),
    ("bl_ui.properties_output", "RENDER_PT_output"),
    ("bl_ui.properties_output", "RENDER_PT_encoding"),
    ("bl_ui.properties_output", "RENDER_PT_encoding_video"),
    ("bl_ui.properties_output", "RENDER_PT_encoding_audio"),
    ("bl_ui.properties_output", "RENDER_PT_stamp"),
    ("bl_ui.properties_output", "RENDER_PT_stamp_note"),
    ("bl_ui.properties_output", "RENDER_PT_stamp_burn"),
    ("bl_ui.properties_render", "RENDER_PT_eevee_motion_blur_curve"),
)


# -----------------------------------------------------------------------------
# 登録
# -----------------------------------------------------------------------------

_handlers = (
    ("load_post", _on_load_post),
    ("render_init", _on_render_init),
    ("render_write", _on_render_write),
    ("render_complete", _on_render_complete),
    ("render_cancel", _on_render_cancel),
    ("undo_post", _on_undo_redo),
    ("redo_post", _on_undo_redo),
)

classes = (
    FALCON_OT_vse_edit,
    FALCON_VSE_OT_fit_frame_range,
    FALCON_VSE_RenderEngine,
) + VSE_PANELS


def register():
    from . import scene_follow
    scene_follow.register()
    # ★置き場(Output / Render タブ)は登録の時にしか効かないので、クラスを登録する前に決める。
    from . import output_panels
    output_panels.prepare()
    for cls in classes:
        bpy.utils.register_class(cls)

    bpy.types.Scene.falcon_output_name = StringProperty(
        name="Output Name",
        description=(
            "Name of the files the render writes. "
            "When empty, the output path is used as is"
        ),
        default="",
    )

    bpy.types.Scene.falcon_vse_share_output = BoolProperty(
        name="Share Output with VSE",
        description="When a render finishes, add what was written to the output path to the "
                    "Sequencer",
        default=True,
    )

    bpy.types.Scene.falcon_vse_browser_follow_output = BoolProperty(
        name="Browser Follows Output",
        description=(
            "When Video Editing opens, point its File Browser at the render output folder. "
            "Off leaves Blender's default"
        ),
        default=True,
    )

    # エンジンの自動切り替えの覚え(シーンに保存される・画面には出さない)。
    bpy.types.Scene.falcon_vse_engine_before = StringProperty(
        name="Engine Before VSE",
        description="Engine to return to when leaving a screen with a Sequencer",
        default="",
        options={'HIDDEN'},
    )
    bpy.types.Scene.falcon_vse_engine_auto = BoolProperty(
        name="In VSE Screen",
        description="Whether the engine was switched to VSE because a screen with a Sequencer "
                    "is shown",
        default=False,
        options={'HIDDEN'},
    )

    for name, function in _handlers:
        handler = getattr(bpy.app.handlers, name)
        if function not in handler:
            handler.append(function)

    from bl_ui.properties_output import RENDER_PT_output
    RENDER_PT_output.append(_draw_output)
    from bl_ui.space_topbar import TOPBAR_HT_upper_bar
    TOPBAR_HT_upper_bar.append(_draw_topbar_notice)
    from bl_ui.space_image import IMAGE_HT_header
    IMAGE_HT_header.append(_draw_image_header)
    from bl_ui.space_sequencer import SEQUENCER_MT_add, SEQUENCER_HT_header
    SEQUENCER_MT_add.append(_draw_sequencer_add)
    SEQUENCER_HT_header.append(_draw_sequencer_header)
    from bl_ui.space_filebrowser import FILEBROWSER_HT_header
    FILEBROWSER_HT_header.append(_on_browser_header_draw)

    for _panel in compat_panels():
        _panel.COMPAT_ENGINES.add(FALCON_VSE_ENGINE)

    # 値の正本は `scene.render.engine`。これは一覧の見せ方だけの写し(保存しない)。
    bpy.types.Scene.falcon_vse_render_engine = EnumProperty(
        name="Engine",
        description="Engine to use for rendering",
        items=_engine_menu_items,
        get=_engine_menu_get,
        set=_engine_menu_set,
        options=set(),
    )
    _install_render_context_draw()

    _subscribe_workspace()
    from . import filebrowser_keys
    filebrowser_keys.register()
    from . import media_info
    media_info.register()


def unregister():
    from . import filebrowser_keys
    filebrowser_keys.unregister()
    from . import media_info
    media_info.unregister()
    if not bpy.app.background:
        try:
            bpy.msgbus.clear_by_owner(_msgbus_owner)
        except Exception as ex:  # noqa: BLE001
            print("falcon_vse_bridge:", ex)

    from bl_ui.space_filebrowser import FILEBROWSER_HT_header
    FILEBROWSER_HT_header.remove(_on_browser_header_draw)
    from bl_ui.space_sequencer import SEQUENCER_MT_add, SEQUENCER_HT_header
    SEQUENCER_HT_header.remove(_draw_sequencer_header)
    SEQUENCER_MT_add.remove(_draw_sequencer_add)
    from bl_ui.space_image import IMAGE_HT_header
    IMAGE_HT_header.remove(_draw_image_header)
    from bl_ui.properties_output import RENDER_PT_output
    RENDER_PT_output.remove(_draw_output)
    from bl_ui.space_topbar import TOPBAR_HT_upper_bar
    TOPBAR_HT_upper_bar.remove(_draw_topbar_notice)

    for name, function in _handlers:
        handler = getattr(bpy.app.handlers, name)
        if function in handler:
            handler.remove(function)

    if bpy.app.timers.is_registered(flush_pending):
        bpy.app.timers.unregister(flush_pending)
    if bpy.app.timers.is_registered(_browser_retry):
        bpy.app.timers.unregister(_browser_retry)
    if bpy.app.timers.is_registered(_ensure_seq_scene_deferred):
        bpy.app.timers.unregister(_ensure_seq_scene_deferred)
    if bpy.app.timers.is_registered(_jump_to_video_editing):
        bpy.app.timers.unregister(_jump_to_video_editing)
    if bpy.app.timers.is_registered(_remember_engines_deferred):
        bpy.app.timers.unregister(_remember_engines_deferred)
    for function in (_auto_engine_deferred, _auto_engine_watch):
        if bpy.app.timers.is_registered(function):
            bpy.app.timers.unregister(function)
    global _auto_last_sig, _auto_dirty, _auto_reason
    _auto_last_sig, _auto_dirty, _auto_reason = None, True, None
    _engine_seen.clear()
    _browser_seen.clear()
    _written.clear()
    _pending.clear()

    # ★差し替えたままアドオンを外されても、.blend に残さない。
    for scene_name in list(_saved_filepath):
        restore_output_name(bpy.data.scenes.get(scene_name))
    _saved_filepath.clear()

    for _panel in compat_panels():
        _panel.COMPAT_ENGINES.discard(FALCON_VSE_ENGINE)

    _remove_render_context_draw()
    del bpy.types.Scene.falcon_vse_render_engine
    _engine_menu_items_cache.clear()

    del bpy.types.Scene.falcon_vse_engine_auto
    del bpy.types.Scene.falcon_vse_engine_before
    del bpy.types.Scene.falcon_vse_browser_follow_output
    del bpy.types.Scene.falcon_vse_share_output
    del bpy.types.Scene.falcon_output_name

    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
    from . import scene_follow
    scene_follow.unregister()
