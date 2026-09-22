# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: Apache-2.0

"""Falcon plugin folder: files placed in `falcon_plugins/<name>/` next to the Blender
executable (or in the user configuration folder) are checked against their manifest
`falcon_plugin.toml` and, when everything matches, connected automatically.

The only kind today is `dlss`: the NVIDIA DLSS Ray Reconstruction runtime, which is not
shipped with Blender. The loader passes the plugin folder to NGX as an extra search path
(`_cycles.set_falcon_plugin_paths`), so the runtime is found without copying it next to
the executable.

Search order (first one wins for the same id):
  1. `FALCON_PLUGINS_DIR` (':' separated, for testing)
  2. `<user config>/falcon_plugins/`     e.g. ~/.config/blender/5.2/falcon_plugins/
  3. `<blender executable dir>/falcon_plugins/`

`FALCON_PLUGINS=0` turns the loader off (nothing is scanned and no path is passed on,
the runtime is then only looked up next to the executable as before, and DLSS does not
need the add-on below).

DLSS is off by default. A valid runtime only makes the DLSS add-on (`falcon_dlss`, in
`addons_core`) appear in Preferences > Add-ons; the runtime folder is handed to NGX, DLSS
switched on in Cycles (`_cycles.set_dlss_enabled`) and offered as a denoiser, only while that
add-on is enabled. While Blender runs, the plugin folders are checked every few seconds, so
placing or removing the runtime is followed without a restart (`FALCON_PLUGINS_WATCH=0` turns
that off; "Rescan" still works).
"""

from __future__ import annotations

import os
import platform
import re
import sys

import bpy

MANIFEST = "falcon_plugin.toml"
FOLDER = "falcon_plugins"
SCHEMA_MAJOR = 1

# id -> file name rules. A plugin of this id may only contain these files (+ the manifest).
KINDS = {
    "dlss": {
        "label": "NVIDIA DLSS",
        # NGX looks for the Ray Reconstruction runtime under this name only (checked 2026-09-19 with
        # driver 615: `libnvidia-ngx-dldenoiser.so` is not picked up).
        "linux-x64": re.compile(r"^libnvidia-ngx-dlssd\.so(\.\d+)*$"),
        "windows-x64": re.compile(r"^nvngx_dlssd\.dll$", re.IGNORECASE),
    },
}

# The add-on that turns DLSS on (scripts/addons_core/falcon_dlss). Keep the name in sync with its bl_info.
DLSS_ADDON = "falcon_dlss"
DLSS_ADDON_NAME = "NVIDIA DLSS denoiser"
# Value of 'DLSS' in the Cycles denoiser enums (properties.py).
DENOISER_DLSS = 8

WATCH_INTERVAL = 2.0

# Last scan, read by the panel. `dlss_found`: valid DLSS runtime folders, whether or not the add-on is
# enabled. `connected`: the folders handed to Cycles.
_state = {"plugins": [], "loose": [], "places": [], "connected": [], "dlss_found": [], "note": ""}


def _enabled():
    return os.environ.get("FALCON_PLUGINS", "1") != "0"


def _with_dlss():
    try:
        import _cycles
    except ImportError:
        return False
    return bool(getattr(_cycles, "with_dlss", False))


def dlss_addon_enabled():
    """True while the DLSS add-on is registered: the switch that turns DLSS on."""
    return bool(getattr(sys.modules.get(DLSS_ADDON), "active", False))


def dlss_allowed():
    """Whether DLSS may be used: while the DLSS add-on is enabled. With the loader off
    (FALCON_PLUGINS=0) DLSS works as before the plugin folder existed: whenever NGX finds a runtime."""
    return (not _enabled()) or dlss_addon_enabled()


def dlss_addon_listed():
    """Whether Preferences > Add-ons lists the DLSS add-on (`addon_utils` also lists it while it is
    enabled): only in builds with DLSS, once a valid runtime is in a plugin folder."""
    return bool(_state["dlss_found"]) and _with_dlss()


def dlss_status():
    """'NO_BUILD' (built without DLSS), 'NO_LOADER' (FALCON_PLUGINS=0), 'OFF' (add-on disabled),
    'CONNECTED' or 'NOT_FOUND' (add-on enabled, no valid runtime in the plugin folders)."""
    if not _with_dlss():
        return 'NO_BUILD'
    if not _enabled():
        return 'NO_LOADER'
    if not dlss_addon_enabled():
        return 'OFF'
    return 'CONNECTED' if _state["connected"] else 'NOT_FOUND'


def current_platform():
    machine = platform.machine().lower()
    arch = "x64" if machine in {"x86_64", "amd64"} else ("arm64" if machine in {"aarch64", "arm64"} else machine)
    if sys.platform.startswith("linux"):
        return "linux-" + arch
    if sys.platform == "win32":
        return "windows-" + arch
    if sys.platform == "darwin":
        return "macos-" + arch
    return sys.platform + "-" + arch


def places():
    """(kind, path) in priority order. Paths may not exist."""
    out = []
    for p in os.environ.get("FALCON_PLUGINS_DIR", "").split(os.pathsep):
        if p:
            out.append(("env", os.path.abspath(p)))
    user = bpy.utils.resource_path('USER')
    if user:
        out.append(("user", os.path.join(user, FOLDER)))
    out.append(("bundle", os.path.join(os.path.dirname(bpy.app.binary_path), FOLDER)))
    return out


def _version_tuple(text):
    parts = []
    for p in str(text).split("."):
        if not p.isdigit():
            raise ValueError(text)
        parts.append(int(p))
    return tuple(parts + [0] * (3 - len(parts)))


def disabled_ids(context=None):
    try:
        prefs = (context or bpy.context).preferences.addons[__package__].preferences
        return {s for s in prefs.falcon_plugins_disabled.split(";") if s}
    except (AttributeError, KeyError):
        return set()


def check_plugin(folder, plat=None, blender_version=None):
    """Check one plugin folder. Returns a dict with `errors` (reasons not to connect) and
    `info` (not an error, e.g. another platform)."""
    import tomllib

    plat = plat or current_platform()
    blender_version = blender_version or tuple(bpy.app.version)
    rec = {"dir": folder, "name": os.path.basename(folder), "id": None, "version": "",
           "files": [], "errors": [], "info": []}
    manifest = os.path.join(folder, MANIFEST)
    if not os.path.isfile(manifest):
        rec["errors"].append("no manifest (%s)" % MANIFEST)
        return rec
    try:
        with open(manifest, "rb") as f:
            m = tomllib.load(f)
    except (OSError, tomllib.TOMLDecodeError) as ex:
        rec["errors"].append("manifest cannot be read: %s" % ex)
        return rec

    schema = str(m.get("schema_version", ""))
    if not schema.split(".")[0].isdigit() or int(schema.split(".")[0]) != SCHEMA_MAJOR:
        rec["errors"].append("unsupported schema_version %r" % schema)
        return rec
    rec["id"] = m.get("id")
    rec["name"] = str(m.get("name") or rec["name"])
    rec["version"] = str(m.get("version", ""))
    kind = KINDS.get(rec["id"])
    if kind is None:
        rec["errors"].append("unsupported id %r" % rec["id"])
        return rec

    platforms = m.get("platforms", [])
    if not isinstance(platforms, list) or not platforms:
        rec["errors"].append("platforms is missing")
        return rec
    if plat not in platforms:
        rec["info"].append("for %s (this is %s)" % (", ".join(map(str, platforms)), plat))
        return rec

    try:
        vmin = _version_tuple(m.get("blender_version_min", "0.0.0"))
        vmax = m.get("blender_version_max")
        vmax = _version_tuple(vmax) if vmax is not None else None
    except ValueError as ex:
        rec["errors"].append("bad version in manifest: %s" % ex)
        return rec
    bv = tuple(blender_version[:3])
    if bv < vmin or (vmax is not None and bv > vmax):
        rec["errors"].append("needs Blender %s%s (this is %s)" % (
            ".".join(map(str, vmin)), " to %s" % ".".join(map(str, vmax)) if vmax else " or newer",
            ".".join(map(str, bv))))

    files = m.get("files", [])
    if not isinstance(files, list) or not files:
        rec["errors"].append("files is missing")
        return rec
    rec["files"] = [str(x) for x in files]
    rule = kind.get(plat)
    for fn in rec["files"]:
        if os.path.basename(fn) != fn:
            rec["errors"].append("file must be directly in the folder: %s" % fn)
        elif rule is None or not rule.match(fn):
            rec["errors"].append("not a %s file: %s" % (rec["id"], fn))
        elif not os.path.isfile(os.path.join(folder, fn)):
            rec["errors"].append("missing: %s" % fn)
    listed = set(rec["files"]) | {MANIFEST}
    for fn in sorted(os.listdir(folder)):
        if fn not in listed:
            rec["errors"].append("unexpected file: %s" % fn)
    return rec


def scan(context=None):
    """Scan all places. Returns the state dict (also kept for the panel)."""
    plugins, loose, seen = [], [], {}
    plat = current_platform()
    off = disabled_ids(context)
    place_list = places()
    for where, root in place_list:
        if not os.path.isdir(root):
            continue
        for fn in sorted(os.listdir(root)):
            p = os.path.join(root, fn)
            if not os.path.isdir(p):
                loose.append({"where": where, "path": p, "error": "unexpected file: %s" % fn})
                continue
            if not os.listdir(p):
                # Empty placeholder folder (e.g. created up front by _ensure_folders): not an
                # error, just nothing dropped in yet. Treat it as if it did not exist.
                continue
            rec = check_plugin(p, plat)
            rec["where"] = where
            if rec["id"] and not rec["errors"] and not rec["info"]:
                if rec["id"] in seen:
                    rec["info"].append("not used: %s in %s wins" % (rec["id"], seen[rec["id"]]))
                else:
                    seen[rec["id"]] = where
                    rec["active"] = True
            # DLSS is turned on and off with its add-on, the other kinds in the Cycles preferences.
            rec["disabled"] = (not dlss_addon_enabled()) if rec["id"] == "dlss" else (rec["id"] in off)
            plugins.append(rec)
    dlss_found = [r["dir"] for r in plugins if r.get("active") and r["id"] == "dlss"]
    _state.update(plugins=plugins, loose=loose, places=place_list, dlss_found=dlss_found)
    return _state


def apply(state=None):
    """Hand the connected plugin folders to Cycles. Returns the folders passed on."""
    import _cycles

    state = state or _state
    dlss = [r["dir"] for r in state["plugins"]
            if r.get("active") and not r["disabled"] and r["id"] == "dlss"]
    note = ""
    if state["dlss_found"] and not getattr(_cycles, "with_dlss", False):
        note = "This build has no DLSS support (built without WITH_DLSS)."
        dlss = []
    # NGX keeps reporting a runtime it has loaded once, whatever paths it gets: switch DLSS itself too.
    if hasattr(_cycles, "set_dlss_enabled"):
        _cycles.set_dlss_enabled(dlss_allowed())
    if hasattr(_cycles, "set_falcon_plugin_paths"):
        _cycles.set_falcon_plugin_paths(dlss)
    state["connected"] = dlss
    state["note"] = note
    return dlss


def _report(state):
    for r in state["plugins"]:
        for e in r["errors"]:
            print("[Falcon plugins] ERROR %s: %s" % (r["dir"], e))
    for l in state["loose"]:
        print("[Falcon plugins] ERROR %s: %s" % (os.path.dirname(l["path"]), l["error"]))
    for d in state["connected"]:
        print("[Falcon plugins] connected: %s" % d)
    if state["dlss_found"] and not state["connected"] and not state["note"]:
        print("[Falcon plugins] DLSS runtime found, not used until the \"%s\" add-on is enabled: %s" % (
            DLSS_ADDON_NAME, state["dlss_found"][0]))
    if dlss_status() == 'NOT_FOUND':
        print("[Falcon plugins] DLSS runtime not found (the \"%s\" add-on is enabled)" % DLSS_ADDON_NAME)
    if state["note"]:
        print("[Falcon plugins] %s" % state["note"])


def rescan(context=None):
    if not _enabled():
        _state.update(plugins=[], loose=[], places=[], connected=[], dlss_found=[],
                      note="Plugin loading is off (FALCON_PLUGINS=0).")
        try:
            import _cycles
            if hasattr(_cycles, "set_dlss_enabled"):
                _cycles.set_dlss_enabled(True)
            if hasattr(_cycles, "set_falcon_plugin_paths"):
                _cycles.set_falcon_plugin_paths([])
        except ImportError:
            pass
        return _state
    state = scan(context)
    apply(state)
    _report(state)
    _watch_sig[0] = _signature()
    return state


def _stored_enum(group, prop):
    """Raw value of an enum property, also when it is not among the current items (DLSS after the
    add-on was disabled reads back as '')."""
    try:
        return group.get(prop)
    except (AttributeError, TypeError):
        return None


def dlss_fallback(reason):
    """Scenes that use DLSS go back to the default denoisers: OpenImageDenoise for renders and
    Automatic for the viewport. Returns [(scene name, [property, ...]), ...]."""
    changed = []
    scenes = getattr(bpy.data, "scenes", None)
    if scenes is None:  # `bpy.data` is restricted while add-ons register
        return changed
    for sc in scenes:
        cs = getattr(sc, "cycles", None)
        if cs is None:
            continue
        props = [p for p in ("denoiser", "preview_denoiser") if _stored_enum(cs, p) == DENOISER_DLSS]
        if not props:
            continue
        for p in props:
            cs.property_unset(p)
        for view_layer in sc.view_layers:
            try:
                view_layer.update_render_passes()
            except Exception:
                pass
        changed.append((sc.name, props))
        print("[Falcon plugins] %s: %s set back to the default, %s" % (sc.name, " and ".join(props), reason))
    return changed


def dlss_addon_changed(enabled):
    """Called by the DLSS add-on from its register() and unregister()."""
    if not enabled and _with_dlss() and not dlss_allowed():
        dlss_fallback("DLSS is turned off")
    try:
        rescan()
    except Exception as ex:
        print("[Falcon plugins] ERROR: scan failed: %r" % ex)
    _tag_redraw()


@bpy.app.handlers.persistent
def _load_post(*_args):
    # A file saved with DLSS, opened while DLSS is off: use the default denoisers instead of an empty
    # entry (Cycles would fall back to OpenImageDenoise on the CPU anyway).
    if _with_dlss() and not dlss_allowed():
        dlss_fallback("DLSS is off")


# ---------------------------------------------------------------- watch the plugin folders

_watch_sig = [None]


def _mtime(path):
    try:
        return os.stat(path).st_mtime_ns
    except OSError:
        return None


def _signature():
    """Cheap fingerprint of the plugin folders: what is in them and when each plugin folder and
    manifest last changed."""
    if not _enabled():
        return None
    sig = []
    for _where, root in places():
        try:
            names = sorted(os.listdir(root))
        except OSError:
            sig.append((root, None))
            continue
        sig.append((root, tuple((fn, _mtime(os.path.join(root, fn)), _mtime(os.path.join(root, fn, MANIFEST)))
                                for fn in names)))
    return tuple(sig)


def _tag_redraw():
    try:
        wm = bpy.context.window_manager
        for window in wm.windows:
            for area in window.screen.areas:
                area.tag_redraw()
    except (AttributeError, IndexError):  # no window manager yet, or restricted context
        pass


def _watch():
    try:
        sig = _signature()
        if sig != _watch_sig[0]:
            _watch_sig[0] = sig  # also when the scan below fails, so it is not retried every tick
            rescan()
            _tag_redraw()
    except Exception as ex:
        print("[Falcon plugins] ERROR: watch failed: %r" % ex)
    return WATCH_INTERVAL


def _watch_wanted():
    return (not bpy.app.background) and _enabled() and os.environ.get("FALCON_PLUGINS_WATCH", "1") != "0"


# ---------------------------------------------------------------- UI

class CYCLES_OT_falcon_plugins_rescan(bpy.types.Operator):
    """Look for plugins again and connect the ones that match"""
    bl_idname = "cycles.falcon_plugins_rescan"
    bl_label = "Rescan"

    def execute(self, context):
        rescan(context)
        return {'FINISHED'}


class CYCLES_OT_falcon_plugins_open_folder(bpy.types.Operator):
    """Open the plugin folder in the user configuration (create it if missing)"""
    bl_idname = "cycles.falcon_plugins_open_folder"
    bl_label = "Open Plugin Folder"

    def execute(self, context):
        user = bpy.utils.resource_path('USER')
        path = os.path.join(user, FOLDER)
        os.makedirs(path, exist_ok=True)
        bpy.ops.wm.path_open(filepath=path)
        return {'FINISHED'}


class CYCLES_OT_falcon_plugins_toggle(bpy.types.Operator):
    """Turn this plugin on or off"""
    bl_idname = "cycles.falcon_plugins_toggle"
    bl_label = "Toggle Plugin"
    bl_options = {'INTERNAL'}

    plugin_id: bpy.props.StringProperty()

    def execute(self, context):
        if self.plugin_id == "dlss":
            # DLSS is switched with its add-on, so this box and the one in Preferences > Add-ons agree.
            import addon_utils
            if dlss_addon_enabled():
                addon_utils.disable(DLSS_ADDON, default_set=True)
            else:
                addon_utils.enable(DLSS_ADDON, default_set=True)
            context.preferences.is_dirty = True
            return {'FINISHED'}
        prefs = context.preferences.addons[__package__].preferences
        off = {s for s in prefs.falcon_plugins_disabled.split(";") if s}
        off ^= {self.plugin_id}
        prefs.falcon_plugins_disabled = ";".join(sorted(off))
        context.preferences.is_dirty = True
        rescan(context)
        return {'FINISHED'}


def draw_plugins(layout, context):
    """Shared by the sidebar panel and the Cycles add-on preferences."""
    state = _state
    col = layout.column(align=True)
    if not _enabled():
        col.label(text="Plugin loading is off (FALCON_PLUGINS=0)", icon='INFO')
        return
    shown = False
    for r in state["plugins"]:
        shown = True
        row = col.row(align=True)
        if r["errors"]:
            row.alert = True
            row.label(text="%s: %s" % (r["name"], r["errors"][0]), icon='ERROR')
            for e in r["errors"][1:]:
                sub = col.row()
                sub.alert = True
                sub.label(text="    " + e)
            continue
        if r["info"]:
            row.label(text="%s: %s" % (r["name"], r["info"][0]), icon='INFO')
            continue
        icon = 'CHECKBOX_DEHLT' if r["disabled"] else 'CHECKBOX_HLT'
        op = row.operator("cycles.falcon_plugins_toggle", text="", icon=icon, emboss=False)
        op.plugin_id = r["id"]
        connected = r["dir"] in state["connected"]
        row.label(text=r["name"], icon='LINKED' if connected else 'UNLINKED')
        # Version and place on their own line, so a narrow sidebar does not cut them off.
        sub = col.row()
        sub.active = False
        sub.label(text="          %s%s" % (r["version"], " (user folder)" if r["where"] == "user" else ""))
    for l in state["loose"]:
        shown = True
        row = col.row()
        row.alert = True
        row.label(text="%s: %s" % (os.path.basename(os.path.dirname(l["path"])), l["error"]), icon='ERROR')
    if dlss_status() == 'NOT_FOUND':
        shown = True
        row = col.row()
        row.alert = True
        row.label(text="DLSS runtime not found", icon='ERROR')
    if not shown:
        col.label(text="No plugins found", icon='INFO')
    if state["note"]:
        col.label(text=state["note"], icon='INFO')
    row = layout.row(align=True)
    row.operator("cycles.falcon_plugins_rescan", icon='FILE_REFRESH')
    row.operator("cycles.falcon_plugins_open_folder", icon='FILE_FOLDER')


classes = (
    CYCLES_OT_falcon_plugins_rescan,
    CYCLES_OT_falcon_plugins_open_folder,
    CYCLES_OT_falcon_plugins_toggle,
)


def _ensure_folders():
    """Create `<user config>/falcon_plugins/<kind>/` up front for every known kind, so a
    first-time user finds an empty, correctly-named folder waiting instead of having to
    create the path themselves (only the user-config place, not the env/bundle ones)."""
    try:
        user = bpy.utils.resource_path('USER')
        if not user:
            return
        for kind in KINDS:
            os.makedirs(os.path.join(user, FOLDER, kind), exist_ok=True)
    except Exception as ex:  # never keep Cycles from registering
        print("[Falcon plugins] ERROR: could not create plugin folder: %r" % ex)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    _ensure_folders()
    try:
        rescan()
    except Exception as ex:  # never keep Cycles from registering
        print("[Falcon plugins] ERROR: scan failed: %r" % ex)
    if _load_post not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_load_post)
    if _watch_wanted() and not bpy.app.timers.is_registered(_watch):
        bpy.app.timers.register(_watch, first_interval=WATCH_INTERVAL, persistent=True)


def unregister():
    if bpy.app.timers.is_registered(_watch):
        bpy.app.timers.unregister(_watch)
    if _load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_load_post)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
