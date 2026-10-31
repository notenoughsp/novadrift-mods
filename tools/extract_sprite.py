"""Schneidet ein Sprite aus data.win aus (DXT5-Texturseiten) und speichert es als PNG.
Aufruf: python -I extract_sprite.py <data.win> <ausgabeordner> <spritename> [...]
"""
import os
import struct
import sys
import zlib


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def rgb565(c):
    return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def dxt5_block(b):
    a0, a1 = b[0], b[1]
    alphas = [a0, a1] + ([((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1
                         else [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
    abits = int.from_bytes(b[2:8], "little")
    c0, c1 = struct.unpack_from("<HH", b, 8)
    p0, p1 = rgb565(c0), rgb565(c1)
    cols = [p0, p1, tuple((2 * x + y) // 3 for x, y in zip(p0, p1)), tuple((x + 2 * y) // 3 for x, y in zip(p0, p1))]
    cbits = struct.unpack_from("<I", b, 12)[0]
    return [cols[(cbits >> (2 * i)) & 3] + (alphas[(abits >> (3 * i)) & 7],) for i in range(16)]


def main(path, out, names):
    d = open(path, "rb").read()
    pos, ch = 8, {}
    while pos < len(d):
        n = d[pos:pos + 4].decode()
        sz = struct.unpack_from("<I", d, pos + 4)[0]
        ch[n] = pos + 8
        pos += 8 + sz

    def lst(c):
        o = ch[c]
        return [struct.unpack_from("<I", d, o + 4 + 4 * i)[0] for i in range(struct.unpack_from("<I", d, o)[0])]

    def cstr(p):
        return d[p:d.index(b"\0", p)].decode("latin1")

    sprites = {cstr(struct.unpack_from("<I", d, p)[0]): p for p in lst("SPRT") if p}
    pages = lst("TXTR")
    os.makedirs(out, exist_ok=True)
    for name in names:
        tp = struct.unpack_from("<I", d, sprites[name] + 22 * 4)[0]
        sx, sy, sw, shh, tx, ty, tw, th, bw, bh, page = struct.unpack_from("<11H", d, tp)
        dds = struct.unpack_from("<I", d, pages[page] + 24)[0]
        pw = struct.unpack_from("<I", d, dds + 16)[0]
        base = dds + 128
        img = [[(0, 0, 0, 0)] * bw for _ in range(bh)]
        for by in range(sy // 4, (sy + shh + 3) // 4):
            for bx in range(sx // 4, (sx + sw + 3) // 4):
                o = base + (by * (pw // 4) + bx) * 16
                px = dxt5_block(d[o:o + 16])
                for i in range(16):
                    x, y = bx * 4 + i % 4 - sx, by * 4 + i // 4 - sy
                    if 0 <= x < sw and 0 <= y < shh:
                        img[ty + y][tx + x] = px[i]
        write_png(os.path.join(out, name + ".png"), bw, bh, [bytearray(b for p in row for b in p) for row in img])
        print(name, bw, bh)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
