# SPDX-FileCopyrightText: 2026 Falcon Engine
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Render Properties > "Source Media": what the selected strip's file is.

Shown while the render engine is VSE. Blender's own data comes first (the strip's elements,
its movie frame rate, its color space); what Blender does not keep (codec, bit depth, whether
a movie has alpha) comes from `ffprobe` when it is on the PATH and is left out otherwise.
Results are cached per file (path, size, modification time), so drawing the panel does not
touch the disk again.
"""

import json
import os
import re
import shutil
import subprocess

import bpy
from bpy.app.translations import pgettext_iface as iface_

# (path, size, mtime) -> dict
_probe_cache = {}
_image_cache = {}
_size_cache = {}


def _abspath(path, strip):
    try:
        return os.path.normpath(bpy.path.abspath(path, library=strip.id_data.library))
    except Exception:  # noqa: BLE001
        return os.path.normpath(path)


def _stat_key(path):
    try:
        st = os.stat(path)
    except OSError:
        return None
    return (path, st.st_size, st.st_mtime_ns)


def ffprobe_path():
    return shutil.which("ffprobe")


def _bit_depth(stream):
    """Bits per component: ffprobe's bits_per_raw_sample, else read off the pixel format."""
    bits = str(stream.get("bits_per_raw_sample") or "")
    if bits.isdigit() and int(bits) > 0:
        return int(bits)
    pix_fmt = stream.get("pix_fmt") or ""
    if not pix_fmt:
        return None
    match = re.search(r"p(\d+)(le|be)?$", pix_fmt)            # yuv420p10le, gbrap12le
    if match:
        return int(match.group(1))
    if re.search(r"(rgb|bgr)48|(rgba|bgra)64|gray16|ya16", pix_fmt):
        return 16
    match = re.search(r"gray(\d+)", pix_fmt)
    if match:
        return int(match.group(1))
    if pix_fmt.startswith(("p010",)):
        return 10
    if pix_fmt.startswith(("p016",)):
        return 16
    return 8


def _has_alpha(pix_fmt):
    pix_fmt = pix_fmt or ""
    return pix_fmt.startswith(("yuva", "rgba", "bgra", "argb", "abgr", "ya8", "ya16", "gbrap"))


def probe(path):
    """ffprobe the first video stream. {} when ffprobe is missing or fails."""
    key = _stat_key(path)
    if key is None:
        return {}
    if key in _probe_cache:
        return _probe_cache[key]
    info = {}
    exe = ffprobe_path()
    if exe:
        try:
            out = subprocess.run(
                [exe, "-v", "error", "-select_streams", "v:0", "-show_entries",
                 "stream=codec_name,pix_fmt,bits_per_raw_sample,width,height,avg_frame_rate,"
                 "r_frame_rate,nb_frames,duration:format=duration",
                 "-of", "json", path],
                capture_output=True, timeout=10, check=False).stdout
            data = json.loads(out or b"{}")
            stream = (data.get("streams") or [{}])[0]
            info = {
                "codec": stream.get("codec_name"),
                "pix_fmt": stream.get("pix_fmt"),
                "bit_depth": _bit_depth(stream),
                "alpha": _has_alpha(stream.get("pix_fmt")),
                "width": stream.get("width"),
                "height": stream.get("height"),
                "nb_frames": int(stream["nb_frames"]) if str(stream.get("nb_frames", "")).isdigit()
                else None,
            }
            rate = stream.get("avg_frame_rate") or stream.get("r_frame_rate") or ""
            if "/" in rate:
                num, den = rate.split("/", 1)
                if float(den or 0):
                    info["fps"] = float(num) / float(den)
            duration = stream.get("duration") or (data.get("format") or {}).get("duration")
            if duration not in (None, "N/A"):
                info["duration"] = float(duration)
        except Exception as ex:  # noqa: BLE001  a bad file must not break drawing
            print("falcon_vse_bridge: ffprobe failed for %s: %s" % (path, ex))
            info = {}
    _probe_cache[key] = info
    return info


def _image_header(path):
    """(width, height, has_alpha) of an image file, read by Blender's image loader."""
    key = _stat_key(path)
    if key is None:
        return None
    if key in _image_cache:
        return _image_cache[key]
    result = None
    try:
        import imbuf
        ibuf = imbuf.load(path)
        try:
            result = (int(ibuf.size[0]), int(ibuf.size[1]), int(ibuf.planes) == 32)
        finally:
            ibuf.free()
    except Exception as ex:  # noqa: BLE001
        print("falcon_vse_bridge: could not read %s: %s" % (path, ex))
    _image_cache[key] = result
    return result


def _total_size(paths):
    key = tuple(_stat_key(p) for p in paths)
    if key in _size_cache:
        return _size_cache[key]
    total = 0
    for k in key:
        if k is not None:
            total += k[1]
    _size_cache[key] = total
    return total


def _format_size(size):
    for unit, scale in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if size >= scale:
            return "%.1f %s" % (size / scale, unit)
    return "%d B" % size


def _fps_text(fps):
    return ("%.3f" % fps).rstrip("0").rstrip(".") + " fps"


def strip_files(strip):
    """The source files of a movie or image strip (absolute paths)."""
    if strip.type == 'MOVIE':
        return [_abspath(strip.filepath, strip)]
    if strip.type == 'IMAGE':
        directory = _abspath(strip.directory, strip)
        return [os.path.join(directory, element.filename) for element in strip.elements]
    return []


def media_info(strip, scene):
    """Plain values for a strip (numbers are not formatted). None when it has no source file."""
    files = strip_files(strip)
    if not files:
        return None
    render = scene.render
    project_fps = render.fps / (render.fps_base or 1.0)
    info = {
        "kind": 'MOVIE' if strip.type == 'MOVIE' else ('SEQUENCE' if len(files) > 1 else 'IMAGE'),
        "path": files[0],
        "count": len(files),
        "project_fps": project_fps,
        "colorspace": strip.colorspace_settings.name,
        "size": _total_size(files),
        "exists": os.path.isfile(files[0]),
    }
    element = strip.elements[0] if len(strip.elements) else None
    width = element.orig_width if element is not None else 0
    height = element.orig_height if element is not None else 0
    probed = probe(files[0]) if info["exists"] else {}
    if strip.type == 'MOVIE':
        fps = float(getattr(strip, "fps", 0.0) or 0.0) or probed.get("fps") or 0.0
        # ffprobe counts the file's own frames. Without it, the strip's length in the
        # timeline (which Blender may have scaled to the project frame rate) is shown.
        frames = int(getattr(strip, "content_duration", 0) or 0)
        if probed.get("nb_frames"):
            frames = probed["nb_frames"]
        info["fps"] = fps or None
        info["frames"] = frames or None
        info["seconds"] = (frames / fps) if (frames and fps) else probed.get("duration")
        if not (width and height):
            width, height = probed.get("width") or 0, probed.get("height") or 0
        info["alpha"] = probed.get("alpha") if probed else None
    else:
        info["fps"] = None
        info["frames"] = len(files)
        info["seconds"] = len(files) / project_fps if len(files) > 1 and project_fps else None
        header = _image_header(files[0]) if info["exists"] else None
        if header is not None:
            if not (width and height):
                width, height = header[0], header[1]
            info["alpha"] = header[2]
        else:
            info["alpha"] = probed.get("alpha") if probed else None
    info["width"], info["height"] = (width or None), (height or None)
    info["codec"] = probed.get("codec")
    info["bit_depth"] = probed.get("bit_depth")
    info["ffprobe"] = bool(ffprobe_path())
    return info


_KIND_LABEL = {'MOVIE': "Movie", 'SEQUENCE': "Image Sequence", 'IMAGE': "Image"}


def media_rows(strip, scene):
    """[(label msgid, value text, alert)] as drawn. Values are already translated."""
    info = media_info(strip, scene)
    if info is None:
        return None
    rows = [("Type", iface_(_KIND_LABEL[info["kind"]]), False),
            ("File", info["path"], not info["exists"])]
    if info["count"] > 1:
        rows.append(("File Count", "%d" % info["count"], False))
    if info["width"] and info["height"]:
        rows.append(("Resolution", "%d x %d" % (info["width"], info["height"]), False))
    project_text = _fps_text(info["project_fps"])
    if info["fps"]:
        differs = abs(info["fps"] - info["project_fps"]) > 1e-3
        rows.append(("Frame Rate", _fps_text(info["fps"]), differs))
        rows.append(("Project Frame Rate", project_text, differs))
    else:
        rows.append(("Project Frame Rate", project_text, False))
    if info["frames"] and info["seconds"]:
        rows.append(("Length", iface_("%d frames, %.2f s") % (info["frames"], info["seconds"]), False))
    rows.append(("Color Space", info["colorspace"], False))
    if info["alpha"] is not None:
        rows.append(("Alpha", iface_("Present") if info["alpha"] else iface_("None"), False))
    if info["count"] > 1:
        rows.append(("File Size", iface_("%s (%d files)") % (_format_size(info["size"]), info["count"]),
                     False))
    else:
        rows.append(("File Size", _format_size(info["size"]), False))
    if info["codec"]:
        rows.append(("Codec", info["codec"], False))
    if info["bit_depth"]:
        rows.append(("Bit Depth", iface_("%d-bit") % info["bit_depth"], False))
    return rows


def active_strip(context):
    """The selected active strip of the scene the Sequencer shows, or None."""
    from . import sequencer_scene
    scene = sequencer_scene(context)
    editor = getattr(scene, "sequence_editor", None) if scene is not None else None
    strip = getattr(editor, "active_strip", None) if editor is not None else None
    if strip is None or not strip.select:
        return None, scene
    return strip, scene


class FALCON_VSE_PT_source_media(bpy.types.Panel):
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "render"
    bl_label = "Source Media"
    bl_order = 5
    COMPAT_ENGINES = {'FALCON_VSE'}

    @classmethod
    def poll(cls, context):
        return context.engine in cls.COMPAT_ENGINES

    def draw(self, context):
        layout = self.layout
        strip, scene = active_strip(context)
        if strip is None:
            row = layout.row()
            row.active = False
            row.label(text="Select a strip in the Sequencer", icon='INFO')
            return
        rows = media_rows(strip, scene)
        if rows is None:
            row = layout.row()
            row.active = False
            row.label(text="This strip has no source file", icon='INFO')
            return
        col = layout.column(align=True)
        for label, value, alert in rows:
            split = col.split(factor=0.4)
            left = split.row()
            left.alignment = 'RIGHT'
            left.alert = alert
            left.label(text=label)
            right = split.row()
            right.alert = alert
            right.label(text=value, translate=False)
        if any(alert for label, _value, alert in rows if label == "Frame Rate"):
            layout.label(text="Differs from the project frame rate", icon='ERROR')
        if not ffprobe_path():
            row = layout.row()
            row.active = False
            row.label(text="ffprobe not found: codec and bit depth are not shown", icon='INFO')


def register():
    bpy.utils.register_class(FALCON_VSE_PT_source_media)


def unregister():
    bpy.utils.unregister_class(FALCON_VSE_PT_source_media)
    _probe_cache.clear()
    _image_cache.clear()
    _size_cache.clear()
