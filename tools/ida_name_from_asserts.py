"""
ida_name_from_asserts.py - Turn Nocturne's live assert strings into function names.

Watcom emitted, at every assert site, a `__FILE__` literal and a message that is
nearly always `CClass::method - ...`. Both are referenced from inside the function
that asserts, so a string xref walk gives us two things per function:

  * source file attribution  (`..\core\werewolf.cpp`)
  * a real name              (`CWerewolf::think`)

Only functions with exactly one candidate name are renamed, so a helper that
merely mentions two classes never gets a wrong label. Ambiguous and file-only
hits are still recorded in the JSON for the lifter to use as comments.

Run inside IDA's Python (idalib/idapro), Python 3.11:

    py -3.11 tools/ida_name_from_asserts.py analysis/nocturne.exe analysis/named.json
"""
import json
import re
import sys
from collections import defaultdict

import idapro
import ida_auto, ida_bytes, ida_funcs, ida_name, idautils

BS = chr(92)
SRC_FILE = re.compile("([A-Za-z0-9_]+" + BS + BS + r"[A-Za-z0-9_]+[.](?:cpp|hpp|c|h))", re.I)
CXX_NAME = re.compile(r"^\W*([A-Z][A-Za-z0-9_]{2,})::([A-Za-z_][A-Za-z0-9_]*)")


def containing_funcs(ea):
    """Functions that reference the data at `ea`."""
    out = set()
    for xref in idautils.DataRefsTo(ea):
        f = ida_funcs.get_func(xref)
        if f:
            out.add(f.start_ea)
    return out


def main(argv):
    binary, out_path = argv[0], argv[1]
    idapro.open_database(binary, True)
    ida_auto.auto_wait()

    names = defaultdict(set)   # func ea -> {"CClass::method"}
    files = defaultdict(set)   # func ea -> {"core\actor.cpp"}

    for s in idautils.Strings():
        text = str(s)
        m = SRC_FILE.search(text)
        if m:
            for fea in containing_funcs(s.ea):
                files[fea].add(m.group(1).lower())
            continue
        m = CXX_NAME.match(text)
        if m:
            label = f"{m.group(1)}::{m.group(2)}"
            for fea in containing_funcs(s.ea):
                names[fea].add(label)

    renamed = 0
    result = []
    for fea in sorted(set(names) | set(files)):
        cand = sorted(names.get(fea, ()))
        src = sorted(files.get(fea, ()))
        chosen = cand[0] if len(cand) == 1 else None
        if chosen:
            # IDA won't take '::'; keep it readable and unambiguous.
            if ida_name.set_name(fea, chosen.replace("::", "__"), ida_name.SN_NOCHECK | ida_name.SN_FORCE):
                renamed += 1
        result.append({"ea": fea, "name": chosen, "candidates": cand, "source_files": src})

    idapro.close_database(False)
    json.dump({"binary": binary, "renamed": renamed, "functions": result},
              open(out_path, "w"), indent=1)
    attributed = sum(1 for r in result if r["source_files"])
    print(f"{renamed} functions named, {attributed} attributed to a source file "
          f"({len(result)} touched) -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
