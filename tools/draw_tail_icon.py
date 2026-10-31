"""Zeichnet das Hex-Icon des Stachelschwanzes im Stil der Nova-Drift-Waffen-Icons (268x308).

Aufruf: python -I draw_tail_icon.py <ausgabeordner>
Ausgabe: stachelschwanz_icon.png
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from draw_icon import H, W, blur, hexagon, mask_of, nebula  # noqa: E402
from draw_skorpion import circle, write_png  # noqa: E402


def tail_points():
    """Bogen von unten links nach oben rechts, Segmente werden zur Spitze kleiner."""
    path = [(-20, -118), (-30, -90), (-36, -60), (-36, -30), (-30, 0), (-18, 28), (0, 52), (22, 70), (46, 78),
            (68, 72), (82, 56)]
    return [(x, y, 21 - 11 * i / (len(path) - 1)) for i, (x, y) in enumerate(path)]


def tail(c):
    for x, y, r in tail_points():
        c.fill(circle(x, y, r), 1.0)
    x, y, _ = tail_points()[-1]
    # Stachel: nach unten links gebogene Spitze (Schlagrichtung)
    c.fill([(x - 8, y + 4), (x + 10, y + 2), (x + 6, y - 22), (x - 10, y - 46), (x - 6, y - 20)], 1.0)


def joints(c):
    pts = tail_points()
    for (x0, y0, r0), (x1, y1, _) in zip(pts, pts[1:]):
        mx, my = (x0 + x1) / 2, (y0 + y1) / 2
        dx, dy = x1 - x0, y1 - y0
        ln = math.hypot(dx, dy)
        nx, ny = -dy / ln * r0 * 0.8, dx / ln * r0 * 0.8
        c.fill([(mx + nx - dx * 0.04, my + ny - dy * 0.04), (mx + nx + dx * 0.04, my + ny + dy * 0.04),
                (mx - nx + dx * 0.04, my - ny + dy * 0.04), (mx - nx - dx * 0.04, my - ny - dy * 0.04)], 1.0)


def main(out):
    os.makedirs(out, exist_ok=True)
    hex_outer = mask_of(lambda c: hexagon(c, 152))
    hex_inner = mask_of(lambda c: hexagon(c, 141))
    body = mask_of(tail)
    seams = mask_of(joints)
    x, y, _ = tail_points()[-1]
    venom = mask_of(lambda c: [c.fill(circle(x - 12 + dx, y - 50 + dy, r), 1.0)
                               for dx, dy, r in [(0, 0, 6), (-14, -12, 5), (-26, -26, 4), (-6, -24, 3)]])

    n = nebula(13)
    t = np.clip((n - 0.35) * 1.8, 0, 1)
    rgb = np.stack([92 + 60 * t, 108 + 70 * t, 200 + 40 * t], axis=-1)
    rng = np.random.default_rng(31)
    stars = (rng.random((H, W)) > 0.9975).astype(float)
    rgb = rgb * (1 - stars[..., None] * 0.7) + 255 * stars[..., None] * 0.7

    glow = np.clip(blur(body, 8) * 2.0, 0, 1)
    rgb = rgb * (1 - glow[..., None] * 0.8) + np.array([160, 235, 255]) * glow[..., None] * 0.8
    rgb = rgb * (1 - body[..., None]) + np.array([225, 248, 255]) * body[..., None]
    rgb = rgb * (1 - seams[..., None] * body[..., None]) + np.array([120, 200, 240]) * (seams * body)[..., None]
    vglow = np.clip(blur(venom, 5) * 2.2, 0, 1)
    rgb = rgb * (1 - vglow[..., None]) + np.array([170, 255, 170]) * vglow[..., None]

    border = np.clip(hex_outer - hex_inner, 0, 1)
    rgb = rgb * (1 - border[..., None])
    img = np.dstack([np.clip(rgb, 0, 255), hex_outer * 255]).astype(np.uint8)
    write_png(os.path.join(out, "stachelschwanz_icon.png"), W, H, [bytearray(img[y].tobytes()) for y in range(H)])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
