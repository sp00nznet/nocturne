#!/usr/bin/env python3
"""
Generate Nocturne's import-bridge layer (Phase 4).

The lifted code reaches each Win32 import through its IAT slot VA. This emits a
C shim per import plus a {iat_va -> shim} bridge table the runtime installs.

Every shim must be stdcall-correct: it pops the dummy return address plus
exactly the right number of argument slots. That count is not cosmetic. Get it
wrong and the simulated stack silently desynchronises, so every value read after
that call is garbage -- and the symptom shows up nowhere near the cause. The
sibling projects kept the counts in a hand-typed table of a few hundred entries,
which is a lot of chances to mistype a number that fails silently.

So we don't type them. The Windows SDK's 32-bit import libraries already carry
the answer: stdcall exports are decorated `_Name@N`, where N is the argument
byte count, straight from the header the API was compiled against. We read the
counts out of the .lib files and refuse to emit anything we could not resolve.

Ordinal-only imports (Nocturne pulls DirectSound in by ordinal) are resolved
against the system DLL's export table first, then looked up by name.

Output: src/runtime/imports_gen.c
"""
import collections
import glob
import os
import re
import sys

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_here, '..', 'tools', 'tools', 'pe'))
from pe_analyze import analyze_pe, build_iat_map

SDK_GLOB = r"C:\Program Files (x86)\Windows Kits\10\Lib\*\um\x86"
SYSTEM32 = r"C:\Windows\SysWOW64"
DECORATED = re.compile(rb'_([A-Za-z_][A-Za-z0-9_]*)@(\d+)')

# Imports that are genuinely not stdcall-decorated in any .lib, with the reason.
# Keep this empty unless something really cannot be derived -- a hand-written
# number here is exactly the failure mode this script exists to avoid.
MANUAL_ARGC = {}


def sdk_lib_dir():
    """Newest installed 32-bit SDK lib directory."""
    dirs = sorted(glob.glob(SDK_GLOB))
    if not dirs:
        raise SystemExit("no 32-bit Windows SDK lib directory found under "
                         + SDK_GLOB)
    return dirs[-1]


def argc_from_lib(libdir, dll):
    """Map exported name -> argument slot count, from the DLL's import library.

    The decoration is a *byte* count, so 4 bytes per 32-bit slot.
    """
    path = os.path.join(libdir, os.path.splitext(dll)[0] + ".lib")
    if not os.path.exists(path):
        return {}
    data = open(path, "rb").read()
    out = {}
    for m in DECORATED.finditer(data):
        out[m.group(1).decode()] = int(m.group(2)) // 4
    return out


def ordinal_names(dll):
    """ordinal -> export name, read from the system copy of the DLL."""
    path = os.path.join(SYSTEM32, dll)
    if not os.path.exists(path):
        return {}
    try:
        import pefile
    except ImportError:
        return {}
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
    if not hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        return {}
    return {e.ordinal: e.name.decode()
            for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}


def resolve(iat):
    """[(va, dll, import_name, real_name, argc)], plus the unresolved ones."""
    libdir = sdk_lib_dir()
    lib_cache, ord_cache = {}, {}
    rows, unresolved = [], []

    for va, (dll, name) in sorted(iat.items()):
        if dll not in lib_cache:
            lib_cache[dll] = argc_from_lib(libdir, dll)

        real = name
        m = re.fullmatch(r'ordinal_(\d+)', name)
        if m:
            if dll not in ord_cache:
                ord_cache[dll] = ordinal_names(dll)
            real = ord_cache[dll].get(int(m.group(1)), name)

        argc = lib_cache[dll].get(real, MANUAL_ARGC.get(real))
        if argc is None:
            unresolved.append((va, dll, name, real))
        else:
            rows.append((va, dll, name, real, argc))
    return rows, unresolved


HEADER = """/* Nocturne Recompilation - import bridge layer - AUTO-GENERATED */
/* Regenerate: py -3 gen_imports.py   -- do not edit by hand */
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include "imports.h"

/* %d imports across %d DLLs.
 *
 * Argument counts are derived from the stdcall decoration (_Name@N) in the
 * Windows SDK's 32-bit import libraries, not hand-written. Every shim below
 * therefore pops exactly what the real API pops.
 *
 * Bodies are stubs: each logs once and returns a benign default. Real bodies
 * land in shims_impl.c during bring-up, declared extern here.
 */

"""


def emit(rows, out_path):
    dlls = sorted({d for _, d, _, _, _ in rows})
    lines = [HEADER % (len(rows), len(dlls))]
    seen = set()
    for va, dll, name, real, argc in rows:
        if real in seen:
            continue          # same export reached through two IAT slots
        seen.add(real)
        lines.append("/* %s!%s  (%d args) */" % (dll, real, argc))
        lines.append("static void imp_%s(void) { IMPORT_STUB(\"%s\"); "
                     "RET(0); STDRET(%d); }" % (real, real, argc))

    lines.append("\n/* IAT slot VA -> shim. The runtime installs these before "
                 "entry. */")
    lines.append("const recomp_dispatch_entry_t nocturne_import_bridges[] = {")
    for va, dll, name, real, argc in rows:
        lines.append("    { 0x%08Xu, imp_%s },  /* %s!%s */" % (va, real, dll, real))
    lines.append("};")
    lines.append("const uint32_t nocturne_import_bridge_count = %d;\n" % len(rows))

    lines.append("/* The IAT slots themselves, so the runtime can point each one "
                 "at its bridge. */")
    lines.append("const uint32_t nocturne_iat_slots[] = {")
    for va, _, _, _, _ in rows:
        lines.append("    0x%08Xu," % va)
    lines.append("};")
    lines.append("const uint32_t nocturne_iat_slot_count = %d;" % len(rows))

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    open(out_path, "w").write("\n".join(lines) + "\n")


def main(argv):
    exe = argv[0] if argv else os.path.join(_here, 'analysis', 'nocturne.exe')
    out = argv[1] if len(argv) > 1 else os.path.join(
        _here, 'src', 'runtime', 'imports_gen.c')

    info = analyze_pe(exe)
    iat = build_iat_map(info)
    rows, unresolved = resolve(iat)

    if unresolved:
        print("[!] could not derive an argument count for %d import(s):"
              % len(unresolved), file=sys.stderr)
        for va, dll, name, real in unresolved:
            print("      0x%08X  %s!%s%s" % (va, dll, name,
                  "" if real == name else " (-> %s)" % real), file=sys.stderr)
        print("    A guessed count silently desynchronises the simulated stack.\n"
              "    Add it to MANUAL_ARGC with a reason, or install the matching\n"
              "    32-bit SDK library.", file=sys.stderr)
        return 1

    emit(rows, out)
    by_dll = collections.Counter(d for _, d, _, _, _ in rows)
    print("%d import bridges across %d DLLs -> %s" % (len(rows), len(by_dll), out))
    for dll, n in sorted(by_dll.items()):
        print("  %-16s %d" % (dll, n))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
