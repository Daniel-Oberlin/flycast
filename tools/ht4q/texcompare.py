"""Local HTML gallery comparing original and upscaled textures (game-derived: keep it outside the repo).

    python3 -I texcompare.py TEXTURES_DIR [OUT.html]

TEXTURES_DIR holds track10_src/ (originals), track10_x4/ and track10_x2/ (texup.py output) and
optionally pilot/gen0.35, pilot/gen0.6 (generative experiments). Image paths in the page are
relative to TEXTURES_DIR, so the page goes there.
"""
import html, os, sys
import numpy as np
from PIL import Image

base = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(base, 'compare.html')
src = os.path.join(base, 'track10_src')
names = sorted(f[:-4] for f in os.listdir(src) if f.endswith('.png'))


def info(n):
    a = np.array(Image.open(os.path.join(src, n + '.png')).convert('RGBA')).astype(np.int16)
    h, w = a.shape[:2]
    alpha = a[..., 3]
    kind = 'opaque' if alpha.min() == 255 else 'cut-out' if np.isin(alpha, (0, 255)).all() else 'translucent'
    # how much the upscale changed it: original (smoothly enlarged) vs x4, mean abs difference
    up = np.array(Image.open(os.path.join(base, 'track10_x4', n + '.png')).convert('RGB')).astype(np.int16)
    big = np.array(Image.open(os.path.join(src, n + '.png')).convert('RGB').resize(up.shape[1::-1], Image.BICUBIC)).astype(np.int16)
    change = float(np.abs(up - big).mean())
    return dict(name=n, w=w, h=h, kind=kind, change=change)


items = [info(n) for n in names]
gens = [n for n in names if os.path.exists(os.path.join(base, 'pilot', 'gen0.6', n + '.png'))]


def card(it):
    n = it['name']
    scale = 256 / max(it['w'], it['h'])
    dw, dh = round(it['w'] * scale), round(it['h'] * scale)
    imgs = [('Original', f'track10_src/{n}.png', 'orig'), ('Sharpened (on the Quest, 2x)', f'track10_x2/{n}.png', ''),
            ('Sharpened 4x', f'track10_x4/{n}.png', '')]
    cells = ''.join(f'<figure><figcaption>{lab}</figcaption><a href="{p}" target="_blank">'
                    f'<img class="{c}" src="{p}" width="{dw}" height="{dh}" loading="lazy"></a></figure>'
                    for lab, p, c in imgs)
    return (f'<section class="card" data-kind="{it["kind"]}" data-size="{it["w"]}x{it["h"]}" data-change="{it["change"]:.1f}">'
            f'<h3>{n}</h3><p class="meta">{it["w"]}&times;{it["h"]} &middot; {it["kind"]} &middot; change {it["change"]:.1f}</p>'
            f'<div class="row">{cells}</div></section>')


def gen_card(n):
    imgs = [('Original', f'track10_src/{n}.png', 'orig'), ('Sharpened', f'track10_x4/{n}.png', ''),
            ('Remaster light', f'pilot/gen0.35/{n}.png', ''), ('Remaster strong', f'pilot/gen0.6/{n}.png', '')]
    cells = ''.join(f'<figure><figcaption>{lab}</figcaption><a href="{p}" target="_blank">'
                    f'<img class="{c}" src="{p}" width="256" height="256" loading="lazy"></a></figure>' for lab, p, c in imgs)
    return f'<section class="card"><h3>{n}</h3><div class="row">{cells}</div></section>'


sizes = sorted({f'{i["w"]}x{i["h"]}' for i in items}, key=lambda s: -int(s.split('x')[0]) * int(s.split('x')[1]))
page = f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Texture Comparison</title>
<style>
:root {{ --bg:#16181c; --card:#20242a; --text:#e8e8e8; --muted:#9aa3ad; --accent:#f2b134; }}
@media (prefers-color-scheme: light) {{ :root {{ --bg:#f3f4f6; --card:#ffffff; --text:#1d2025; --muted:#5d6670; --accent:#b36b00; }} }}
body {{ margin:0; padding:16px; background:var(--bg); color:var(--text); font:14px/1.4 -apple-system, system-ui, sans-serif; }}
h1 {{ font-size:20px; margin:0 0 4px; }} h2 {{ font-size:17px; margin:28px 0 8px; }}
.intro {{ color:var(--muted); max-width:900px; }}
.bar {{ position:sticky; top:0; z-index:2; background:var(--bg); padding:8px 0; display:flex; flex-wrap:wrap; gap:12px; align-items:center; border-bottom:1px solid #8883; }}
.bar label {{ color:var(--muted); }} select, input {{ font:inherit; }}
.grid {{ display:flex; flex-direction:column; gap:12px; }}
.card {{ background:var(--card); border-radius:8px; padding:10px 12px; }}
.card h3 {{ margin:0; font:600 13px ui-monospace, monospace; }} .meta {{ margin:2px 0 8px; color:var(--muted); font-size:12px; }}
.row {{ display:flex; flex-wrap:wrap; gap:12px; }}
figure {{ margin:0; }} figcaption {{ font-size:12px; color:var(--muted); margin-bottom:4px; }}
img {{ display:block; max-width:100%; height:auto; background:repeating-conic-gradient(#8885 0 25%, transparent 0 50%) 0 0/16px 16px; }}
body.pixels img.orig {{ image-rendering:pixelated; }}
.count {{ color:var(--accent); }}
</style></head><body>
<h1>Thunder Park textures: original vs AI-sharpened</h1>
<p class="intro">All {len(items)} textures from one lap of Thunder Park (minimap frames left out), upscaled with Real-ESRGAN x4plus.
The Quest currently uses the 2x versions. Each image is shown at the same size; click one to open it at full resolution.
"Change" is how much the sharpened version differs from the original simply enlarged (higher = more visible difference).
Originals are shown smoothly enlarged, as the game draws them; tick "show original pixels" to see the raw pixels instead.</p>
<div class="bar">
 <label>Sort <select id="sort"><option value="change">most changed first</option><option value="name">by name</option><option value="size">by size</option></select></label>
 <label>Type <select id="kind"><option value="">all</option><option>opaque</option><option>cut-out</option><option>translucent</option></select></label>
 <label>Size <select id="size"><option value="">all</option>{''.join(f'<option>{s}</option>' for s in sizes)}</select></label>
 <label><input type="checkbox" id="pix"> show original pixels</label>
 <span class="count" id="count"></span>
</div>
{'<h2>Generative remaster experiments</h2><div class="grid">' + ''.join(gen_card(n) for n in gens) + '</div>' if gens else ''}
<h2>All textures</h2>
<div class="grid" id="grid">{''.join(card(i) for i in items)}</div>
<script>
const grid = document.getElementById('grid'), cards = [...grid.children];
function apply() {{
  const s = sort.value, k = kind.value, z = size.value;
  const area = c => c.dataset.size.split('x').reduce((a, b) => a * b, 1);
  cards.sort((a, b) => s === 'change' ? b.dataset.change - a.dataset.change
    : s === 'size' ? area(b) - area(a) : a.querySelector('h3').textContent.localeCompare(b.querySelector('h3').textContent));
  let n = 0;
  for (const c of cards) {{ const show = (!k || c.dataset.kind === k) && (!z || c.dataset.size === z); c.style.display = show ? '' : 'none'; n += show; grid.appendChild(c); }}
  count.textContent = n + ' shown';
}}
for (const el of [sort, kind, size]) el.addEventListener('change', apply);
pix.addEventListener('change', () => document.body.classList.toggle('pixels', pix.checked));
apply();
</script></body></html>'''
open(out, 'w').write(page)
print(out, len(items), 'textures,', len(gens), 'generative')
