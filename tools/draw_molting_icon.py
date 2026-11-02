"""Draws the Molting wild mod hex icon (268x308): a cracked empty husk with the
ship slipping out of it, in the style of Nova Drift upgrade icons.

Usage:  python -I draw_molting_icon.py <output_dir>
Output: molting_icon.png
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from draw_icon import H, W, blur, hexagon, mask_of, nebula  # noqa: E402

SHIP = [(0, 70), (34, -40), (14, -26), (0, -46), (-14, -26), (-34, -40)]  # nose up, y up


def shifted(points, dx, dy, scale=1.0):
    return [(x * scale + dx, y * scale + dy) for x, y in points]


def husk(c):
    c.fill(shifted(SHIP, -10, -38, 1.25), 1.0)


def husk_hollow(c):
    c.fill(shifted(SHIP, -10, -38, 0.95), 1.0)


def crack(c):
    pts = [(-30, 40), (-14, 18), (-22, 2), (-4, -20), (-12, -36), (4, -58)]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        c.fill([(x0 - 3, y0), (x0 + 3, y0), (x1 + 3, y1), (x1 - 3, y1)], 1.0)


def ship(c):
    c.fill(shifted(SHIP, 18, 42, 0.95), 1.0)


def main(out):
    os.makedirs(out, exist_ok=True)
    hex_outer = mask_of(lambda c: hexagon(c, 152))
    hex_inner = mask_of(lambda c: hexagon(c, 141))
    shell = np.clip(mask_of(husk) - mask_of(husk_hollow), 0, 1)
    cracks = mask_of(crack) * mask_of(husk)
    body = mask_of(ship)

    n = nebula(29)
    t = np.clip((n - 0.35) * 1.8, 0, 1)
    rgb = np.stack([170 + 60 * t, 92 + 50 * t, 150 + 40 * t], axis=-1)  # wild: warm violet
    rng = np.random.default_rng(7)
    stars = (rng.random((H, W)) > 0.9975).astype(float)
    rgb = rgb * (1 - stars[..., None] * 0.7) + 255 * stars[..., None] * 0.7

    # Dim husk with cracks, bright ship with glow
    shell = np.clip(shell - cracks, 0, 1)
    rgb = rgb * (1 - shell[..., None] * 0.75) + np.array([200, 190, 210]) * shell[..., None] * 0.75
    glow = np.clip(blur(body, 8) * 2.0, 0, 1)
    rgb = rgb * (1 - glow[..., None] * 0.8) + np.array([255, 220, 160]) * glow[..., None] * 0.8
    rgb = rgb * (1 - body[..., None]) + np.array([255, 248, 235]) * body[..., None]

    border = np.clip(hex_outer - hex_inner, 0, 1)
    rgb = rgb * (1 - border[..., None])
    img = np.dstack([np.clip(rgb, 0, 255), hex_outer * 255]).astype(np.uint8)
    from draw_scorpion import write_png  # noqa: E402
    write_png(os.path.join(out, "molting_icon.png"), W, H, [bytearray(img[y].tobytes()) for y in range(H)])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
