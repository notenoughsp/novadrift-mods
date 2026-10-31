"""Draws the tail lash animation (stinger tail weapon) as a sprite strip.

A segmented scorpion tail lashing out from the stern (origin left) to the right.
Over the frames: curled wind-up -> fully stretched strike -> curl to the other side,
like a whip. Drawn in white, the game colors it via image_blend.
One frame (16 total): 220x130, origin (12, 65), 0 degrees = right.
Output: stachelschwanz_whip.png (FRAMES frames side by side, for sprite_add with imgnum)
Usage:  python -I draw_whip_sprite.py <output_dir>
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from draw_icon import blur  # noqa: E402
from draw_skorpion import Canvas, circle, write_png  # noqa: E402

W, H, OX, OY = 220, 130, 12, 65
FRAMES = 16
SEGMENTS = 16
LENGTH = 150


def mask(draw):
    c = Canvas(W, H)
    draw(c)
    return np.array(c.alpha).reshape(H, W)


def to_canvas(x, y):
    # Sprite coordinates (origin left middle, y up) -> canvas (center = 0,0)
    return x + OX - W / 2, y


def tail_path(f):
    """Segments (x, y, r) and end heading for animation phase f in [0, 1]."""
    extend = 0.62 + 0.38 * math.sin(math.pi * f)  # winds up, stretches, pulls back
    curl = 2.4 * (0.5 - f) * 2                    # curled -> straight -> curled the other way
    step = LENGTH * extend / (SEGMENTS - 1)
    x, y, heading = 6.0, 0.0, -curl * 0.25
    out = []
    for i in range(SEGMENTS):
        t = i / (SEGMENTS - 1)
        out.append((x, y, 13 - 6 * t))
        heading += curl * (t ** 1.5) * 2.2 / SEGMENTS
        x += math.cos(heading) * step
        y += math.sin(heading) * step
    return out, heading


def rotated(points, ox, oy, ang):
    ca, sa = math.cos(ang), math.sin(ang)
    return [to_canvas(ox + px * ca - py * sa, oy + px * sa + py * ca) for px, py in points]


def tail(c, f):
    segs, heading = tail_path(f)
    for x, y, r in segs:
        c.fill(circle(*to_canvas(x, y), r), 1.0)
    x, y, _ = segs[-1]
    # venom bulb and curled stinger along the last segment
    c.fill(circle(*to_canvas(x + math.cos(heading) * 10, y + math.sin(heading) * 10), 10), 1.0)
    sting = [(16, 8), (30, 9), (40, 15), (45, 23), (36, 14), (27, 2), (14, -8)]
    c.fill(rotated(sting, x, y, heading), 1.0)


def seams(c, f):
    segs, _ = tail_path(f)
    for (x0, y0, r0), (x1, y1, _) in zip(segs, segs[1:]):
        ang = math.atan2(y1 - y0, x1 - x0)
        mx, my = (x0 + x1) / 2, (y0 + y1) / 2
        c.fill(rotated([(-1.2, -r0 * 0.8), (1.2, -r0 * 0.8), (1.2, r0 * 0.8), (-1.2, r0 * 0.8)], mx, my, ang), 1.0)


def frame(f):
    body = mask(lambda c: tail(c, f))
    seam = mask(lambda c: seams(c, f)) * body
    # wide glow; counts towards the in-game collision mask (whip_mask_tolerance)
    glow = np.clip(blur(body, 8) * 1.5, 0, 1) * 0.55
    alpha = np.maximum(body, glow)
    gray = np.clip(1.0 - seam * 0.45, 0, 1) * body + 0.85 * (1 - body)
    return np.dstack([np.stack([gray] * 3, axis=-1) * 255, alpha * 255])


def main(out):
    os.makedirs(out, exist_ok=True)
    strip = np.concatenate([frame(i / (FRAMES - 1)) for i in range(FRAMES)], axis=1).astype(np.uint8)
    write_png(os.path.join(out, "stachelschwanz_whip.png"), W * FRAMES, H,
              [bytearray(strip[y].tobytes()) for y in range(H)])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
