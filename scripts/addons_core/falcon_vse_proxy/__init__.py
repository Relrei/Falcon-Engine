# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""One button that makes a video edit playable on a machine with little memory.

Measured on 2026-09-20 (FHD H.264, four overlapping strips, 512 MB cache budget, provisional):

    preview at 100%            1.7 / 2.2 fps
    preview at 50%, no proxy   1.5 / 2.0 fps   <- lowering the preview alone is *not* a win:
                                                  every frame is still decoded at full size and
                                                  only then scaled down, which is more work
    50% proxy files            4.5 / 6.9 / 9.4 fps, CPU per displayed frame 1/4.4
                               (building them: 960x540 H.264, 601 frames x 4 strips,
                                2.6 MB, 6-7 seconds)

So what helps is the proxy *files*, not the preview size, and they are cheap to make. This
add-on sets the strips up and starts the build, so that nobody has to know the word "proxy"
to get a timeline that plays.

Turn it off with `FALCON_VSE_PROXY_HELPER=0`.
"""

bl_info = {
    "name": "VSE Proxy Helper",
    "author": "Falcon Engine",
    "version": (0, 1, 0),
    "blender": (5, 2, 0),
    "location": "Video Sequencer > Sidebar > Strip > Proxy Settings",
    "description": "Set up and build half-size proxies so playback stays smooth on little memory",
    "category": "Sequencer",
}

import os

import bpy
# 日本語は Falcon の一枚表(`scripts/startup/falcon_i18n.py`)側にあります。
from bpy.app.translations import pgettext_rpt as rpt_
from bpy.props import BoolProperty

#: Half size is the measured sweet spot: a quarter of the pixels, and the picture is still
#: good enough to cut on. 25% was not faster than 50% in the same test (both are decode-bound
#: on the proxy file, which is already small).
PROXY_RENDER_SIZE = 'PROXY_50'
#: JPEG-ish quality of the proxy file. 50 keeps it small; the proxy is never what gets rendered.
PROXY_QUALITY = 50

def _enabled():
    return os.environ.get("FALCON_VSE_PROXY_HELPER", "1") not in {"0", "false", "False"}


def _sequencer_scene(context):
    """ストリップの在るシーン。

    Falcon はストリップを専用の `VSE` シーンへ置き、窓のシーン(`context.scene`)はレンダー用の
    まま残す。VSE が読むのはワークスペースの `sequencer_scene` なので、そちらを見る
    (取れない時だけ `context.scene`)。
    """
    scene = getattr(context, "sequencer_scene", None)
    return scene if scene is not None else context.scene


def _proxy_strips(scene):
    """Strips a proxy can be built for (movies and image sequences)."""
    ed = scene.sequence_editor
    if ed is None:
        return []
    return [strip for strip in ed.strips_all if strip.type in {'MOVIE', 'IMAGE'}]


def _mark_strip(strip):
    """このストリップに「50% のプロキシを使う」印を付ける(作成はしない)。"""
    strip.use_proxy = True
    proxy = strip.proxy
    proxy.build_25 = False
    proxy.build_50 = True
    proxy.build_75 = False
    proxy.build_100 = False
    proxy.quality = PROXY_QUALITY
    # 既に在る物は作り直さない(押し直しても待たされない)。
    proxy.use_overwrite = False


def _project_uses_proxies(scene):
    """この編集が既に「プロキシで再生する」状態か。"""
    ed = scene.sequence_editor
    if ed is None:
        return False
    return any(strip.use_proxy for strip in ed.strips_all if hasattr(strip, "proxy"))


class SEQUENCER_OT_falcon_proxy_setup(bpy.types.Operator):
    """Build half-size proxies for every movie strip and play those back instead of the originals"""

    bl_idname = "sequencer.falcon_proxy_setup"
    bl_label = "Set Up Proxies for Playback"
    bl_options = {'REGISTER', 'UNDO'}

    build: BoolProperty(
        name="Build Now",
        description="Start building the proxy files right away",
        default=True,
    )

    @classmethod
    def poll(cls, context):
        scene = _sequencer_scene(context)
        return scene is not None and scene.sequence_editor is not None

    def execute(self, context):
        scene = _sequencer_scene(context)
        ed = scene.sequence_editor
        strips = _proxy_strips(scene)
        if not strips:
            self.report({'INFO'}, rpt_("No movie or image strips to set up"))
            return {'CANCELLED'}

        # 置き場は触りません(Blender の既定 = 素材の隣の `BL_proxy`)。
        # 素材の置き場が書けない時は「プロジェクトの隣にまとめる」(Storage: Project)へ
        # 手で変えられます。こちらで勝手に変えると、既に作ってある代役を見失います。

        for strip in strips:
            _mark_strip(strip)

        # 表示側を代役に向ける。向けないとファイルだけ作って誰も使わない。
        for window in context.window_manager.windows:
            for area in window.screen.areas:
                if area.type != 'SEQUENCE_EDITOR':
                    continue
                for space in area.spaces:
                    if space.type == 'SEQUENCE_EDITOR':
                        space.proxy_render_size = PROXY_RENDER_SIZE
                        # ★これが無いと、代役を作るだけで再生は元のまま(大きさだけ縮む)。
                        space.use_proxies = True

        self.report({'INFO'}, rpt_("Proxies set up for %d strip(s)") % len(strips))

        if self.build:
            # ★Blender の作成は「選択されているストリップ」しか見ない
            #   (`sequencer_proxy.cc` の `SEQ_SELECT` の検査・選択が無いと警告を出して何もしない)。
            #   ここが手作業でいちばん取りこぼす所なので、こちらで選び直してから呼び、後で戻す。
            selection = {strip.name: strip.select for strip in ed.strips_all}
            targets = {strip.name for strip in strips}
            for strip in ed.strips_all:
                strip.select = strip.name in targets
            try:
                # 窓がある時は裏のジョブ(押した後も編集を続けられる)、`-b` では同期。
                # 実測 2026-09-20: FHD 20 秒 × 4 本 = 2.55 MB・裏で数秒、`-b` で 6.7 秒。
                if bpy.app.background:
                    bpy.ops.sequencer.rebuild_proxy()
                else:
                    bpy.ops.sequencer.rebuild_proxy('INVOKE_DEFAULT')
            finally:
                for strip in ed.strips_all:
                    strip.select = selection.get(strip.name, False)

        return {'FINISHED'}



# -------------------------------------------------------------------------------------------
# 後から足したストリップを取りこぼさない
#
# ★一度プロキシで再生する状態にした編集に、後から動画を足すと、その 1 本だけ元の大きさで
#   復号される。そこだけ重くなるのに、画面には何も出ないので理由が分からない
#   (手で「プロキシ設定 → 選んで作り直す」を毎回やる人はいない)。
#   そこで、**既にプロキシを使っている編集に限り**、新しく来たストリップへ同じ印を付け、
#   裏で作成まで走らせる。まだ 1 本もプロキシを使っていない編集には何もしない。
# `FALCON_VSE_PROXY_AUTO=0` で自動だけ止まる(ボタンは残る)。
# -------------------------------------------------------------------------------------------

_auto_state = {"busy": False, "pending": False}
#: 見終えたストリップ名(シーンの `session_uid` ごと)。
#: ★「プロキシの印が無い」だけでは、後から足した物と区別が付かない。使う人が自分で
#:   Strip Proxy を外した物や、一部だけ作った状態で開いた物まで毎回付け直してしまう。
#:   そこで**この作業中に初めて見えた物だけ**を新しいストリップとして扱う
#:   (最初に見た時点で在った物は基準として覚えるだけで、何もしない)。
_auto_seen = {}
#: タイマー待ちのストリップ名(シーン名ごと)。
_auto_queue = {}


def _auto_enabled():
    return os.environ.get("FALCON_VSE_PROXY_AUTO", "1") not in {"0", "false", "False"}


def _new_unmarked_strips(scene):
    """この作業中に初めて見えた、まだプロキシの印が無いストリップ。"""
    ed = scene.sequence_editor
    if ed is None:
        return []
    strips = [strip for strip in ed.strips_all if strip.type in {'MOVIE', 'IMAGE'}]
    key = scene.session_uid
    seen = _auto_seen.get(key)
    # 消えた名前は忘れる(消して同じ名前で足し直した物は、新しい物として扱う)。
    _auto_seen[key] = {strip.name for strip in strips}
    if seen is None:
        return []
    return [strip for strip in strips if strip.name not in seen and not strip.use_proxy]


def _auto_apply():
    """タイマーから 1 回だけ走る。重い処理はここでだけ行う。"""
    _auto_state["pending"] = False
    queue = dict(_auto_queue)
    _auto_queue.clear()
    for scene_name, names in queue.items():
        # シーンはポインタではなく名前で持ち越す(待つ間に消えているかもしれない)。
        scene = bpy.data.scenes.get(scene_name)
        if scene is None or not _project_uses_proxies(scene):
            continue
        ed = scene.sequence_editor
        strips = [strip for strip in ed.strips_all
                  if strip.name in names and strip.type in {'MOVIE', 'IMAGE'} and not strip.use_proxy]
        if not strips:
            continue

        _auto_state["busy"] = True
        try:
            selection = {strip.name: strip.select for strip in ed.strips_all}
            targets = {strip.name for strip in strips}
            for strip in strips:
                _mark_strip(strip)
            for strip in ed.strips_all:
                strip.select = strip.name in targets
            try:
                if not bpy.app.background:
                    # 作成の操作が見るのはワークスペースの `sequencer_scene`。
                    # このシーンがそれでない場合に備えて、明示して呼ぶ。
                    with bpy.context.temp_override(sequencer_scene=scene):
                        bpy.ops.sequencer.rebuild_proxy('INVOKE_DEFAULT')
            finally:
                for strip in ed.strips_all:
                    strip.select = selection.get(strip.name, False)
            print("falcon_vse_proxy: %d 本に後から印を付けて作成を始めました" % len(targets))
        finally:
            _auto_state["busy"] = False
    return None


@bpy.app.handlers.persistent
def _on_depsgraph_update(scene, depsgraph=None):
    # 毎回の更新で走るので、ここでは**数えるだけ**にしてタイマーへ逃がす。
    if _auto_state["busy"] or not _auto_enabled():
        return
    if scene is None or scene.sequence_editor is None:
        return
    new = _new_unmarked_strips(scene)
    if not new or not _project_uses_proxies(scene):
        return
    _auto_queue.setdefault(scene.name, set()).update(strip.name for strip in new)
    if not _auto_state["pending"]:
        _auto_state["pending"] = True
        # ★persistent: 既定のままだとファイルを開いた時にタイマーだけ消され、
        #   `pending` が立ちっぱなしになって、以後の自動付けが止まる。
        bpy.app.timers.register(_auto_apply, first_interval=1.0, persistent=True)


@bpy.app.handlers.persistent
def _on_load_post(*_args):
    # 名前の一覧は開いたファイルのもの。持ち越すと別のファイルの同名のストリップを取り違える。
    _auto_seen.clear()
    _auto_queue.clear()


def _draw_proxy_panel(self, context):
    """Add the button to the sidebar's own Proxy Settings panel."""
    layout = self.layout
    column = layout.column(align=True)
    column.operator(SEQUENCER_OT_falcon_proxy_setup.bl_idname, icon='SEQ_PREVIEW')


classes = (SEQUENCER_OT_falcon_proxy_setup,)
_panel = None


def register():
    if not _enabled():
        return
    global _panel
    for cls in classes:
        bpy.utils.register_class(cls)
    _panel = getattr(bpy.types, "SEQUENCER_PT_proxy_settings", None)
    if _panel is not None:
        _panel.append(_draw_proxy_panel)
    if _on_depsgraph_update not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph_update)
    if _on_load_post not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_on_load_post)


def unregister():
    global _panel
    if _on_depsgraph_update in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_on_depsgraph_update)
    if _on_load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load_post)
    if bpy.app.timers.is_registered(_auto_apply):
        bpy.app.timers.unregister(_auto_apply)
    _auto_state["pending"] = False
    _auto_seen.clear()
    _auto_queue.clear()
    if _panel is not None:
        _panel.remove(_draw_proxy_panel)
        _panel = None
    for cls in reversed(classes):
        try:
            bpy.utils.unregister_class(cls)
        except Exception:
            pass
