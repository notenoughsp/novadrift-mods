"""Zeichnet das Projektil-Sprite des Stachelschwanzes (Ersatz fuer sDefaultWeapon).

Gleiches Format wie das Original: 201x80, Ursprung (160, 40), Flugrichtung nach rechts.
Weiss/grau gezeichnet, die Farbe kommt im Spiel ueber image_blend.
Aufruf: python -I draw_stinger_shot.py <ausgabeordner>
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from draw_icon import blur  # noqa: E402
from draw_skorpion import Canvas, write_png  # noqa: E402

W, H, OX, OY = 201, 80, 160, 40


def mask(draw):
    c = Canvas(W, H)
    draw(c)
    return np.array(c.alpha).reshape(H, W)


def at(pts):
    # Canvas-Koordinaten: Mitte = (0,0), y nach oben -> auf Ursprung (160,40) verschieben
    k = 1.5  # Kopf etwa so gross wie beim Original-Schuss
    return [(x * k + OX - W / 2, y * k) for x, y in pts]


def main(out):
    os.makedirs(out, exist_ok=True)
    head = mask(lambda c: c.fill(at([(16, 0), (4, 5), (-14, 7), (-22, 4), (-14, 0), (-22, -4), (-14, -7), (4, -5)]), 1.0))
    barb = mask(lambda c: c.fill(at([(-6, 6), (-2, 13), (-12, 9)]), 1.0))
    core = mask(lambda c: c.fill(at([(12, 0), (0, 2.5), (-14, 2), (-14, -2), (0, -2.5)]), 1.0))

    xs = np.arange(W)[None, :]
    ys = np.arange(H)[:, None]
    trail = np.clip((xs - 40) / (OX - 22 - 40), 0, 1) ** 1.6 * (np.abs(ys - OY) < 3.5) * (xs < OX - 18)
    trail = blur(trail.astype(float), 1) * 0.7

    shape = np.clip(head + barb, 0, 1)
    glow = np.clip(blur(shape, 6) * 1.4, 0, 1) * 0.6
    alpha = np.clip(np.maximum.reduce([shape, glow, trail]), 0, 1)
    gray = np.clip(0.55 + 0.45 * np.maximum(core, shape * 0.6), 0, 1)
    rgb = (np.stack([gray] * 3, axis=-1) * 255)
    img = np.dstack([rgb, alpha * 255]).astype(np.uint8)
    write_png(os.path.join(out, "stachelschwanz_shot.png"), W, H, [bytearray(img[y].tobytes()) for y in range(H)])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
