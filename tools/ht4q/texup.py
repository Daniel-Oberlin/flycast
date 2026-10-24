"""Upscale dumped Flycast textures with Real-ESRGAN (ncnn-vulkan build) for Flycast's custom textures.

    python3 -I texup.py ESRGAN_DIR IN_DIR OUT_DIR [--model realesrgan-x4plus] [--scale 4|2] [--nowrap]

IN_DIR: PNGs from Flycast's texture dump (config:rend.DumpTextures=yes), named by texture hash.
OUT_DIR: same names, upscaled - copy into <data>/textures/<game ID>/ to use them (config:rend.CustomTextures=yes).
Game-derived images: keep them outside the repo.

Per texture: pad each axis by wrapping if its opposite edges match (a tiling texture), else by
extending the edge (sky panels, signs; --nowrap forces that everywhere), fill fully
transparent pixels with nearby colours (no fringes), upscale RGB with the model; alpha, if any,
separately - hard 0/255 cut-outs stay hard.

Skipped: textures already in OUT_DIR (packs grow track by track), and textures the game redraws
every frame (the minimap: each frame is a new hash) - more than GENERATED of the same size with the
same (non-uniform) transparency mask.
"""
import collections, hashlib, os, subprocess, sys, tempfile
import numpy as np
from PIL import Image

PAD = 8
GENERATED = 20


def fill_transparent(rgba):
    """Colour of transparent pixels <- average of opaque neighbours, grown outward."""
    rgb = rgba[..., :3].astype(np.float32)
    known = rgba[..., 3] > 0
    if known.all() or not known.any():
        return rgb
    for _ in range(64):
        if known.all():
            break
        acc = np.zeros_like(rgb); cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (-1, -1), (1, -1), (-1, 1)):
            k = np.roll(known, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]; cnt += k
        new = ~known & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][:, None]
        known = known | new
    return rgb


def tiles(rgb, axis):
    """Opposite edges about as alike as neighbouring rows/columns: the texture repeats that way."""
    a = np.moveaxis(rgb, axis, 0)
    across = np.abs(a[0] - a[-1]).mean()
    inside = (np.abs(a[0] - a[1]).mean() + np.abs(a[-1] - a[-2]).mean()) / 2
    return across <= 1.5 * inside + 2.0


def pad(img, wrap):
    """wrap: (rows, cols) - wrap or extend each axis."""
    extra = ((0, 0),) * (img.ndim - 2)
    img = np.pad(img, ((PAD, PAD), (0, 0)) + extra, 'wrap' if wrap[0] else 'edge')
    return np.pad(img, ((0, 0), (PAD, PAD)) + extra, 'wrap' if wrap[1] else 'edge')


def main():
    args = sys.argv[1:]
    esr, src, dst = args[:3]
    model = args[args.index('--model') + 1] if '--model' in args else 'realesrgan-x4plus'
    scale = int(args[args.index('--scale') + 1]) if '--scale' in args else 4
    nowrap = '--nowrap' in args
    os.makedirs(dst, exist_ok=True)
    names = sorted(f for f in os.listdir(src) if f.endswith('.png') and not os.path.exists(os.path.join(dst, f)))
    groups = collections.defaultdict(list)
    for n in names:
        a = Image.open(os.path.join(src, n)).convert('RGBA').getchannel('A')
        if a.getextrema()[0] != a.getextrema()[1]:	# a shaped mask (all-opaque textures share theirs)
            groups[(a.size, hashlib.md5(a.tobytes()).digest())].append(n)
    generated = {n for g in groups.values() if len(g) > GENERATED for n in g}
    names = [n for n in names if n not in generated]
    if generated:
        print(f'skipping {len(generated)} textures the game redraws every frame (minimap)')
    if not names:
        print(f'nothing new for {dst}')
        return
    with tempfile.TemporaryDirectory() as tmp:
        tin, tout = os.path.join(tmp, 'in'), os.path.join(tmp, 'out')
        os.makedirs(tin); os.makedirs(tout)
        meta = {}
        for n in names:
            a = np.array(Image.open(os.path.join(src, n)).convert('RGBA'))
            alpha = a[..., 3]
            filled = fill_transparent(a)
            wrap = (False, False) if nowrap else (tiles(filled, 0), tiles(filled, 1))
            rgb = pad(filled, wrap)
            Image.fromarray(rgb.round().clip(0, 255).astype(np.uint8)).save(os.path.join(tin, n))
            meta[n] = (a.shape[1], a.shape[0], alpha)
            if alpha.min() < 255:	# alpha through the model too, as grey
                ap = pad(alpha, wrap)
                Image.fromarray(np.stack([ap] * 3, -1)).save(os.path.join(tin, 'alpha_' + n))
        subprocess.run([os.path.join(esr, 'realesrgan-ncnn-vulkan'), '-i', tin, '-o', tout,
                        '-m', os.path.join(esr, 'models'), '-n', model, '-f', 'png'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for n in names:
            w, h, alpha = meta[n]
            up = Image.open(os.path.join(tout, n)).convert('RGB')
            f = up.width // (w + 2 * PAD)
            up = up.crop((PAD * f, PAD * f, PAD * f + w * f, PAD * f + h * f))
            if alpha.min() < 255:
                au = Image.open(os.path.join(tout, 'alpha_' + n)).convert('L')
                au = au.crop((PAD * f, PAD * f, PAD * f + w * f, PAD * f + h * f))
                if np.isin(alpha, (0, 255)).all():	# punch-through cut-out: keep it hard
                    au = au.point(lambda v: 255 if v >= 128 else 0)
                up.putalpha(au)
            if scale != f:
                up = up.resize((w * scale, h * scale), Image.LANCZOS)
            up.save(os.path.join(dst, n))
    print(f'{len(names)} textures -> {dst} ({model}, x{scale})')


if __name__ == '__main__':
    main()
