"""Draws the Molting wild mod hex icon (268x308) in the style of Nova Drift upgrade icons.

Variants (prototypes):
  husk    - cracked empty husk with the ship slipping out of it (default)
  egg     - ship breaking out of a cracked shell, shards flying
  skin    - a shed, hollow segmented skin curling away
  absorb  - ghostly husk swallowing incoming shots
  flake   - ship shedding its plates, scales drifting off behind it

Usage:  python -I draw_molting_icon.py <output_dir> [variant ...]
Output: molting_icon.png (first variant) or molting_icon_<variant>.png for several
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from draw_icon import H, W, blur, hexagon, mask_of, nebula  # noqa: E402
from draw_scorpion import circle, write_png  # noqa: E402

SHIP = [(0, 70), (34, -40), (14, -26), (0, -46), (-14, -26), (-34, -40)]  # nose up, y up


def shifted(points, dx, dy, scale=1.0, angle=0.0):
    ca, sa = math.cos(angle), math.sin(angle)
    return [((x * ca - y * sa) * scale + dx, (x * sa + y * ca) * scale + dy) for x, y in points]


def poly(points):
    return lambda c: c.fill(points, 1.0)


def line(c, x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    ln = math.hypot(dx, dy) or 1
    nx, ny = -dy / ln * w, dx / ln * w
    c.fill([(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)], 1.0)


# Each variant returns layers: [(mask, rgb, opacity, glow radius)], drawn in order
def v_husk():
    shell = np.clip(mask_of(poly(shifted(SHIP, -10, -38, 1.25))) - mask_of(poly(shifted(SHIP, -10, -38, 0.95))), 0, 1)
    cracks = mask_of(lambda c: [line(c, *p, 3) for p in [(-30, 40, -14, 18), (-14, 18, -22, 2), (-22, 2, -4, -20),
                                                         (-4, -20, -12, -36), (-12, -36, 4, -58)]])
    return [(np.clip(shell - cracks, 0, 1), (200, 190, 210), 0.75, 0),
            (mask_of(poly(shifted(SHIP, 18, 42, 0.95))), (255, 248, 235), 1.0, 8)]


def v_egg():
    egg = lambda c: c.fill([(math.cos(a) * 62 + 0, math.sin(a) * 78 - 30) for a in np.linspace(0, 2 * math.pi, 48)], 1.0)
    shell = np.clip(mask_of(egg) - mask_of(lambda c: c.fill(
        [(math.cos(a) * 52, math.sin(a) * 68 - 30) for a in np.linspace(0, 2 * math.pi, 48)], 1.0)), 0, 1)
    top = mask_of(lambda c: c.fill([(-90, -10), (90, -10), (90, 120), (-90, 120)], 1.0))  # top half broken away
    zig = mask_of(lambda c: c.fill([(-70, -10), (-46, 8), (-26, -14), (-6, 10), (16, -12), (36, 8), (58, -12),
                                    (70, -10), (70, -30), (-70, -30)], 1.0))
    lower = np.clip(shell * (1 - top) + shell * zig, 0, 1)
    shards = mask_of(lambda c: [c.fill(shifted([(-10, -6), (10, -8), (4, 9)], x, y, s, a), 1.0)
                                for x, y, s, a in [(-70, 60, 1.4, 0.5), (66, 72, 1.2, -0.8), (-46, 100, 1.0, 1.4),
                                                   (52, 108, 0.9, 2.2)]])
    return [(lower, (220, 205, 230), 0.85, 0), (shards, (220, 205, 230), 0.75, 0),
            (mask_of(poly(shifted(SHIP, 0, 34, 1.0))), (255, 248, 235), 1.0, 8)]


def v_skin():
    rings = []
    for i in range(9):
        t = i / 8
        ang = -2.2 + t * 3.6
        rad = 70 - 34 * t
        rings.append((math.cos(ang) * rad - 6, math.sin(ang) * rad + 6, 26 - 14 * t))
    outer = mask_of(lambda c: [c.fill(circle(x, y, r), 1.0) for x, y, r in rings])
    inner = mask_of(lambda c: [c.fill(circle(x, y, r * 0.62), 1.0) for x, y, r in rings])
    seams = mask_of(lambda c: [line(c, x - r * 0.9, y, x + r * 0.9, y, 1.6) for x, y, r in rings])
    skin = np.clip(outer - inner * 0.85 - seams * 0.5, 0, 1)
    return [(skin, (235, 215, 160), 0.9, 6)]


def v_absorb():
    ghost = mask_of(poly(shifted(SHIP, 0, 0, 1.45)))
    hollow = mask_of(poly(shifted(SHIP, 0, 0, 1.1)))
    shots = mask_of(lambda c: [c.fill(circle(math.cos(a) * r, math.sin(a) * r, 7 - r / 30), 1.0)
                               for a, r in [(0.3, 110), (1.2, 96), (2.4, 104), (3.4, 118), (4.4, 100), (5.4, 112)]])
    trails = mask_of(lambda c: [line(c, math.cos(a) * r, math.sin(a) * r, math.cos(a) * (r + 34), math.sin(a) * (r + 34),
                                     2.2) for a, r in [(0.3, 110), (1.2, 96), (2.4, 104), (3.4, 118), (4.4, 100),
                                                       (5.4, 112)]])
    return [(np.clip(ghost - hollow, 0, 1), (230, 220, 240), 0.9, 6), (hollow, (200, 180, 220), 0.35, 0),
            (trails, (255, 150, 120), 0.5, 0), (shots, (255, 190, 150), 1.0, 5)]


def v_flake():
    ship = mask_of(poly(shifted(SHIP, 0, 30, 1.0)))
    plates = mask_of(lambda c: [c.fill(shifted([(-9, -7), (9, -7), (11, 6), (-11, 6)], x, y, s, a), 1.0)
                                for x, y, s, a in [(-30, -40, 1.3, 0.4), (24, -58, 1.1, -0.6), (-12, -78, 1.0, 1.0),
                                                   (34, -92, 0.85, -1.3), (-38, -104, 0.7, 2.0), (6, -118, 0.6, 0.2)]])
    return [(plates, (210, 200, 225), 0.8, 0), (ship, (255, 248, 235), 1.0, 8)]


# --- Turtle shell variants: carapace seen from above (oval, rim, 3 central scutes, side plates)
def ellipse(cx, cy, rx, ry, angle=0.0, n=56):
    ca, sa = math.cos(angle), math.sin(angle)
    return [(cx + (math.cos(t) * rx) * ca - (math.sin(t) * ry) * sa, cy + (math.cos(t) * rx) * sa + (math.sin(t) * ry) * ca)
            for t in np.linspace(0, 2 * math.pi, n, endpoint=False)]


def carapace(cx, cy, s=1.0, angle=0.0):
    """Masks (body, seams) of a turtle shell."""
    ca, sa = math.cos(angle), math.sin(angle)

    def p(x, y):
        return cx + (x * ca - y * sa) * s, cy + (x * sa + y * ca) * s

    def seam(c, a, b, w=2.6):
        line(c, *p(*a), *p(*b), w * s)

    hexes = [(0, 38), (0, 0), (0, -38)]
    corners = lambda hx, hy: [(hx + math.cos(k * math.pi / 3) * 21, hy + math.sin(k * math.pi / 3) * 21) for k in range(6)]

    def seams(c):
        rim = ellipse(0, 0, 50, 64)
        for a, b in zip(rim, rim[1:] + rim[:1]):
            seam(c, a, b)
        for hx, hy in hexes:
            pts = corners(hx, hy)
            for a, b in zip(pts, pts[1:] + pts[:1]):
                seam(c, a, b)
        for hx, hy in hexes:  # side plates: from the side corners out to the rim
            for k in (0, 3):
                x, y = corners(hx, hy)[k]
                seam(c, (x, y), (x + (50 if k == 0 else -50) * 0.62, y + 6))
        for x, y in [(0, 59), (0, -59)]:
            seam(c, (x, y * 0.66), (x, y))
        for ang in np.linspace(0, 2 * math.pi, 14, endpoint=False):  # rim plates
            seam(c, (math.cos(ang) * 50, math.sin(ang) * 64), (math.cos(ang) * 62, math.sin(ang) * 78), 2.0)

    body = mask_of(lambda c: c.fill([p(x, y) for x, y in ellipse(0, 0, 62, 78)], 1.0))
    return body, mask_of(seams) * body


SHELL = (232, 214, 170)
SEAM = (120, 92, 70)


def v_shell_molt():
    old, old_seams = carapace(-14, -34, 1.05, 0.25)
    hollow = mask_of(lambda c: c.fill(ellipse(-14, -34, 52, 66, 0.25), 1.0))
    new, new_seams = carapace(16, 30, 0.85, -0.1)
    return [(np.clip(old - hollow * 0.8, 0, 1), (205, 190, 200), 0.75, 0), (old_seams * 0.6, SEAM, 0.5, 0),
            (new, SHELL, 1.0, 8), (new_seams, SEAM, 0.85, 0)]


def v_shell_crack():
    body, seams = carapace(0, 0, 1.25)
    crack = mask_of(lambda c: [line(c, *a, *b, 3.2) for a, b in
                               [((-6, 96), (8, 60)), ((8, 60), (-10, 30)), ((-10, 30), (12, 0)), ((12, 0), (-6, -28)),
                                ((-6, -28), (10, -62)), ((10, -62), (-4, -96)), ((12, 0), (44, 12)), ((-10, 30), (-46, 40))]])
    return [(body, SHELL, 1.0, 0), (seams, SEAM, 0.85, 0), (crack, (255, 250, 220), 1.0, 7)]


def v_shell_plates():
    body, seams = carapace(0, 14, 1.0)
    plates = mask_of(lambda c: [c.fill(shifted([(-12, -8), (12, -8), (15, 7), (-15, 7)], x, y, sc, a), 1.0)
                                for x, y, sc, a in [(-46, -78, 1.0, 0.5), (40, -88, 0.9, -0.7), (-14, -106, 0.8, 1.1),
                                                    (58, -54, 0.75, -1.4), (-62, -46, 0.7, 2.1)]])
    return [(plates, SHELL, 0.85, 0), (body, SHELL, 1.0, 6), (seams, SEAM, 0.85, 0)]


def v_shell_absorb():
    body, seams = carapace(0, 0, 1.15)
    hollow = mask_of(lambda c: c.fill(ellipse(0, 0, 54, 70), 1.0))
    shots = [(0.3, 112), (1.2, 100), (2.4, 108), (3.4, 118), (4.4, 104), (5.4, 114)]
    dots = mask_of(lambda c: [c.fill(circle(math.cos(a) * r, math.sin(a) * r, 7 - r / 30), 1.0) for a, r in shots])
    trails = mask_of(lambda c: [line(c, math.cos(a) * r, math.sin(a) * r, math.cos(a) * (r + 32), math.sin(a) * (r + 32), 2.2)
                                for a, r in shots])
    return [(np.clip(body - hollow * 0.55, 0, 1), (230, 215, 235), 0.9, 6), (seams * 0.8, SEAM, 0.6, 0),
            (trails, (255, 150, 120), 0.5, 0), (dots, (255, 190, 150), 1.0, 5)]


def v_shell_split():
    left, ls = carapace(-30, -6, 1.0, 0.3)
    right, rs = carapace(30, -6, 1.0, -0.3)
    cut_l = mask_of(lambda c: c.fill([(-200, -200), (-12, -200), (-12, 200), (-200, 200)], 1.0))
    cut_r = mask_of(lambda c: c.fill([(12, -200), (200, -200), (200, 200), (12, 200)], 1.0))
    gap = mask_of(lambda c: c.fill([(-7, -84), (7, -84), (7, 84), (-7, 84)], 1.0))
    return [(gap, (255, 245, 210), 1.0, 10), (left * cut_l, SHELL, 1.0, 0), (ls * cut_l, SEAM, 0.85, 0),
            (right * cut_r, SHELL, 1.0, 0), (rs * cut_r, SEAM, 0.85, 0)]


VARIANTS = {"husk": v_husk, "egg": v_egg, "skin": v_skin, "absorb": v_absorb, "flake": v_flake,
            "shell_molt": v_shell_molt, "shell_crack": v_shell_crack, "shell_plates": v_shell_plates,
            "shell_absorb": v_shell_absorb, "shell_split": v_shell_split}


def render(variant, seed=29):
    hex_outer = mask_of(lambda c: hexagon(c, 152))
    hex_inner = mask_of(lambda c: hexagon(c, 141))
    n = nebula(seed)
    t = np.clip((n - 0.35) * 1.8, 0, 1)
    rgb = np.stack([170 + 60 * t, 92 + 50 * t, 150 + 40 * t], axis=-1)  # wild: warm violet
    stars = (np.random.default_rng(7).random((H, W)) > 0.9975).astype(float)
    rgb = rgb * (1 - stars[..., None] * 0.7) + 255 * stars[..., None] * 0.7
    for mask, color, opacity, glow in VARIANTS[variant]():
        if glow:
            g = np.clip(blur(mask, glow) * 2.0, 0, 1) * 0.7
            rgb = rgb * (1 - g[..., None]) + np.array([255, 220, 170]) * g[..., None]
        m = mask * opacity
        rgb = rgb * (1 - m[..., None]) + np.array(color) * m[..., None]
    border = np.clip(hex_outer - hex_inner, 0, 1)
    rgb = rgb * (1 - border[..., None])
    return np.dstack([np.clip(rgb, 0, 255), hex_outer * 255]).astype(np.uint8)


def main(out, variants):
    os.makedirs(out, exist_ok=True)
    for v in variants:
        img = render(v)
        name = "molting_icon.png" if len(variants) == 1 else f"molting_icon_{v}.png"
        write_png(os.path.join(out, name), W, H, [bytearray(img[y].tobytes()) for y in range(H)])
    if len(variants) > 1:  # side-by-side preview
        strip = np.concatenate([render(v) for v in variants], axis=1)
        write_png(os.path.join(out, "molting_icons_preview.png"), W * len(variants), H,
                  [bytearray(strip[y].tobytes()) for y in range(H)])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".", sys.argv[2:] or ["husk"])
