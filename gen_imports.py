#!/usr/bin/env python3
"""
Generate Nocturne's import-bridge layer (Phase 4).

The lifted code reaches each Win32 import through its IAT slot VA. This emits a
C shim per import plus a {iat_va -> shim} bridge table the runtime installs.

Every shim must be stdcall-correct: it pops the dummy return address plus
exactly the right number of argument slots. That count is not cosmetic. Get it
wrong and the simulated stack silently desynchronises, so every value read after
that call is garbage -- and the symptom shows up nowhere near the cause.

So we don't type them. `pcrecomp/tools/pe/stdcall_argc.py` derives every count
from the decoration (`_Name@N`) in the Windows SDK's 32-bit import libraries,
and resolves ordinal-only imports (Nocturne pulls DirectSound in by ordinal)
against the system DLL's export table. This script just refuses to emit anything
the resolver could not derive.

Output: src/runtime/imports_gen.c
"""
import collections
import re
import os
import sys

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_here, '..', 'tools', 'tools', 'pe'))
from pe_analyze import analyze_pe, build_iat_map
from stdcall_argc import ArgcResolver

# Imports with real bodies in src/runtime/shims_impl.c. These get an extern
# declaration instead of a stub; the bridge table still points at them, so
# swapping a stub for a real body is a one-line change here.
#
# Names are checked against the import table below -- a typo would otherwise
# leave the stub in place and look exactly like the shim not being called.
HAND_WRITTEN = {
    # Memory
    'VirtualAlloc', 'VirtualFree', 'VirtualQuery',
    'GlobalMemoryStatus', 'GlobalAlloc', 'GlobalFree', 'GlobalLock',
    'GlobalUnlock',
    # Process / thread identity
    'GetModuleHandleA', 'GetCurrentThreadId', 'GetCurrentProcessId',
    'GetCurrentThread', 'GetCurrentProcess', 'GetVersion', 'GetStdHandle',
    # Synchronisation
    'CreateEventA', 'CreateMutexA', 'SetEvent', 'ReleaseMutex',
    'WaitForSingleObject', 'CloseHandle',
    # TLS
    'TlsAlloc', 'TlsFree', 'TlsGetValue', 'TlsSetValue',
    # Critical sections
    'InitializeCriticalSection', 'DeleteCriticalSection',
    'EnterCriticalSection', 'LeaveCriticalSection',
    # Errors
    'GetLastError', 'SetLastError',
    # Program identity / environment: what the CRT builds argv and environ from
    'GetCommandLineA', 'GetCommandLineW', 'GetModuleFileNameA',
    'GetModuleFileNameW', 'GetEnvironmentStrings', 'FreeEnvironmentStringsA',
    # File I/O: forwarded to the host's own Win32, which is the same API
    'CreateFileA', 'ReadFile', 'WriteFile', 'SetFilePointer', 'GetFileType',
    'FlushFileBuffers', 'GetFileTime', 'SetFileTime',
    'FindFirstFileA', 'FindNextFileA', 'FindClose',
    'GetFileAttributesA', 'SetFileAttributesA', 'DeleteFileA', 'MoveFileA',
    'CreateDirectoryA', 'GetCurrentDirectoryA', 'SetCurrentDirectoryA',
    'GetFullPathNameA',
    # Window, message pump and time: also forwarded to the host
    'RegisterClassA', 'CreateWindowExA', 'DefWindowProcA',
    'PeekMessageA', 'TranslateMessage', 'DispatchMessageA', 'PostQuitMessage',
    'ShowWindow', 'UpdateWindow', 'BringWindowToTop', 'SetForegroundWindow',
    'SetFocus', 'IsIconic', 'SetCursor', 'SetCursorPos', 'MoveWindow',
    'GetClientRect', 'SetRectEmpty', 'InvalidateRect', 'FindWindowA',
    'MessageBoxA', 'LoadIconA', 'LoadCursorA', 'GetStockObject',
    'timeGetTime', 'timeBeginPeriod', 'timeEndPeriod', 'Sleep',
    'QueryPerformanceCounter', 'QueryPerformanceFrequency', 'GetLocalTime',
    # Devices with no implementation yet: they report absent, not success
    'waveOutGetDevCapsA', 'waveOutOpen', 'joyGetDevCapsA', 'joyGetPos',
    'mciSendStringA', 'DirectSoundCreate', 'DirectDrawCreate',
    # Exit really exits
    'ExitProcess',
}


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
        if real in HAND_WRITTEN:
            lines.append("extern void imp_%s(void);  /* %s!%s (%d args) "
                         "- real body in shims_impl.c */" % (real, dll, real, argc))
            continue
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


def check_hand_written(rows, shims_path):
    """Verify each hand-written shim pops what the derived count says it should.

    Deriving 171 argument counts and then hand-typing STDRET in the shim bodies
    would put the mistake straight back where it was taken from -- and a wrong
    STDRET fails silently, desynchronising the simulated stack so that the dummy
    return address leaks into a register and the crash lands somewhere unrelated.
    So the two are checked against each other on every generate.
    """
    if not os.path.exists(shims_path):
        return
    derived = {real: argc for _, _, _, real, argc in rows}
    src = open(shims_path, encoding='utf-8').read()
    # Bound each body by the start of the next definition, so one-line shims
    # sitting on consecutive lines don't run together.
    starts = [(m.start(), m.group(1))
              for m in re.finditer(r'void imp_(\w+)\(void\)\s*\{', src)]
    problems = []
    for i, (pos, name) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(src)
        found = {int(x) for x in re.findall(r'STDRET\((\d+)\)', src[pos:end])}
        want = derived.get(name)
        if want is None:
            problems.append("%s: defined but not imported by this binary" % name)
        elif not found:
            problems.append("%s: no STDRET (should pop %d)" % (name, want))
        elif found != {want}:
            problems.append("%s: pops %s, should pop %d"
                            % (name, sorted(found), want))
    defined = {n for _, n in starts}
    for name in sorted(HAND_WRITTEN - defined):
        problems.append("%s: listed in HAND_WRITTEN but no body in %s"
                        % (name, os.path.basename(shims_path)))
    if problems:
        print("[!] hand-written shims disagree with the derived counts:",
              file=sys.stderr)
        for p in problems:
            print("      " + p, file=sys.stderr)
        raise SystemExit(1)
    print("  %d hand-written shims verified against derived counts" % len(starts))


def main(argv):
    exe = argv[0] if argv else os.path.join(_here, 'analysis', 'nocturne.exe')
    out = argv[1] if len(argv) > 1 else os.path.join(
        _here, 'src', 'runtime', 'imports_gen.c')

    info = analyze_pe(exe)
    iat = build_iat_map(info)
    rows, unresolved = ArgcResolver().resolve_iat(iat)

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

    # A HAND_WRITTEN name that is not actually imported means a typo, and a typo
    # here fails silently: the stub stays and looks just like the shim not being
    # reached. Say so rather than emitting something quietly wrong.
    imported = {real for _, _, _, real, _ in rows}
    stray = sorted(HAND_WRITTEN - imported)
    if stray:
        print("[!] HAND_WRITTEN names not in this binary's import table: %s"
              % ", ".join(stray), file=sys.stderr)
        return 1

    emit(rows, out)
    check_hand_written(rows, os.path.join(os.path.dirname(out), 'shims_impl.c'))
    by_dll = collections.Counter(d for _, d, _, _, _ in rows)
    print("%d import bridges across %d DLLs (%d hand-written, %d stubs) -> %s"
          % (len(rows), len(by_dll), len(HAND_WRITTEN),
             len(imported) - len(HAND_WRITTEN), out))
    for dll, n in sorted(by_dll.items()):
        print("  %-16s %d" % (dll, n))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
