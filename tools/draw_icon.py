"""Zeichnet das Hex-Icon des Skorpions im Stil der Nova-Drift-Gear-Icons (268x308).

Aufruf: python -I draw_icon.py <ausgabeordner>
Ausgabe: skorpion_icon.png
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from draw_skorpion import CLAW, ENGINE, HULL, STINGER, TAIL_SEGMENTS, Canvas, circle, mirror, write_png  # noqa: E402

W, H = 268, 308


def mask_of(draw):
    c = Canvas(W, H)
    draw(c)
    return np.array(c.alpha, dtype=np.float64).reshape(H, W)


def hexagon(c, r):
    cx, cy = 0, 0
    pts = [(cx + r * math.cos(math.radians(90 + 60 * i)) * 0.87 / 0.866,
            cy + r * math.sin(math.radians(90 + 60 * i))) for i in range(6)]
    c.fill(pts, 1.0)


def blur(a, radius):
    k = 2 * radius + 1
    for axis in (0, 1):
        p = np.pad(a, [(radius, radius) if i == axis else (0, 0) for i in range(2)], mode="edge")
        cs = np.cumsum(p, axis=axis)
        cs = np.concatenate([np.zeros_like(np.take(cs, [0], axis=axis)), cs], axis=axis)
        a = (np.take(cs, range(k, cs.shape[axis]), axis=axis) - np.take(cs, range(0, cs.shape[axis] - k), axis=axis)) / k
    return a


def nebula(seed=7):
    rng = np.random.default_rng(seed)
    acc = np.zeros((H, W))
    for octave, weight in ((6, 0.5), (12, 0.3), (24, 0.2)):
        g = rng.random((octave + 2, octave + 2))
        ys = np.linspace(0, octave, H)
        xs = np.linspace(0, octave, W)
        y0, x0 = ys.astype(int), xs.astype(int)
        fy, fx = (ys - y0)[:, None], (xs - x0)[None, :]
        fy, fx = fy * fy * (3 - 2 * fy), fx * fx * (3 - 2 * fx)
        a = g[y0][:, x0]
        b = g[y0][:, x0 + 1]
        c = g[y0 + 1][:, x0]
        d = g[y0 + 1][:, x0 + 1]
        acc += weight * (a * (1 - fx) * (1 - fy) + b * fx * (1 - fy) + c * (1 - fx) * fy + d * fx * fy)
    return acc


def ship(c):
    # Skorpion aus draw_skorpion, um 90 Grad gedreht (Nase nach oben) und skaliert
    k = 0.8

    def rot(pts):
        return [(-y * k, x * k + 12) for x, y in pts]

    c.fill(rot(HULL), 1.0)
    c.fill(rot(CLAW), 1.0)
    c.fill(rot(mirror(CLAW)), 1.0)
    for x, r in TAIL_SEGMENTS:
        c.fill(rot(circle(x, 0, r)), 1.0)
    c.fill(rot(STINGER), 1.0)
    c.fill(rot(ENGINE), 1.0)
    c.fill(rot(mirror(ENGINE)), 1.0)


def details(c):
    k = 0.8

    def rot(pts):
        return [(-y * k, x * k + 12) for x, y in pts]

    c.fill(rot([(78, 0), (50, 9), (10, 12), (-24, 6), (-24, -6), (10, -12), (50, -9)]), 1.0)
    for sgn in (1, -1):
        c.fill(rot([(x, sgn * y) for x, y in [(36, 52), (58, 60), (84, 57), (92, 52), (66, 53), (44, 49)]]), 1.0)


def main(out):
    os.makedirs(out, exist_ok=True)
    hex_outer = mask_of(lambda c: hexagon(c, 152))
    hex_inner = mask_of(lambda c: hexagon(c, 141))
    body = mask_of(ship)
    inner = mask_of(details)

    n = nebula()
    t = np.clip((n - 0.35) * 1.8, 0, 1)
    base = np.stack([88 + 70 * t, 104 + 70 * t, 196 + 45 * t], axis=-1)
    pink = np.clip((nebula(3) - 0.62) * 3, 0, 1)[..., None] * np.array([70, -20, 10])
    yy = np.linspace(0, 1, H)[:, None, None]
    bg = np.clip(base + pink - yy * np.array([10, 0, -10]), 0, 255)
    rng = np.random.default_rng(11)
    stars = (rng.random((H, W)) > 0.9975).astype(float)
    bg = bg * (1 - stars[..., None] * 0.7) + 255 * stars[..., None] * 0.7

    rgb = bg.copy()
    glow = np.clip(blur(body, 6) * 2.2 - body, 0, 1)
    rgb = rgb * (1 - glow[..., None]) + np.array([170, 235, 255]) * glow[..., None]
    edge = np.clip(body - blur(body, 2) * 0.0, 0, 1)
    rim = np.clip((body - np.clip(blur(body, 3) * 1.6 - 0.6, 0, 1)) * 3, 0, 1)
    navy = np.array([4, 22, 74])
    rgb = rgb * (1 - edge[..., None]) + navy * edge[..., None]
    rgb = rgb * (1 - rim[..., None]) + np.array([200, 245, 255]) * rim[..., None]
    detail_line = np.clip((inner - np.clip(blur(inner, 1) * 1.8 - 0.8, 0, 1)) * 2.5, 0, 1)
    rgb = rgb * (1 - detail_line[..., None]) + np.array([170, 235, 255]) * detail_line[..., None]

    border = np.clip(hex_outer - hex_inner, 0, 1)
    rgb = rgb * (1 - border[..., None])  # schwarzer Rand
    alpha = hex_outer * 255

    img = np.dstack([np.clip(rgb, 0, 255), alpha]).astype(np.uint8)
    write_png(os.path.join(out, "skorpion_icon.png"), W, H, [bytearray(img[y].tobytes()) for y in range(H)])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
