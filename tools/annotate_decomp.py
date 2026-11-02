"""Turns a REA/Ghidra decompile_all export into readable, searchable files.

FUN_<addr> becomes the GML or builtin function name, DAT_<addr> the variable or
builtin name of that slot. GML functions are written as <out>/gml/<name>.c,
everything else as <out>/other/<addr>.c, plus <out>/index.tsv.
The output is decompiled game code: keep it local, never commit or publish it.

Usage: python -I annotate_decomp.py <NovaDrift.exe> <decomp/functions dir> <out dir>
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re_tables  # noqa: E402

TOKEN = re.compile(r"\b(FUN|DAT|PTR_FUN)_(1[0-9a-f]{8})\b")


def main(exe, src, out):
    t = re_tables.load(exe)
    funcs = {**t["builtins"], **t["functions"]}
    slots = t["slots"]

    def name_of(m):
        addr = int(m.group(2), 16)
        if m.group(1) == "DAT" and addr in slots:
            return slots[addr]
        return funcs.get(addr, m.group(0))

    for sub in ("gml", "other"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    rows = []
    for fn in sorted(os.listdir(src)):
        if not fn.endswith(".c"):
            continue
        text = open(os.path.join(src, fn), encoding="utf-8", errors="replace").read()
        addr = int(fn[:-2], 16)
        body = "\n".join(l for l in text.splitlines() if not l.startswith("// limitations:"))
        body = TOKEN.sub(name_of, body)
        name = t["functions"].get(addr)
        path = os.path.join(out, "gml", re.sub(r"[^\w.]", "_", name) + ".c") if name else \
            os.path.join(out, "other", fn)
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"// {name or 'FUN'} @ {hex(addr)}\n{body}")
        rows.append((name or "", hex(addr), str(body.count("\n"))))
    with open(os.path.join(out, "index.tsv"), "w") as f:
        f.write("name\taddress\tlines\n" + "".join("\t".join(r) + "\n" for r in rows))
    print(f"{len(rows)} files, {sum(1 for r in rows if r[0])} GML -> {out}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
