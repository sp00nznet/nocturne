#!/usr/bin/env python3
"""
mine_symbols.py - Recover Nocturne's original source-file map and C++ symbol
names from the assert strings Watcom left in the binary.

NOCTURNE.EXE ships with asserts live. Every assert site emitted two literals into
DGROUP: the `__FILE__` path (`..\core\actor.cpp`) and the message text, and the
messages are overwhelmingly of the form `CClass::method - what went wrong`. That
gives us, for free, most of the original module layout and a partial symbol table
for a binary that has no symbols at all.

This only mines the strings. Attaching them to code addresses (string xref ->
containing function -> name) happens in the IDA pass; see docs/PHASE0.md.

    py -3 tools/mine_symbols.py analysis/nocturne.exe analysis/symbols.json
"""
import json
import re
import sys
from collections import Counter

BS = chr(92)
ASCII_RUN = re.compile(rb"[\x20-\x7e]{6,}")
SRC_FILE = re.compile("([A-Za-z0-9_]+" + BS + BS + r"[A-Za-z0-9_]+[.](?:cpp|hpp|c|h))", re.I)
CXX_NAME = re.compile(r"\b([A-Z][A-Za-z0-9_]{2,})::([A-Za-z_][A-Za-z0-9_]*)")


def mine(path):
    data = open(path, "rb").read()
    strings = [(m.start(), m.group().decode("ascii")) for m in ASCII_RUN.finditer(data)]
    blob = "\n".join(s for _, s in strings)

    files = sorted({m.group(1).lower() for m in SRC_FILE.finditer(blob)})
    methods = sorted({(m.group(1), m.group(2)) for m in CXX_NAME.finditer(blob)})
    # Offsets let the IDA pass turn a string xref back into a source file.
    file_at = {off: m.group(1).lower()
               for off, s in strings
               for m in [SRC_FILE.search(s)] if m}
    return {
        "binary": path,
        "source_files": files,
        "dirs": dict(Counter(f.split(BS)[0] for f in files)),
        "classes": dict(Counter(c for c, _ in methods)),
        "methods": [f"{c}::{f}" for c, f in methods],
        "file_string_offsets": {hex(k): v for k, v in sorted(file_at.items())},
    }


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    out = mine(argv[0])
    json.dump(out, open(argv[1], "w"), indent=1)
    print(f"{len(out['source_files'])} source files across {len(out['dirs'])} dirs, "
          f"{len(out['classes'])} classes, {len(out['methods'])} methods "
          f"-> {argv[1]}")
    for d, n in sorted(out["dirs"].items(), key=lambda kv: -kv[1]):
        print(f"  {d:<12} {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
