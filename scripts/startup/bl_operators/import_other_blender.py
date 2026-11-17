# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Falcon: import add-ons, extensions and preferences from other Blender installations on this computer.
#
# Falcon keeps its configuration in its own folder (the launcher sets `BLENDER_USER_RESOURCES`), so the
# stock "Import Blender X.Y Preferences" (`preferences.copy_prev`) never finds anything. This operator looks
# in the standard per-version configuration folders of official Blender (and any folder the user types),
# lets the user pick what to bring over, and copies it into Falcon's own configuration folder.
# Whatever it overwrites is first copied to `<Falcon config>/import_backup/<time>/`.

__all__ = (
    "PREFERENCES_OT_import_other_blender",
    "find_other_blender_configs",
)

import os
import re
import sys

import bpy
from bpy.types import (
    Operator,
    PropertyGroup,
    UIList,
)
from bpy.props import (
    BoolProperty,
    CollectionProperty,
    EnumProperty,
    IntProperty,
    StringProperty,
)

# Settings that can be imported: (identifier, label, files or folders relative to the source root, selected by default).
_SETTINGS = (
    ("USERPREF", "Preferences (theme, keymap, input, paths…)", ("config/userpref.blend",), True),
    ("PRESETS", "Presets (keymaps, themes, operators…)", ("scripts/presets",), True),
    ("HISTORY", "Recent files and bookmarks", ("config/recent-files.txt", "config/bookmarks.txt"), True),
    ("STARTUP", "Startup file (default scene and layout)", ("config/startup.blend",), False),
)

# Names that usually mean an add-on was switched off or kept as a copy by hand.
_UNSELECTED_NAME = re.compile(r"(disabled|\.bak|bak[-_]\d|backup|__MACOSX)", re.IGNORECASE)

# Keeps the strings of the dynamic source list alive (Blender only borrows them).
_source_items_cache = []


def _config_bases():
    """The folders that hold official Blender's per-version configuration folders."""
    home = os.path.expanduser("~")
    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA") or os.path.join(home, "AppData", "Roaming")
        return [os.path.join(appdata, "Blender Foundation", "Blender")]
    if sys.platform == "darwin":
        return [os.path.join(home, "Library", "Application Support", "Blender")]
    xdg = os.environ.get("XDG_CONFIG_HOME") or os.path.join(home, ".config")
    return [os.path.join(xdg, "blender")]


def _own_config_root():
    return os.path.normpath(bpy.utils.resource_path('USER'))


def _looks_like_config(path):
    return any(os.path.exists(os.path.join(path, sub)) for sub in ("config", "scripts", "extensions"))


def find_other_blender_configs():
    """Return ``[(path, label), ...]`` of other Blender configuration folders, newest version first."""
    own = _own_config_root()
    found = []
    for base in _config_bases():
        try:
            names = os.listdir(base)
        except OSError:
            continue
        for name in names:
            match = re.fullmatch(r"(\d+)\.(\d+)", name)
            path = os.path.normpath(os.path.join(base, name))
            if not match or path == own or not _looks_like_config(path):
                continue
            found.append(((int(match.group(1)), int(match.group(2))), path, "Blender " + name))
    found.sort(reverse=True)
    return [(path, label) for _version, path, label in found]


def _source_items(_self, _context):
    items = [(path, label, path) for path, label in find_other_blender_configs()]
    items.append(('CUSTOM', "Other Folder", "Type the path of a Blender configuration folder"))
    _source_items_cache[:] = items
    return items


def _addon_candidates(root):
    """Legacy add-ons in ``scripts/addons``: ``[(name, path, is_link)]``."""
    addons_dir = os.path.join(root, "scripts", "addons")
    result = []
    try:
        names = sorted(os.listdir(addons_dir), key=str.lower)
    except OSError:
        return result
    for name in names:
        path = os.path.join(addons_dir, name)
        if name.startswith((".", "__")):
            continue
        # Python cannot import a name with a dot in it (e.g. "MyAddon.bak"), so Blender could never load it.
        if "." in (name[:-3] if name.endswith(".py") else name):
            continue
        if os.path.isdir(path):
            if not os.path.isfile(os.path.join(path, "__init__.py")):
                continue
        elif not name.endswith(".py"):
            continue
        result.append((name, path, os.path.islink(path)))
    return result


def _extension_candidates(root):
    """Extensions in ``extensions/<repository>/<package>``: ``[(repository, package, path, is_link)]``."""
    ext_dir = os.path.join(root, "extensions")
    result = []
    try:
        repos = sorted(os.listdir(ext_dir))
    except OSError:
        return result
    for repo in repos:
        repo_dir = os.path.join(ext_dir, repo)
        if repo.startswith(".") or not os.path.isdir(repo_dir):
            continue
        for pkg in sorted(os.listdir(repo_dir), key=str.lower):
            path = os.path.join(repo_dir, pkg)
            if pkg.startswith(".") or not os.path.isfile(os.path.join(path, "blender_manifest.toml")):
                continue
            result.append((repo, pkg, path, os.path.islink(path)))
    return result


def _source_root(props):
    if props.source == 'CUSTOM':
        return os.path.normpath(bpy.path.abspath(props.custom_path)) if props.custom_path else ""
    return props.source


def _fill_items(props, _context=None):
    props.items.clear()
    root = _source_root(props)
    props.filled_root = root
    if not root or not os.path.isdir(root):
        return
    for ident, label, rel_paths, selected in _SETTINGS:
        if not any(os.path.exists(os.path.join(root, rel)) for rel in rel_paths):
            continue
        item = props.items.add()
        item.kind = 'SETTING'
        item.name = label
        item.ident = ident
        item.selected = selected
    for name, path, is_link in _addon_candidates(root):
        item = props.items.add()
        item.kind = 'ADDON'
        item.name = name[:-3] if name.endswith(".py") else name
        item.ident = name
        item.path = path
        item.is_link = is_link
        item.selected = not _UNSELECTED_NAME.search(name)
    for repo, pkg, path, is_link in _extension_candidates(root):
        item = props.items.add()
        item.kind = 'EXTENSION'
        item.name = pkg
        item.ident = repo
        item.path = path
        item.is_link = is_link
        item.selected = not _UNSELECTED_NAME.search(pkg)


def _select_all_update(props, _context):
    for item in props.items:
        if item.kind != 'SETTING':
            item.selected = props.select_all_addons


class PREFERENCES_PG_import_other_item(PropertyGroup):
    __slots__ = ()

    kind: EnumProperty(
        items=(
            ('SETTING', "Setting", ""),
            ('ADDON', "Add-on", ""),
            ('EXTENSION', "Extension", ""),
        ),
    )
    ident: StringProperty()
    path: StringProperty()
    is_link: BoolProperty()
    selected: BoolProperty(name="Import")


class PREFERENCES_UL_import_other(UIList):
    def draw_item(self, _context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        row = layout.row(align=True)
        icon = {'SETTING': 'PREFERENCES', 'ADDON': 'PLUGIN', 'EXTENSION': 'PACKAGE'}[item.kind]
        row.prop(item, "selected", text="")
        text = item.name
        if item.kind == 'EXTENSION':
            text = "{:s}  ({:s})".format(item.name, item.ident)
        row.label(text=text, icon=icon, translate=False)
        if item.is_link:
            row.label(text="", icon='LINKED')


class PREFERENCES_OT_import_other_blender(Operator):
    """Import add-ons, extensions and preferences from another Blender installed on this computer"""
    bl_idname = "preferences.import_other_blender"
    bl_label = "Import From Other Blender"
    bl_options = {'REGISTER', 'INTERNAL'}

    source: EnumProperty(
        name="From",
        description="Configuration folder of the other Blender",
        items=_source_items,
    )
    custom_path: StringProperty(
        name="Folder",
        description="A Blender configuration folder (the one holding \"config\", \"scripts\" or \"extensions\")",
        subtype='DIR_PATH',
    )
    # The list is filled from the chosen folder when the folder changes (in `draw`), not from an update
    # callback: a script passing both `source` and `items` must keep its own `items`.
    filled_root: StringProperty(options={'HIDDEN', 'SKIP_SAVE'})
    items: CollectionProperty(type=PREFERENCES_PG_import_other_item)
    active_index: IntProperty()
    select_all_addons: BoolProperty(
        name="All Add-ons",
        description="Select or clear every add-on and extension",
        default=True,
        update=_select_all_update,
    )

    def invoke(self, context, _event):
        _fill_items(self)
        return context.window_manager.invoke_props_dialog(
            self, width=520, title="Import From Other Blender", confirm_text="Import",
        )

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.prop(self, "source")
        if self.source == 'CUSTOM':
            layout.prop(self, "custom_path")
        if self.filled_root != _source_root(self):
            _fill_items(self)

        if not self.items:
            layout.label(text="Nothing to import was found in this folder", icon='INFO')
            return

        root = _source_root(self)
        match = re.search(r"(\d+)\.(\d+)$", root)
        if match and (int(match.group(1)), int(match.group(2))) < bpy.app.version[:2]:
            layout.label(text="Add-ons made for older versions may not work here", icon='ERROR')

        row = layout.row()
        row.use_property_split = False
        row.prop(self, "select_all_addons")
        layout.template_list("PREFERENCES_UL_import_other", "", self, "items", self, "active_index", rows=14)
        col = layout.column(align=True)
        col.label(text="Files are copied. What they replace is kept in \"import_backup\".", icon='INFO')

    def execute(self, context):
        import addon_utils
        import shutil
        import time

        root = _source_root(self)
        if not root or not os.path.isdir(root):
            self.report({'ERROR'}, "Folder not found: {:s}".format(root))
            return {'CANCELLED'}
        if not self.items and not self.filled_root:
            # Called from a script without a list: take the defaults.
            _fill_items(self)

        own = _own_config_root()
        backup_root = os.path.join(own, "import_backup", time.strftime("%Y%m%d-%H%M%S"))
        backed_up = []

        def keep_old(dst):
            if not os.path.lexists(dst):
                return
            rel = os.path.relpath(dst, own)
            keep = os.path.join(backup_root, rel)
            os.makedirs(os.path.dirname(keep), exist_ok=True)
            if os.path.isdir(dst) and not os.path.islink(dst):
                shutil.copytree(dst, keep, symlinks=True)
            else:
                shutil.copy2(dst, keep, follow_symlinks=False)
            backed_up.append(rel)

        def copy_any(src, dst, replace):
            """Copy following links (the user asked for copies, not links)."""
            keep_old(dst)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.isdir(src):
                if replace and os.path.lexists(dst):
                    if os.path.isdir(dst) and not os.path.islink(dst):
                        shutil.rmtree(dst)
                    else:
                        os.remove(dst)
                shutil.copytree(
                    src, dst, symlinks=False, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
                )
            else:
                if os.path.islink(dst):
                    os.remove(dst)
                shutil.copy2(src, dst)

        chosen = [item for item in self.items if item.selected]
        settings = {item.ident for item in chosen if item.kind == 'SETTING'}
        errors = []

        # Add-ons that are on now (Falcon's own, e.g. Cycles-expansion) stay on, even when the imported
        # preferences did not know them.
        enabled_before = [addon.module for addon in context.preferences.addons]

        # 1. Settings. Preferences go first: they decide which extension repositories exist.
        for ident, _label, rel_paths, _default in _SETTINGS:
            if ident not in settings:
                continue
            for rel in rel_paths:
                src = os.path.join(root, rel)
                if os.path.exists(src):
                    # Presets merge into what is there; single files replace.
                    copy_any(src, os.path.join(own, rel), replace=False)

        if "USERPREF" in settings:
            bpy.ops.wm.read_userpref()
        if "HISTORY" in settings:
            bpy.ops.wm.read_history()
        if "PRESETS" in settings:
            bpy.ops.wm.operator_presets_cleanup()

        prefs = context.preferences

        # 2. Legacy add-ons.
        to_enable = []
        addons_dst = bpy.utils.user_resource('SCRIPTS', path="addons", create=True)
        for item in chosen:
            if item.kind != 'ADDON':
                continue
            try:
                copy_any(item.path, os.path.join(addons_dst, item.ident), replace=True)
                to_enable.append(item.name)
            except OSError as ex:
                errors.append("{:s}: {:s}".format(item.name, str(ex)))

        # 3. Extensions: into the repository of the same name if Falcon has it, otherwise the local one.
        repos = {repo.module: repo for repo in prefs.extensions.repos if repo.enabled}
        for item in chosen:
            if item.kind != 'EXTENSION':
                continue
            repo = repos.get(item.ident) or repos.get("user_default")
            if repo is None:
                errors.append("{:s}: no local extension repository".format(item.name))
                continue
            try:
                copy_any(item.path, os.path.join(repo.directory, item.name), replace=True)
            except OSError as ex:
                errors.append("{:s}: {:s}".format(item.name, str(ex)))
                continue
            if os.path.isfile(os.path.join(item.path, "__init__.py")):
                to_enable.append("bl_ext.{:s}.{:s}".format(repo.module, item.name))

        # 4. Turn the add-ons on and drop entries for add-ons that are not here (e.g. official Cycles).
        # The user add-on folder may not have existed at start-up, so it is not on `sys.path` yet.
        bpy.utils.refresh_script_paths()
        addon_utils.modules_refresh()
        if any(item.kind == 'EXTENSION' for item in chosen):
            try:
                addon_utils.extensions_refresh(ensure_wheels=True)
            except Exception as ex:
                errors.append("extensions: {:s}".format(str(ex)))

        def on_error(ex):
            errors.append(str(ex))

        for module in enabled_before:
            if module not in prefs.addons:
                addon_utils.enable(module, default_set=True, handle_error=on_error)
        for module in to_enable:
            addon_utils.enable(module, default_set=True, handle_error=on_error)

        known = {mod.__name__ for mod in addon_utils.modules(refresh=False)}
        repo_dirs = {repo.module: repo.directory for repo in prefs.extensions.repos}
        for addon in list(prefs.addons):
            module = addon.module
            if module.startswith("bl_ext."):
                parts = module.split(".", 2)  # "bl_ext", repository, package
                repo_dir = repo_dirs.get(parts[1]) if len(parts) == 3 else None
                missing = not (repo_dir and os.path.isdir(os.path.join(repo_dir, parts[2])))
            else:
                missing = module not in known
            if missing:
                addon_utils.disable(module, default_set=True)

        bpy.ops.wm.save_userpref()

        # Keep the user's work if a file is open; otherwise show the imported startup file and theme.
        if bpy.data.is_saved is bpy.data.is_dirty is False and "STARTUP" in settings:
            bpy.ops.wm.read_homefile()

        message = "Imported {:d} item(s) from {:s}".format(len(chosen), root)
        if backed_up:
            message += "; replaced files kept in {:s}".format(backup_root)
        if errors:
            print("Import From Other Blender:\n  " + "\n  ".join(errors))
            self.report({'WARNING'}, message + "; {:d} problem(s), see the console".format(len(errors)))
        else:
            self.report({'INFO'}, message)
        return {'FINISHED'}


def has_other_blender_configs():
    return bool(find_other_blender_configs())


classes = (
    PREFERENCES_PG_import_other_item,
    PREFERENCES_UL_import_other,
    PREFERENCES_OT_import_other_blender,
)
