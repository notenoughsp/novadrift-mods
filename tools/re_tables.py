"""Reads lookup tables from NovaDrift.exe and data.win for reverse engineering.

- functions: YYC function table {name, func} -> {address: "gml_Script_x"}
- slots: variable/builtin table {name, id} -> {address of the id slot: name}
- builtins: runtime registrations (lea rdx, func; lea rcx, "name") -> {address: name}
- objects: OBJT chunk -> [(index, name, parent index)]

Usage: python -I re_tables.py <NovaDrift.exe> [data.win] [out.json]
"""
import json
import re
import struct
import sys


class PE:
    def __init__(self, path):
        self.d = d = open(path, "rb").read()
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        count = struct.unpack_from("<H", d, pe + 6)[0]
        opt = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
        self.sections = {}
        for i in range(count):
            o = pe + 24 + opt + 40 * i
            name = d[o:o + 8].rstrip(b"\0").decode()
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, o + 8)
            self.sections[name] = (self.base + va, vsize, raw, rsize)

    def contains(self, section, va):
        start, size, _, _ = self.sections[section]
        return start <= va < start + size

    def offset(self, va):
        for start, _, raw, rsize in self.sections.values():
            if start <= va < start + rsize:
                return va - start + raw
        return None

    def cstr(self, va):
        o = self.offset(va)
        return self.d[o:self.d.index(b"\0", o)].decode("latin1") if o is not None else None

    def words(self, section):
        _, _, raw, rsize = self.sections[section]
        start = self.sections[section][0]
        for i in range(0, rsize - 16, 8):
            yield start + i, struct.unpack_from("<QQ", self.d, raw + i)


def functions(pe):
    out = {}
    for _, (name, func) in pe.words(".data"):
        if pe.contains(".rdata", name) and pe.contains(".text", func):
            s = pe.cstr(name)
            if s and s.startswith("gml_"):
                out.setdefault(func, s)
    return out


def slots(pe):
    out = {}
    for va, (name, value) in pe.words(".data"):
        if pe.contains(".rdata", name) and value in (0xFFFFFFFF, 0):
            s = pe.cstr(name)
            if s and re.fullmatch(r"[A-Za-z_]\w*", s):
                out[va + 8] = s
    return out


def builtins(pe):
    """lea rdx, [rip+func]; lea rcx, [rip+name] pairs (Function_Add calls)."""
    d = pe.d
    text_va, _, raw, rsize = pe.sections[".text"]
    out = {}
    for m in re.finditer(rb"\x48\x8d\x15(....)\x48\x8d\x0d(....)", d[raw:raw + rsize], re.S):
        o = m.start()
        func = text_va + o + 7 + struct.unpack("<i", m.group(1))[0]
        name_va = text_va + o + 14 + struct.unpack("<i", m.group(2))[0]
        if pe.contains(".text", func) and pe.contains(".rdata", name_va):
            s = pe.cstr(name_va)
            if s and re.fullmatch(r"[a-z_][a-z0-9_]*", s):
                out.setdefault(func, s)
    return out


def objects(path):
    d = open(path, "rb").read()
    pos = 8
    while pos < len(d):
        if d[pos:pos + 4] == b"OBJT":
            break
        pos += 8 + struct.unpack_from("<I", d, pos + 4)[0]
    o = pos + 8
    out = []
    for i in range(struct.unpack_from("<I", d, o)[0]):
        p = struct.unpack_from("<I", d, o + 4 + 4 * i)[0]
        if not p:
            continue
        s = struct.unpack_from("<I", d, p)[0]
        out.append((i, d[s:d.index(b"\0", s)].decode("latin1"), struct.unpack_from("<i", d, p + 0x1C)[0]))
    return out


def load(exe, data_win=None):
    pe = PE(exe)
    tables = {"functions": functions(pe), "slots": slots(pe), "builtins": builtins(pe)}
    if data_win:
        tables["objects"] = objects(data_win)
    return tables


def main(argv):
    tables = load(argv[1], argv[2] if len(argv) > 2 else None)
    out = argv[3] if len(argv) > 3 else "tables.json"
    hexed = {k: {hex(a): n for a, n in v.items()} if isinstance(v, dict) else v for k, v in tables.items()}
    with open(out, "w") as f:
        json.dump(hexed, f, indent=1)
    print({k: len(v) for k, v in tables.items()}, "->", out)


if __name__ == "__main__":
    main(sys.argv)
