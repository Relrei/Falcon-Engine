"""Optional linear-HDR LT denoise and screen-space artistic light spill."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import numpy as np

_cache = None


def denoise(layer):
    """OIDN RT on unsplatted HDR light, before gain/spill. One-result RAM cache."""
    global _cache
    a = np.ascontiguousarray(layer, dtype='<f4')
    if not np.isfinite(a).all():
        raise ValueError('LT layer contains non-finite values')
    h, w, channels = a.shape
    assert channels == 3
    # Built-in OIDN (the one Cycles links) first; the `oidnDenoise` executable is only a
    # fallback for builds without the binding, so a normal install needs nothing on PATH.
    import _cycles
    builtin = getattr(_cycles, 'oidn_denoise_rgb', None)
    executable = None if builtin else shutil.which('oidnDenoise')
    if not builtin and not executable:
        raise RuntimeError('LT denoise needs OpenImageDenoise. Disable LT Denoise to continue.')
    digest = hashlib.sha256(a.tobytes()).hexdigest()
    key = (a.shape, digest, 'builtin' if builtin else (executable, os.stat(executable).st_mtime_ns))
    if _cache is not None and _cache[0] == key:
        return _cache[1]
    if not np.any(a):
        return a
    if builtin:
        out = np.frombuffer(builtin(a.tobytes(), w, h), dtype='<f4').reshape(h, w, 3).copy()
        if not np.isfinite(out).all():
            raise ValueError('OIDN output contains non-finite values')
        _cache = key, out
        return out
    with tempfile.TemporaryDirectory(prefix='falcon_lt_oidn_') as tmp:
        src, dst = Path(tmp) / 'input.pfm', Path(tmp) / 'output.pfm'
        try:
            with src.open('wb') as f:
                f.write(('PF\n%d %d\n-1.0\n' % (w, h)).encode('ascii'))
                a.tofile(f)
            result = subprocess.run(
                [executable, '-d', 'default', '--hdr', str(src), '-o', str(dst),
                 '--threads', '4', '--affinity', '0', '-n', '1'],
                capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise RuntimeError('LT OIDN failed: ' + (result.stderr + result.stdout)[-1500:])
            with dst.open('rb') as f:
                if f.readline().strip() != b'PF':
                    raise ValueError('Invalid OIDN output format')
                if tuple(map(int, f.readline().split())) != (w, h):
                    raise ValueError('OIDN output dimensions changed')
                scale = float(f.readline())
                out = np.fromfile(f, dtype='<f4' if scale < 0 else '>f4')
            out = (out.reshape(h, w, 3) * abs(scale)).astype(np.float32)
            if not np.isfinite(out).all():
                raise ValueError('OIDN output contains non-finite values')
            _cache = key, out
            return out
        finally:
            # Record generated temporary artifacts before TemporaryDirectory removes them.
            for p in (src, dst):
                if p.exists():
                    data = p.read_bytes()
                    print('[Falcon LT cleanup] %s bytes=%d sha256=%s' %
                          (p.name, len(data), hashlib.sha256(data).hexdigest()))


def _fft_length(n):
    """Smallest 2/3/5-smooth size >= n; zero padding preserves linear convolution."""
    best = 1
    while best < n:
        best *= 2
    two = 1
    while two < best:
        three = two
        while three < best:
            five = three
            while five < n:
                five *= 5
            best = min(best, five)
            three *= 3
        two *= 2
    return best


def spill(layer, radius, falloff):
    """Normalized circular kernel; returns only added light, zero outside frame.

    Radius is in output pixels. Wider kernels dilute the added energy. Falloff
    is the exponent of (1-distance/radius), not physical inverse-square falloff.
    This is a 2D look effect and has no depth/occlusion information.
    """
    radius = float(radius)
    if radius <= 0:
        return np.zeros_like(layer)
    r = int(np.ceil(radius))
    y, x = np.ogrid[-r:r + 1, -r:r + 1]
    k = np.maximum(1.0 - np.sqrt(x*x + y*y) / radius, 0.0) ** float(falloff)
    k /= k.sum()
    h, w = layer.shape[:2]
    shape = (_fft_length(h + 2*r), _fft_length(w + 2*r))
    spectrum = np.fft.rfft2(k, s=shape)
    out = np.empty_like(layer, dtype=np.float32)
    for c in range(3):
        full = np.fft.irfft2(np.fft.rfft2(layer[:, :, c], s=shape) * spectrum, s=shape)
        out[:, :, c] = np.maximum(full[r:r+h, r:r+w], 0.0)
    return out


def prepare(layer, settings):
    strength = settings.falcon_lt_denoise_strength
    if settings.falcon_lt_denoise and strength > 0:
        cleaned = denoise(layer)
        layer = (1.0 - strength) * layer + strength * cleaned
    if settings.falcon_lt_spill_radius > 0 and settings.falcon_lt_spill_strength > 0:
        layer = layer + settings.falcon_lt_spill_strength * spill(
            layer, settings.falcon_lt_spill_radius, settings.falcon_lt_spill_falloff)
    return layer
