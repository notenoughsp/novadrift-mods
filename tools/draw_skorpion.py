"""Zeichnet den Skorpion-Skin fuer Nova Drift (weiss auf transparent).

Nase zeigt nach rechts (+x), wie bei den offiziellen Beispiel-Skins.
Ausgabe: skorpion.png, skorpion_force.png, skorpion_drone.png, preview.png
Aufruf:  python -I draw_skorpion.py <ausgabeordner>
"""
import math
import os
import struct
import sys
import zlib

SS = 4  # Supersampling pro Achse


class Canvas:
    def __init__(self, w, h, scale=1.0):
        self.w, self.h, self.scale = w, h, scale
        self.alpha = [0.0] * (w * h)

    def _to_px(self, pts):
        cx, cy, s = self.w / 2, self.h / 2, self.scale
        return [(cx + x * s, cy - y * s) for x, y in pts]

    def fill(self, pts, value):
        """Fuellt ein Polygon (Even-Odd) und setzt dort den Alphawert (Maler-Algorithmus)."""
        poly = self._to_px(pts)
        cov = {}
        ys = [p[1] for p in poly]
        y0, y1 = max(0, int(min(ys) * SS)), min(self.h * SS - 1, int(max(ys) * SS) + 1)
        n = len(poly)
        for sy in range(y0, y1 + 1):
            y = (sy + 0.5) / SS
            xs = []
            for i in range(n):
                (ax, ay), (bx, by) = poly[i], poly[(i + 1) % n]
                if (ay <= y < by) or (by <= y < ay):
                    xs.append(ax + (y - ay) * (bx - ax) / (by - ay))
            xs.sort()
            row = sy // SS
            for a, b in zip(xs[0::2], xs[1::2]):
                sa, sb = max(0, math.ceil(a * SS - 0.5)), min(self.w * SS - 1, math.floor(b * SS - 0.5))
                for sx in range(sa, sb + 1):
                    k = row * self.w + sx // SS
                    cov[k] = cov.get(k, 0) + 1
        full = SS * SS
        for k, c in cov.items():
            t = c / full
            self.alpha[k] = self.alpha[k] * (1 - t) + value * t

    def png(self, path):
        rows = []
        for y in range(self.h):
            row = bytearray()
            for x in range(self.w):
                row += bytes((255, 255, 255, int(round(self.alpha[y * self.w + x] * 255))))
            rows.append(row)
        write_png(path, self.w, self.h, rows)


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# ------------------------------------------------------------- Formen -----

def mirror(pts):
    return [(x, -y) for x, y in reversed(pts)]


def circle(cx, cy, r, n=28):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def thick_line(a, b, width):
    (ax, ay), (bx, by) = a, b
    dx, dy = bx - ax, by - ay
    ln = math.hypot(dx, dy)
    nx, ny = -dy / ln * width / 2, dx / ln * width / 2
    return [(ax + nx, ay + ny), (bx + nx, by + ny), (bx - nx, by - ny), (ax - nx, ay - ny)]


def scaled(pts, k):
    return [(x * k, y * k) for x, y in pts]


# Rumpf: spitze Nase vorne, breiter Brustpanzer, schmal zum Heck
HULL = [(100, 0), (62, 14), (30, 26), (-10, 30), (-38, 22), (-52, 10), (-56, 0),
        (-52, -10), (-38, -22), (-10, -30), (30, -26), (62, -14)]

# Scherenarm oben (unten gespiegelt): Schulter -> Unterarm -> zweiteilige Schere
CLAW = [(12, 20), (2, 40), (10, 48), (22, 62), (52, 70), (84, 64), (104, 50), (110, 38),
        (96, 42), (90, 36), (98, 30), (82, 32), (72, 44), (48, 50), (32, 44), (30, 30)]

# Heckpanzer-Segmente (Schwanz nach hinten, endet im Stachel)
TAIL_SEGMENTS = [(-66, 16), (-84, 13), (-100, 10)]
STINGER = [(-108, 9), (-128, 10), (-140, 2), (-134, -2), (-124, 3), (-110, -6)]

ENGINE = [(-14, 24), (-24, 44), (-50, 46), (-60, 32), (-46, 16)]


def draw_ship(c, detail=True):
    c.fill(HULL, 1.0)
    for claw in (CLAW, mirror(CLAW)):
        c.fill(claw, 1.0)
    for x, r in TAIL_SEGMENTS:
        c.fill(circle(x, 0, r), 1.0)
    c.fill(STINGER, 1.0)
    c.fill(ENGINE, 1.0)
    c.fill(mirror(ENGINE), 1.0)
    if not detail:
        return
    # halbtransparente Panzerplatten und Fugen fuer Tiefe
    c.fill([(70, 0), (40, 12), (0, 16), (-30, 10), (-30, -10), (0, -16), (40, -12)], 0.55)
    c.fill([(48, 0), (20, 7), (-14, 6), (-14, -6), (20, -7)], 1.0)          # Cockpit-Grat
    for claw in (CLAW, mirror(CLAW)):
        sgn = 1 if claw is CLAW else -1
        c.fill([(x, sgn * y) for x, y in [(30, 50), (50, 58), (78, 56), (90, 50), (62, 50), (40, 46)]], 0.5)
    for x, r in TAIL_SEGMENTS:
        c.fill(thick_line((x + r - 2, -r * 0.7), (x + r - 2, r * 0.7), 2.4), 0.25)
    for sgn in (1, -1):
        c.fill(thick_line((22, sgn * 24), (-30, sgn * 20), 2.2), 0.3)        # Fugen am Brustpanzer
        c.fill(thick_line((-40, sgn * 30), (-50, sgn * 38), 2.0), 0.35)      # Duesenrippen


def force_armor(c):
    # Sechseckiger Schild-Rahmen um das ganze Schiff (wie bei den Beispiel-Skins)
    outer = [(150, 0), (78, 98), (-96, 98), (-162, 0), (-96, -98), (78, -98)]
    c.fill(outer, 1.0)
    c.fill(scaled(outer, 0.955), 0.0)


def preview(path, w, h, layers):
    """Vorschau: farbig getoent (wie Schildfarbe im Spiel) auf dunklem Grund."""
    tint = (90, 200, 255)
    bg = (14, 18, 30)
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = bg
            for canvas, ox, oy, col in layers:
                lx, ly = x - ox, y - oy
                if 0 <= lx < canvas.w and 0 <= ly < canvas.h:
                    a = canvas.alpha[ly * canvas.w + lx]
                    r, g, b = (r * (1 - a) + col[0] * a, g * (1 - a) + col[1] * a, b * (1 - a) + col[2] * a)
            row += bytes((int(r), int(g), int(b), 255))
        rows.append(row)
    write_png(path, w, h, rows)
    return tint


def main(out):
    os.makedirs(out, exist_ok=True)
    body = Canvas(300, 240)
    draw_ship(body)
    body.png(os.path.join(out, "skorpion.png"))

    force = Canvas(340, 220)
    force_armor(force)
    force.png(os.path.join(out, "skorpion_force.png"))

    drone = Canvas(150, 120, scale=0.5)
    draw_ship(drone, detail=False)
    drone.png(os.path.join(out, "skorpion_drone.png"))

    # Vorschau in doppelter Groesse (nur zum Ansehen, nicht fuers Spiel)
    big_body = Canvas(600, 480, scale=2.0)
    draw_ship(big_body)
    big_force = Canvas(680, 440, scale=2.0)
    force_armor(big_force)
    preview(os.path.join(out, "preview.png"), 720, 520,
            [(big_force, 20, 40, (60, 140, 190)), (big_body, 60, 20, (90, 200, 255))])


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
