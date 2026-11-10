"""Generate the README banner (docs/banner.svg).

Usage:
    python scripts/make_banner.py

This writes docs/banner.svg. To turn it into docs/banner.png, render it at
1600x400 with any Chromium browser, for example:

    brave --headless=new --hide-scrollbars --window-size=1600,400 \
          --screenshot=docs/banner.png docs/banner.svg

The title uses fonts that ship with Windows (Impact, Bahnschrift), so render
on Windows to get the intended look. The art is procedural and seeded, so the
output is the same on every run.
"""

import random
from pathlib import Path

W, H = 1600, 400
OUT = Path(__file__).resolve().parent.parent / "docs" / "banner.svg"

rng = random.Random(482)


def skyline(x0, x1, base, min_h, max_h, min_w, max_w, landmarks=()):
    """Return a list of (x, y, w, h) buildings filling x0..x1."""
    blocks = []
    x = x0
    while x < x1:
        w = rng.randint(min_w, max_w)
        h = rng.randint(min_h, max_h)
        blocks.append((x, base - h, w, h))
        x += w - rng.randint(0, 6)
    return blocks


def windows(blocks, density, colors, size=3, gap=9):
    out = []
    for (bx, by, bw, bh) in blocks:
        for wy in range(by + 10, by + bh - 8, gap):
            for wx in range(bx + 6, bx + bw - 6, gap - 1):
                if rng.random() < density:
                    c = rng.choice(colors)
                    o = rng.uniform(0.45, 1.0)
                    out.append(
                        f'<rect x="{wx}" y="{wy}" width="{size}" height="{size}" '
                        f'fill="{c}" opacity="{o:.2f}"/>'
                    )
    return out


def rects(blocks, fill):
    return "".join(
        f'<rect x="{x}" y="{y}" width="{w}" height="{h + 2}" fill="{fill}"/>'
        for (x, y, w, h) in blocks
    )


# --- landmarks -------------------------------------------------------------

def empire_state(cx, base, fill):
    # Stepped tower with a mast, built from rectangles.
    tiers = [(150, 270), (110, 300), (78, 320), (52, 336), (30, 348), (14, 362)]
    parts = []
    for w, top in tiers:
        top_y = base - (top - 60)
        parts.append(f'<rect x="{cx - w / 2}" y="{top_y}" width="{w}" height="{base - top_y + 2}" fill="{fill}"/>')
    # The upper setbacks are floodlit, like the real building at night.
    # Each lit band runs from a tier's top down to the top of the wider tier below it.
    for i in range(2, len(tiers)):
        w, top = tiers[i]
        top_y = base - (top - 60)
        h = (base - (tiers[i - 1][1] - 60)) - top_y
        parts.append(f'<rect x="{cx - w / 2}" y="{top_y}" width="{w}" height="{h}" fill="url(#esbLit)" opacity="{0.55 + 0.1 * (i - 2):.2f}"/>')
    mast_top = base - (362 - 60) - 40
    parts.append(f'<rect x="{cx - 1.5}" y="{mast_top}" width="3" height="44" fill="#2a3048"/>')
    top_y = base - (336 - 60)
    parts.insert(0, f'<ellipse cx="{cx}" cy="{top_y + 10}" rx="70" ry="60" fill="url(#esbHalo)"/>')
    return "".join(parts), (cx - 75, base - 210, 150, 210), mast_top


def chrysler(cx, base, fill):
    body_top = base - 200
    parts = [f'<rect x="{cx - 34}" y="{body_top}" width="68" height="202" fill="{fill}"/>']
    # Terraced crown: shrinking arches, then a needle.
    y = body_top
    for i, w in enumerate([56, 44, 32, 20]):
        h = 16
        arch = (f'M{cx - w / 2},{y} L{cx - w / 2},{y - h + 6} '
                f'Q{cx},{y - h - 10} {cx + w / 2},{y - h + 6} L{cx + w / 2},{y}')
        parts.append(f'<path d="{arch} Z" fill="#1b2236"/>')
        parts.append(
            f'<path d="M{cx - w / 2 + 3},{y - h + 8} Q{cx},{y - h - 6} {cx + w / 2 - 3},{y - h + 8}" '
            f'fill="none" stroke="#ffd98a" stroke-width="1.6" stroke-opacity="{0.85 - i * 0.12:.2f}"/>'
        )
        y -= h
    parts.append(f'<path d="M{cx - 4},{y + 4} L{cx},{y - 52} L{cx + 4},{y + 4} Z" fill="#1b2236"/>')
    return "".join(parts), (cx - 34, body_top, 68, 200)


# --- build layers ----------------------------------------------------------

base = H + 2
far = skyline(380, W + 20, base, 90, 190, 26, 60)
mid = skyline(560, W + 20, base, 60, 150, 34, 74)
near = skyline(470, W + 20, base, 18, 70, 40, 110)

def water_tower(x, roof, fill):
    # Classic NYC rooftop tank: legs, a barrel, and a cone roof.
    return (f'<path d="M{x + 2},{roof} L{x + 4},{roof - 12} M{x + 16},{roof} L{x + 14},{roof - 12} '
            f'M{x + 9},{roof} L{x + 9},{roof - 12}" stroke="{fill}" stroke-width="1.6"/>'
            f'<rect x="{x}" y="{roof - 30}" width="18" height="19" fill="{fill}"/>'
            f'<path d="M{x - 1.5},{roof - 30} L{x + 9},{roof - 39} L{x + 19.5},{roof - 30} Z" fill="{fill}"/>')


mid += [(1425, base - 235, 44, 235), (1505, base - 190, 58, 190), (1350, base - 165, 50, 165)]
near_fill = "#06070c"
towers = []
for (x, y, w, h) in near:
    if 620 < x < 1560 and w > 50 and h > 30 and rng.random() < 0.55:
        towers.append(water_tower(x + rng.randint(8, w - 26), y, near_fill))
for (x, y, w, h) in mid:
    if 1300 < x < 1600 and rng.random() < 0.4:
        towers.append(water_tower(x + rng.randint(4, max(5, w - 22)), y, "#0b0e18"))

esb_svg, esb_block, mast_top = empire_state(1230, base, "#0a0d16")
chr_svg, chr_block = chrysler(965, base, "#0c0f19")

lit = windows(mid + [esb_block, chr_block], 0.16, ["#f3b24f", "#ffd27a", "#9fd3ff", "#f3b24f"])
lit_far = windows(far, 0.07, ["#7fa6c9", "#c9a46a"], size=2, gap=8)

rain = []
for _ in range(520):
    x = rng.uniform(-40, W + 40)
    y = rng.uniform(-40, H)
    ln = rng.uniform(14, 34)
    o = rng.uniform(0.10, 0.42)
    sw = rng.choice([0.8, 1.0, 1.2])
    rain.append(
        f'<line x1="{x:.1f}" y1="{y:.1f}" x2="{x - ln * 0.22:.1f}" y2="{y + ln:.1f}" '
        f'stroke="#cfe3ff" stroke-width="{sw}" stroke-opacity="{o:.2f}" stroke-linecap="round"/>'
    )

svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="1" y2="0.35">
      <stop offset="0" stop-color="#2b0608"/>
      <stop offset="0.38" stop-color="#140709"/>
      <stop offset="0.7" stop-color="#0c1220"/>
      <stop offset="1" stop-color="#11233a"/>
    </linearGradient>
    <radialGradient id="cityglow" cx="0.72" cy="1.05" r="0.6">
      <stop offset="0" stop-color="#3a2a45" stop-opacity="0.75"/>
      <stop offset="1" stop-color="#3a2a45" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="sirenRed" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#ff2a3a" stop-opacity="0.55"/>
      <stop offset="1" stop-color="#ff2a3a" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="sirenBlue" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#2f7dff" stop-opacity="0.55"/>
      <stop offset="1" stop-color="#2f7dff" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="beam" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#e8f2ff" stop-opacity="0.16"/>
      <stop offset="1" stop-color="#e8f2ff" stop-opacity="0.07"/>
    </linearGradient>
    <linearGradient id="fadeLeft" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#fff" stop-opacity="0"/>
      <stop offset="0.36" stop-color="#fff" stop-opacity="0"/>
      <stop offset="0.58" stop-color="#fff" stop-opacity="1"/>
    </linearGradient>
    <mask id="cityMask"><rect width="{W}" height="{H}" fill="url(#fadeLeft)"/></mask>
    <linearGradient id="rainFade" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#fff" stop-opacity="0.25"/>
      <stop offset="0.45" stop-color="#fff" stop-opacity="0.55"/>
      <stop offset="1" stop-color="#fff" stop-opacity="1"/>
    </linearGradient>
    <mask id="rainMask"><rect width="{W}" height="{H}" fill="url(#rainFade)"/></mask>
    <linearGradient id="haze" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0.55" stop-color="#1a2236" stop-opacity="0"/>
      <stop offset="1" stop-color="#1a2236" stop-opacity="0.55"/>
    </linearGradient>
    <radialGradient id="vignette" cx="0.5" cy="0.5" r="0.75">
      <stop offset="0.6" stop-color="#000" stop-opacity="0"/>
      <stop offset="1" stop-color="#000" stop-opacity="0.55"/>
    </radialGradient>
    <linearGradient id="esbLit" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ff4a4f" stop-opacity="0.95"/>
      <stop offset="1" stop-color="#7a0f18" stop-opacity="0.55"/>
    </linearGradient>
    <radialGradient id="esbHalo" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#ff3540" stop-opacity="0.35"/>
      <stop offset="1" stop-color="#ff3540" stop-opacity="0"/>
    </radialGradient>
    <filter id="grain" x="0" y="0" width="100%" height="100%">
      <feTurbulence type="fractalNoise" baseFrequency="0.85" numOctaves="2" seed="7" stitchTiles="stitch"/>
      <feColorMatrix type="saturate" values="0"/>
    </filter>
    <filter id="beamBlur" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="6"/></filter>
    <filter id="soft" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="1.2"/></filter>
    <filter id="shadow" x="-10%" y="-20%" width="120%" height="150%">
      <feDropShadow dx="0" dy="6" stdDeviation="8" flood-color="#000" flood-opacity="0.75"/>
    </filter>
    <filter id="glow" x="-10%" y="-40%" width="120%" height="180%">
      <feGaussianBlur stdDeviation="14" result="b"/>
      <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
  </defs>

  <rect width="{W}" height="{H}" fill="url(#sky)"/>
  <rect width="{W}" height="{H}" fill="url(#cityglow)"/>


  <g mask="url(#cityMask)">
    <g opacity="0.9">{rects(far, "#18213a")}{''.join(lit_far)}</g>
    <rect width="{W}" height="{H}" fill="url(#haze)"/>
    <g>{rects(mid, "#0b0e18")}{chr_svg}{esb_svg}{''.join(lit)}</g>
    <circle cx="1230" cy="{mast_top}" r="2.6" fill="#ff3b3b"/>
    <circle cx="1230" cy="{mast_top}" r="9" fill="#ff3b3b" opacity="0.25"/>
    <g filter="url(#beamBlur)">
      <path d="M1520,-20 L1060,{H - 20} L1250,{H - 20} Z" fill="url(#beam)"/>
      <ellipse cx="1155" cy="{H - 22}" rx="105" ry="16" fill="#e8f2ff" opacity="0.16"/>
    </g>
    {rects(near, near_fill)}{''.join(towers)}
  </g>

  <!-- siren light washing the street -->
  <ellipse cx="1110" cy="{H - 6}" rx="230" ry="70" fill="url(#sirenRed)"/>
  <ellipse cx="1360" cy="{H - 6}" rx="230" ry="70" fill="url(#sirenBlue)"/>

  <g mask="url(#rainMask)" filter="url(#soft)">{''.join(rain)}</g>

  <rect width="{W}" height="{H}" filter="url(#grain)" opacity="0.07" style="mix-blend-mode:overlay"/>
  <rect width="{W}" height="{H}" fill="url(#vignette)"/>
  <rect x="0" y="0" width="6" height="{H}" fill="#e01f2b"/>

  <!-- title -->
  <g font-family="Bahnschrift, 'Segoe UI', Arial, sans-serif" font-weight="700">
    <text x="56" y="62" font-size="27" letter-spacing="7" fill="#e5262f">XBOX RECOMP</text>
  </g>
  <g filter="url(#shadow)" font-family="Impact, 'Anton', 'Arial Narrow', sans-serif" font-size="158">
    <text x="50" y="222" fill="#f1ece4" letter-spacing="1">TRUE <tspan fill="#e01f2b">CRIME</tspan></text>
  </g>
  <g font-family="Bahnschrift, 'Segoe UI', Arial, sans-serif" font-weight="700" fill="#f1ece4">
    <text x="56" y="280" font-size="42" letter-spacing="11">NEW YORK CITY</text>
    <rect x="57" y="302" width="410" height="3" fill="#e01f2b"/>
    <text x="57" y="345" font-size="27" letter-spacing="5" fill="#cfc8bd">NATIVE WINDOWS + LINUX</text>
  </g>
</svg>
"""

OUT.write_text(svg, encoding="utf-8")
print(f"wrote {OUT}")
