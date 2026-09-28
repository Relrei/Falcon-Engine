"""One cached LT frame, for fast look adjustment of saved passes only."""
import os
import numpy as np
import bpy
from bpy.app.handlers import persistent

_raw = None
_look = None


@persistent
def clear(*args):
    global _raw, _look
    _raw = _look = None


def register():
    if clear not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(clear)


def unregister():
    clear()
    if clear in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(clear)


def compose(manifest, settings, process):
    global _raw, _look
    w, h = manifest['w'], manifest['h']
    paths = [*manifest['passes'], manifest['beauty']]
    if not manifest['passes']:
        raise ValueError('No saved LT passes')
    files = []
    for path in paths:
        st = os.stat(path)
        files.append((os.path.realpath(path), st.st_size, st.st_mtime_ns, st.st_ctime_ns))
    key = (w, h, tuple(files))
    if _raw is None or _raw[0] != key:
        # Drop the old frame first; do not retain multiple scenes/resolutions.
        clear()
        def load(path):
            img = bpy.data.images.load(path, check_existing=False)
            try:
                if tuple(img.size) != (w, h):
                    raise ValueError('Saved LT dimensions do not match manifest')
                array = np.empty(w*h*4, dtype=np.float32)
                img.pixels.foreach_get(array)
                return array.reshape(h, w, 4)
            finally:
                bpy.data.images.remove(img)
        light = np.zeros((h, w, 3), dtype=np.float32)
        for path in manifest['passes']:
            light += load(path)[:, :, :3]
        beauty = load(manifest['beauty'])
        _raw = key, light, beauty
    look_key = (key, settings.falcon_lt_denoise, settings.falcon_lt_denoise_strength,
                settings.falcon_lt_spill_radius, settings.falcon_lt_spill_falloff,
                settings.falcon_lt_spill_strength, settings.falcon_lt_blur)
    if settings.falcon_lt_gain == 0:
        return _raw[2].copy()
    if _look is None or _look[0] != look_key:
        # process() excludes gain: changing gain can reuse expensive filtering.
        _look = look_key, process(_raw[1], settings)
    result = _raw[2].copy()
    result[:, :, :3] += settings.falcon_lt_gain * _look[1]
    return result


def show(context, pixels):
    h, w = pixels.shape[:2]
    name = 'Falcon LT Preview (Saved Frame)'
    image = bpy.data.images.get(name)
    if image is not None and tuple(image.size) != (w, h):
        bpy.data.images.remove(image)
        image = None
    if image is None:
        image = bpy.data.images.new(name, w, h, alpha=True, float_buffer=True)
    image.use_view_as_render = True
    image.pixels.foreach_set(np.ascontiguousarray(pixels).ravel())
    image.update()
    for window in context.window_manager.windows:
        if window.scene != context.scene:
            continue
        for area in window.screen.areas:
            if area.type == 'IMAGE_EDITOR':
                area.spaces.active.image = image
                area.tag_redraw()
    return image
