# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Switch the interface language between English and a second language in one click.

Two buttons at the right end of the Top Bar ("EN" and the second language, "JA" by default) and
one shortcut, Ctrl+Shift+L, which toggles between the two. The second language is the language
last chosen in this session, else the system language (LANG / LC_ALL / LC_MESSAGES) when Blender
has it, else Japanese. Switching to a translated language also turns on the translation of the
interface, tooltips and reports (Preferences > Interface > Translation), which is what makes the
switch visible; new data names are left alone.

The preference is changed like any other one, so it is saved with the preferences as usual.
Written for Falcon Engine; the idea (a one-click language toggle) is the same as the language
switch add-ons on extensions.blender.org, no code is taken from them.
"""

bl_info = {
    "name": "Language Switch",
    "author": "Falcon Engine",
    "version": (1, 0, 0),
    "blender": (5, 2, 0),
    "location": "Top Bar (right end), Ctrl+Shift+L",
    "description": "Switch the interface language between English and a second language",
    "category": "Interface",
}

import os

import bpy
from bpy.app.translations import pgettext_rpt as rpt_
from bpy.app.translations import pgettext_tip as tip_
from bpy.props import StringProperty

ENGLISH = 'en_US'
FALLBACK_SECOND = 'ja_JP'
SHORTCUT = dict(type='L', value='PRESS', ctrl=True, shift=True)

# The non-English language chosen last in this session (None until one is chosen).
_last_second = None
_keymaps = []


def _languages():
    """{identifier: name} of the languages this build offers."""
    try:
        prop = bpy.context.preferences.view.bl_rna.properties["language"]
        return {item.identifier: item.name for item in prop.enum_items}
    except Exception:
        return {}


def _is_english(language):
    return (not language) or language.startswith("en") or language in {'C', 'POSIX'}


def current_language(context=None):
    """The language in effect (resolves "Automatic" to what Blender uses)."""
    context = context or bpy.context
    language = context.preferences.view.language
    if language == 'DEFAULT':
        language = bpy.app.translations.locale or ENGLISH
    return language


def _system_language(available):
    for var in ("LC_ALL", "LC_MESSAGES", "LANG"):
        value = os.environ.get(var, "")
        code = value.split(".")[0].split("@")[0]
        if not code or _is_english(code):
            continue
        if code in available:
            return code
        base = code.split("_")[0]
        for identifier in available:
            if identifier == base or identifier.split("_")[0] == base:
                return identifier
    return None


def second_language(context=None):
    available = _languages()
    current = current_language(context)
    if not _is_english(current) and current in available:
        return current
    if _last_second and _last_second in available:
        return _last_second
    system = _system_language(available)
    if system:
        return system
    return FALLBACK_SECOND


def short_code(language):
    """"ja_JP" -> "JA", "zh_HANS" -> "ZH", "pt_BR" -> "PT"."""
    return (language or "?").split("_")[0].split("@")[0].upper()


def set_language(context, language):
    global _last_second
    view = context.preferences.view
    if not _is_english(language):
        # Otherwise the switch would change nothing on screen.
        view.use_translate_interface = True
        view.use_translate_tooltips = True
        view.use_translate_reports = True
        _last_second = language
    else:
        current = current_language(context)
        if not _is_english(current):
            _last_second = current
    view.language = language


class WM_OT_falcon_language_set(bpy.types.Operator):
    """Set the interface language"""
    bl_idname = "wm.falcon_language_set"
    bl_label = "Set Interface Language"
    bl_options = {'INTERNAL'}

    language: StringProperty(
        name="Language",
        description="Language identifier, e.g. en_US or ja_JP",
        options={'SKIP_SAVE'},
    )

    @classmethod
    def description(cls, context, properties):
        name = _languages().get(properties.language, properties.language)
        return tip_("Switch the interface to %s") % name

    def execute(self, context):
        if self.language not in _languages():
            self.report({'WARNING'}, rpt_("Language not available: %s") % self.language)
            return {'CANCELLED'}
        set_language(context, self.language)
        return {'FINISHED'}


class WM_OT_falcon_language_toggle(bpy.types.Operator):
    """Switch the interface language between English and the second language"""
    bl_idname = "wm.falcon_language_toggle"
    bl_label = "Toggle Interface Language"

    def execute(self, context):
        if _is_english(current_language(context)):
            target = second_language(context)
        else:
            target = ENGLISH
        if target not in _languages():
            self.report({'WARNING'}, rpt_("Language not available: %s") % target)
            return {'CANCELLED'}
        set_language(context, target)
        return {'FINISHED'}


def draw_topbar(self, context):
    region = getattr(context, "region", None)
    if region is None or region.alignment != 'RIGHT':
        return
    english = _is_english(current_language(context))
    second = second_language(context)
    row = self.layout.row(align=True)
    props = row.operator("wm.falcon_language_set", text="EN", translate=False, depress=english)
    props.language = ENGLISH
    props = row.operator("wm.falcon_language_set", text=short_code(second), translate=False,
                         depress=not english)
    props.language = second


classes = (
    WM_OT_falcon_language_set,
    WM_OT_falcon_language_toggle,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_HT_upper_bar.append(draw_topbar)
    keyconfig = bpy.context.window_manager.keyconfigs.addon
    if keyconfig is not None:  # None in background mode
        keymap = keyconfig.keymaps.new(name="Window", space_type='EMPTY')
        item = keymap.keymap_items.new(WM_OT_falcon_language_toggle.bl_idname, **SHORTCUT)
        _keymaps.append((keymap, item))


def unregister():
    for keymap, item in _keymaps:
        try:
            keymap.keymap_items.remove(item)
        except Exception:
            pass
    _keymaps.clear()
    bpy.types.TOPBAR_HT_upper_bar.remove(draw_topbar)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
