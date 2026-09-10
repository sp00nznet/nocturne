# Nocturne Static Recompilation — Project Memory

## Standing Directive
Keep the README's status table and numbers current with every phase. Commit and
push to `main` on GitHub regularly. Repo is **private**.

## Project Overview
Static recompilation of _Nocturne_ (1999, Terminal Reality) — lifting the retail
Watcom-built `nocturne.exe` (v1.01, 1999-11-02) to C for native Windows 11.

## Key Paths
- **Repo root**: `G:/recomp/pc/nocturne`
- **pcrecomp toolbox**: `G:/recomp/pc/tools` (shared checkout; scripts live in `tools/tools/`)
- **Binaries under analysis**: `analysis/` (gitignored — user-supplied)
- **Installed game**: `_game/Nocturne/` (gitignored; extracted from `nocturne.iso`)
- **Source ISO**: `nocturne.iso` — a WinRAR SFX repack, not a pressed disc image

## Hard-Won Facts (do not re-derive)
- **Compiler is Watcom C/C++32**, linker 2.18. Sections are `AUTO` / `DGROUP` /
  `.bss` / `.idata` / `.reloc` / `.rsrc`.
- **Every section header has `VirtualSize = 0`**; real size is in `SizeOfRawData`.
  pcrecomp's `pe_analyze.py` reports a 0-byte code range because of this.
- **Calling convention is cdecl, not `__watcall`.** 4,225 of ~4,400 sampled call
  sites push args; every `ret` is bare (no `ret N`). The stack-oriented lifter
  applies unchanged. Register convention only risks appearing in the CRT.
- **The renderer is a swappable DLL** — `rendererDLLPath=tridx7.dll` in
  `SYSTEM/nocturne.ini`. `tridx7` / `tridx6` / `trid3d` / `tri3dfx` all export the
  same 37-function `APIDLL*` C API. The exe imports one DDraw function and zero
  D3D. Porting the renderer = implementing 37 functions.
- **Asserts are live in the retail build** and messages follow
  `CClass::method - ...`, so the binary carries a partial symbol table.
- **42 MB static BSS.** Engine pools are static; the lifted image needs a real
  reserved region.
- **Assets are POD archives** — 41 of them, 12,383 files. Format is **fully
  decoded** in `tools/pod.py`: 40 are POD2 (magic `POD2`, name table, per-file
  checksums, 312-byte audit records at the very end after the file data), and
  `tground.pod` alone is POD1 (no magic, 32-byte fixed names, 40-byte entries —
  identical to Fury³/Hellbender). The assert `Invalid pod version!` is the tell.
- **`.ACT` files are always exactly 768 bytes** (256×RGB palette); `.THM` always
  3,072,000. `.RAW`+`.ACT`+`.FOG` triples are the texture pipeline, which is what
  the renderer interface consumes.
- **Asset extensions cross-check the assert module map** one-to-one
  (`.KFM`↔`keyframe.cpp`, `.SKL`↔`skeleton.cpp`, `.CTH`↔`cloth.cpp`,
  `.MSN`↔`mission.cpp`, `.SET`↔`set.cpp`). Use it to pick which lifted functions
  to read when a format needs decoding.

## Lift Pipeline (Phase 3)
- `run_lift.py` is the Fury³/Hellbender driver retargeted. It selects the code
  section by the `is_code` characteristic (NOT the name — Watcom calls it `AUTO`)
  and slices with `Section.effective_size`.
- **It copies `recomp_types.h` from the pcrecomp checkout on every run.** Do not
  hand-copy that header into the project; the lifter and the header are two halves
  of one contract and a stale copy is how the `_mm` bug hid.
- Per-function scratch comes from `lift32.FUNCTION_LOCALS` — do not re-inline a
  hand-written preamble list, that is what went stale before.
- Build check (until Phase 5 brings CMake):
  `cl /nologo /c /W1 /I src\recomp\gen src\recomp\gen\*.c` via
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat`.
  Currently 0 errors, 0 warnings, 15 objects.

## Runtime (Phase 4/5)
- `src/runtime/recomp_runtime.c` — register file, dispatch lookups, IAT install,
  entry. `src/runtime/imports.h` — ARG/RET/STDRET macros.
- **The stack goes ABOVE the image**, at a base derived from the loaded span.
  Below does not fit: only ~4 MB exists under 0x00400000 and a 4 MB stack at
  0x00100000 runs into the image at 0x00400000 (ERROR_INVALID_ADDRESS).
- `gen_imports.py` derives every stdcall arg count from the decoration `_Name@N`
  in the Windows SDK's **32-bit** import libs. It REFUSES to emit an import it
  cannot derive — never add a guessed count, a wrong STDRET desynchronises the
  simulated stack silently. Ordinal imports resolve via the system DLL's exports.
- Build is x64 host (the CMakeLists enforces it) so the target's VA range is free.
  `cmake -S . -B build -G Ninja && cmake --build build`
- Generated files are gitignored: `src/recomp/gen/`, `src/runtime/imports_gen.c`.

## Bring-up State (Phase 7)
- 95 of 171 imports have real bodies in `src/runtime/shims_impl.c`; the rest log
  once and return 0. Add a name to `HAND_WRITTEN` in gen_imports.py AND write the
  body — the generator fails if one exists without the other.
- **A stub that returns 0 is not neutral.** MMSYSERR_NOERROR, DS_OK, DD_OK and
  MMSYSERR_* are all zero, so a stub claims success and leaves the caller's out
  struct uninitialised. waveOutGetDevCapsA doing that put garbage into a CRT
  printf and faulted three frames away. Unimplemented devices report *absent*.
- **File I/O and the window are forwarded to the host's own Win32** — the host
  IS Windows. The only translation is the handle: a HANDLE is 64-bit here and a
  32-bit slot over there, so open files/finds/windows/GDI objects live in tagged
  tables (`H_FILE_TAG`/`H_FIND_TAG`/`H_OBJ_TAG`). Buffers need no translation;
  simulated memory is host memory, so ReadFile writes straight into the image.
- **The window procedure calls back INTO lifted code** (`call_lifted()`): push
  args below the current ESP, push the dummy return address, run, restore ESP.
  Restoring ESP rather than trusting the callee's epilogue means it works for a
  stdcall or a cdecl body.
- **A pointer lParam cannot survive the 32-bit round trip.** WM_NCCREATE carries
  a host CREATESTRUCT*; the game gets a truncated half and hands it back to
  DefWindowProcA, which dereferences it, returns FALSE, and aborts
  CreateWindowEx. `g_cur_msg` remembers the real lParam for the message in
  flight and gives it back. If the engine ever *reads* one, marshal that struct.
- **ExitProcess must actually exit.** A stub that returns lets the game run past
  its own decision to quit, and everything after is code the original would
  never have executed.
- **gen_imports.py verifies every hand-written shim's STDRET against the derived
  count on each run.** Never hand-tune a STDRET to "make it work".
- **Build sets `RECOMP_RETADDR=0u`** as a MITIGATION, not a fix: a stack
  imbalance during CRT startup leaks the dummy return address into an out-pointer
  argument at `sub_0056E9D0`. The callee null-checks, so 0 is inert. Finding the
  real argument-count mismatch is open work — then restore a poisoned value.
- **FS/TIB: DONE.** `shims_make_tib()` allocates a TIB in the arena, `g_fs_base`
  points at it, `fs:[0]` seeded to 0xFFFFFFFF. Verified: `ebx=FFFFFFFF` at the
  next fault is that sentinel read back. The lifter already emitted
  `FS_BASE + off` for all 3 fs: sites, so no re-lift was needed.
- **STARTUP WORKS.** CRT → WinMain → RegisterClassA → CreateWindowExA → clean
  exit, 34,987 indirect calls. Root cause of the long stall was a LIFTING bug:
  a body whose last instruction is neither `ret` nor `jmp` falls through to the
  next function, and run_lift.py ended those with a bare `return;` — dropping the
  `ret`, so the dummy return address was never popped. 4 bytes leaked per call,
  272 sites. Fixed in run_lift.py + pcrecomp recover.py (e0f2d84).
- The heap was NEVER broken. **Disproven hypotheses — do not redo:**
  (1) GlobalMemoryStatus zeroed struct; (2) 64 KB allocation granularity;
  (3) `push es/fs/gs` miscounted. (1) and (2) were fixed anyway, correctly.
- **Lesson: three build-and-run cycles against guesses found nothing; one
  crash-time dump of real state found it.** Use `recomp_set_extra_reporter()`.
- **Second marker leak still open** in `sub_0056DD80`, past window creation.
  Build with `-DRECOMP_RETADDR=0xDEAD0000u` to hunt it; default is 0u so
  bring-up continues.
- CPU-cheap workflow: `cmake --build <dir> -- -j 2`. Only runtime files change,
  so Ninja rebuilds 2 objects + links. Do NOT re-run run_lift.py unless the
  lifter changed.
- Debugging: `-DRECOMP_TRACE` gives the function-entry ring buffer; the crash
  handler prints `g_cur_func`, registers, ICALL history, and region names.

## Upstream Bugs Found And Fixed (pcrecomp)
1. `lift32.FUNCTION_LOCALS` — the lifter/driver locals contract existed nowhere;
   each driver's hand-copied preamble went stale when `_flag_k` was added.
2. `#define _mm g_mm` in `recomp_types.h` — the MMX alias was never added, so
   every MMX body failed to compile against a global that was already declared.
3. `op_bits()` — `shl` (and `shld`/`shrd`) hardcoded a 32-bit width for the carry
   flag and the shifted-in bits. CF was 0 for every narrow left shift, silently,
   in every project on the toolchain. Two sites in this binary.
4. `image_loader.c` tested `SizeOfRawData == 0` for "uninitialized section". That
   is only true of MSVC's `.bss` by coincidence; Watcom puts the MEMORY size
   there and leaves `PointerToRawData` 0, so the loader would have copied 42 MB
   from file offset 0 out of a 1.9 MB buffer, over the BSS. Now tests
   `PointerToRawData == 0` and clamps the copy to the file and the mapped span.

5. `runtime/recomp32/crash_report.{c,h}` — the VEH crash reporter was trapped
   inside an XWA-specific `main.c`. Now standalone, reports `g_cur_func`, and
   takes a region-describer callback so bad pointers print their region.
6. `recover.py` only treated `jmp`/`call` into another body as an alternate
   entry. Watcom reaches a shared epilogue with `jcc` just as readily, and those
   were left as `ITAIL: unresolved` — a tail call that silently does nothing.
   It also never re-scanned a *recovered* body for alternate entries of its own,
   so recovery is now a fixpoint. +236 functions on this binary (5,791 → 6,027).
7. `lift32.FUNCTION_LOCALS` declared `ebp` as a per-function local. Correct for a
   well-behaved function, wrong the moment the compiler scatters one function's
   blocks across the image: they all address the same frame through ebp, and a
   private copy starting at 0 makes `[ebp-0x20]` read `0xFFFFFFE0`. ebp is a
   register; it lives in recomp_types.h with the rest of the file.
8. `recomp_eflags()`'s FK_EFLAGS path masked EFLAGS down to the six arithmetic
   bits, dropping AC (18) and ID (21). Toggling ID and reading it back is the
   universal CPUID probe — so the answer was always "no CPUID", and Nocturne put
   up "This CPU does not have an MMX unit" and quit.

Also found: the sibling projects' hand-typed import ARGC table gives
`waveOutOpen` 7 args; the SDK decoration says 6. Not fixed there — noted as the
reason this project derives the counts instead.

## Upstreamed Tooling (use these, don't re-copy)
- `pcrecomp/tools/pe/stdcall_argc.py` — `ArgcResolver` derives stdcall arg counts
  from SDK import libs; resolves ordinals via the system DLL. `--selftest`.
- `pcrecomp/tools/lift/recover.py` — `recover_functions()`, the missing-function
  and alternate-entry scan shared by all lift drivers. `--selftest`.
- `pcrecomp/runtime/recomp32/crash_report.{c,h}` — crash diagnostics.

## Current Numbers (Phase 7, 2026-09-10)
- 6,027 lifted functions (5,494 from IDA + 533 recovered), 903,247 lines of C
- 171 imports across 8 DLLs (KERNEL32 89, USER32 30, GDI32 14, ADVAPI32 5,
  DSOUND 2 by ordinal, DDRAW 1)
- 99 original source files / 7 dirs; 88 classes; 266 `Class::method` names
- 253 functions renamed from asserts; 522 attributed to a source file

## Tools In This Repo
- `tools/mine_symbols.py` — mines source-file map + C++ names from assert strings
  (stdlib only, no IDA needed)
- `tools/ida_name_from_asserts.py` — walks IDA string xrefs to rename functions
  and attribute them to source files. Only renames on an unambiguous single
  candidate.
- `tools/pod.py` — POD1/POD2 reader: `list`, `audit`, `extract`, `--selftest`.
  Stdlib only, no IDA. Extraction refuses `..` traversal paths.

All three are candidates to upstream into pcrecomp; `pod.py` supersedes
`fury3/tools/extract_pod.py` (which is POD1-only).

## Roadmap
0 recon ✅ · 1 disasm + symbols ✅ · 2 POD reader ✅ · 3 lift to C ✅ ·
4 shims + BSS ✅ · 5 build & link ✅ · 6 renderer (37-call `APIDLL*` → D3D11) ·
7 bring-up (CRT, WinMain, real window + message loop, file I/O, engine init;
    now blocked on the renderer — `wincore\wddvmem.cpp` wants DirectDraw)

## Git Workflow
- `main` branch only unless told otherwise. Private repo.
- Stage specific files, never `git add -A` (game binaries live in the tree).
- Push after each meaningful batch; keep README status in sync.
