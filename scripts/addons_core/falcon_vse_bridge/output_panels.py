# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""エンジンが VSE の時のプロパティの置き場 (2026-09-20)。

④ 出力の欄を Output タブへ戻す。作者 2026-09-20「出力がレンダープロパティにあって、
   出力プロパティが何もないから、そっちに移したい」。
   9-12 (FIX5) に「レンダーと出力プロパティは 1 つに統合する」で、VSE の時は出力の欄も
   全部 Render タブへ出していた(Output タブは知らせの 1 行だけ)。
   ⇒ 本家 5.2.2 で Output タブ(`properties_output.py`)に居る物は Output タブへ戻す:
        Format(Frame Rate を含む)・Frame Range・Output(+ Encoding / Video / Audio)・
        Metadata(+ Note / Burn Into Image)
      Render タブに残すのは VSE の書き出しに関わる物だけ:
        Sequencer(+ Cache Settings)・Motion Blur(+ Shutter Curve)・Color Management
      知らせの 1 行(`FALCON_VSE_PT_output_moved`)は、戻した時は出さない。
   `FALCON_VSE_OUTPUT_TAB=0` で 9-12 の置き方(全部 Render タブ)。★登録の時に読む(起動の時)。

③ Format の「Render Region」「Crop to Render Region」を出さない。作者 2026-09-20
   「レンダー範囲制限はいらないから項目削除したい(VSE)」。Sequencer の書き出しは
   `use_border` を見ないので、VSE の時は押しても何も起きない項目だった。
   本家の `RENDER_PT_format.draw` はそのまま借りて、その 2 つの `prop()` だけを落とす
   (それ以外は本家の描き方のまま = 本家が欄を変えても付いていく)。
   Cycles / EEVEE など 3D のエンジンの Format は本家の欄そのもの(ここは VSE の時だけ)。
   `FALCON_VSE_HIDE_RENDER_REGION=0` で出す(描く時に読む)。

★文字は変えない(言語の係の持ち物)。置き場と出す / 出さないだけ。
"""

import os

ENV_OUTPUT_TAB = "FALCON_VSE_OUTPUT_TAB"
ENV_HIDE_REGION = "FALCON_VSE_HIDE_RENDER_REGION"

# 本家 5.2.2 で Output タブに居る欄(借り元)に当たる物。★門が読む表。
OUTPUT_TAB_PANELS = (
    "FALCON_VSE_PT_format",          # RENDER_PT_format(Frame Rate を含む)
    "FALCON_VSE_PT_frame_range",     # RENDER_PT_frame_range
    "FALCON_VSE_PT_output",          # RENDER_PT_output
    "FALCON_VSE_PT_encoding",        # RENDER_PT_encoding
    "FALCON_VSE_PT_encoding_video",  # RENDER_PT_encoding_video
    "FALCON_VSE_PT_encoding_audio",  # RENDER_PT_encoding_audio
    "FALCON_VSE_PT_metadata",        # RENDER_PT_stamp
    "FALCON_VSE_PT_metadata_note",   # RENDER_PT_stamp_note
    "FALCON_VSE_PT_metadata_burn",   # RENDER_PT_stamp_burn
)

# VSE の Format で出さない項目(③)。
HIDDEN_FORMAT_PROPS = frozenset({"use_border", "use_crop_to_border"})

# 登録の時に決めた置き場(知らせの 1 行を出すかに使う)。
_output_tab_at_register = True


def _env_on(name):
    return os.environ.get(name, "1").strip().lower() not in ("", "0", "off", "false", "no")


def output_tab_enabled():
    return _env_on(ENV_OUTPUT_TAB)


def hide_region_enabled():
    return _env_on(ENV_HIDE_REGION)


# -----------------------------------------------------------------------------
# ③ 項目を落とす UILayout の包み
# -----------------------------------------------------------------------------

# 子の layout を返すので包み直す物。
_SUB_LAYOUTS = frozenset({"column", "row", "box", "split", "column_flow", "grid_flow"})


class FilteredLayout:
    """UILayout の代わり。`prop()` のうち `skip` の項目だけを出さない。他は本物へそのまま渡す。"""

    __slots__ = ("_layout", "_skip")

    def __init__(self, layout, skip):
        object.__setattr__(self, "_layout", layout)
        object.__setattr__(self, "_skip", skip)

    def __getattr__(self, name):
        attr = getattr(self._layout, name)
        if name in _SUB_LAYOUTS:
            skip = self._skip

            def sub(*args, **kwargs):
                return FilteredLayout(attr(*args, **kwargs), skip)
            return sub
        if name == "prop":
            skip = self._skip

            def prop(data, property, *args, **kwargs):  # noqa: A002  UILayout.prop と同じ名前
                if property in skip:
                    return None
                return attr(data, property, *args, **kwargs)
            return prop
        return attr

    def __setattr__(self, name, value):
        setattr(self._layout, name, value)


class _FormatSelf:
    """`RENDER_PT_format.draw` に渡す `self`(使うのは `layout` と `draw_framerate` だけ)。"""

    def __init__(self, layout):
        self.layout = layout

    @staticmethod
    def draw_framerate(layout, rd):
        from bl_ui.properties_output import RENDER_PT_format
        RENDER_PT_format.draw_framerate(layout, rd)


def draw_format(panel, context):
    """VSE の Format。本家の描き方から Render Region / Crop to Render Region を落とす。"""
    from bl_ui.properties_output import RENDER_PT_format
    if not hide_region_enabled():
        RENDER_PT_format.draw(panel, context)
        return
    RENDER_PT_format.draw(_FormatSelf(FilteredLayout(panel.layout, HIDDEN_FORMAT_PROPS)), context)


def _poll_output_moved(cls, context):
    """知らせの 1 行: 出力の欄を Render タブへまとめている時(`FALCON_VSE_OUTPUT_TAB=0`)だけ。"""
    return (not _output_tab_at_register) and context.engine in cls.COMPAT_ENGINES


# -----------------------------------------------------------------------------
# 登録の前に置き場を決める
# -----------------------------------------------------------------------------

def prepare():
    """`register()` の先頭(クラスを登録する前)に呼ぶ。★`bl_context` は登録の時にしか効かない。"""
    global _output_tab_at_register
    import sys
    module = sys.modules[__package__]
    _output_tab_at_register = output_tab_enabled()
    context = "output" if _output_tab_at_register else "render"
    for name in OUTPUT_TAB_PANELS:
        getattr(module, name).bl_context = context
    module.FALCON_VSE_PT_output_moved.poll = classmethod(_poll_output_moved)
    module.FALCON_VSE_PT_format.draw = draw_format
