"""Fast reference index for NovaDrift.exe, no decompiler needed (seconds, not minutes).

Finds in the machine code of every GML function:
  calls    - call rel32 to a known GML function or builtin
  vars     - rip-relative references to a variable/builtin id slot
  objects  - object reference constants (0x01000000_00000xxx)

Usage:
  python -I re_index.py build <NovaDrift.exe> <data.win> <index.json>
  python -I re_index.py refs  <index.json> <function name>     what a function uses
  python -I re_index.py users <index.json> <name>              functions using a variable, function or object
"""
import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re_tables  # noqa: E402


def build(exe, data_win, out):
    pe = re_tables.PE(exe)
    t = re_tables.load(exe, data_win)
    text_va, _, raw, size = pe.sections[".text"]
    code = np.frombuffer(pe.d, dtype=np.uint8, count=size, offset=raw)

    funcs = {**t["builtins"], **t["functions"]}
    starts = np.array(sorted(t["functions"]), dtype=np.int64)  # GML functions only define ranges
    names = [t["functions"][a] for a in starts]

    # int32 at every byte offset (little endian)
    c = code.astype(np.int64)
    disp = (c[:-3] | c[1:-2] << 8 | c[2:-1] << 16 | c[3:] << 24)
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    va = text_va + np.arange(len(disp), dtype=np.int64)
    target = va + 4 + disp  # rip-relative target if the disp32 ends the instruction

    def owner(addresses):
        idx = np.searchsorted(starts, addresses, side="right") - 1
        return idx

    refs = {n: {"calls": set(), "vars": set(), "objects": set()} for n in names}

    # calls: E8 rel32
    call_pos = np.nonzero(code[:-4] == 0xE8)[0]
    call_tgt = target[call_pos + 1]
    keys = np.array(sorted(funcs), dtype=np.int64)
    hit = np.isin(call_tgt, keys)
    for pos, tg in zip(call_pos[hit], call_tgt[hit]):
        i = owner(text_va + pos)
        if i >= 0:
            refs[names[i]]["calls"].add(funcs[int(tg)])

    # variable / builtin id slots
    slot_keys = np.array(sorted(t["slots"]), dtype=np.int64)
    hit = np.nonzero(np.isin(target, slot_keys))[0]
    for pos in hit:
        i = owner(text_va + pos)
        if i >= 0:
            refs[names[i]]["vars"].add(t["slots"][int(target[pos])])

    # object references: imm64 0x01000000_00000xxx (bytes: idx lo, idx hi, 0, 0, 0, 0, 0, 1)
    obj_names = {i: n for i, n, _ in t.get("objects", [])}
    cand = np.nonzero((code[7:] == 1) & (code[2:-5] == 0) & (code[3:-4] == 0) & (code[4:-3] == 0) &
                      (code[5:-2] == 0) & (code[6:-1] == 0))[0]
    for pos in cand:
        idx = int(code[pos]) | int(code[pos + 1]) << 8
        if idx in obj_names:
            i = owner(text_va + pos)
            if i >= 0:
                refs[names[i]]["objects"].add(obj_names[idx])

    data = {n: {k: sorted(v) for k, v in r.items()} for n, r in refs.items()}
    with open(out, "w") as f:
        json.dump(data, f, indent=0)
    print(f"{len(data)} functions indexed -> {out}")


def main(argv):
    cmd = argv[1]
    if cmd == "build":
        return build(*argv[2:5])
    data = json.load(open(argv[2]))
    name = argv[3]
    if cmd == "refs":
        for n in [n for n in data if n == name] or [n for n in data if name in n][:5]:
            print(f"== {n}")
            for k, v in data[n].items():
                print(f"  {k}: {' '.join(v)}")
    elif cmd == "users":
        for n, r in sorted(data.items()):
            kinds = [k for k, v in r.items() if name in v]
            if kinds:
                print(f"{n}  ({', '.join(kinds)})")


if __name__ == "__main__":
    main(sys.argv)
