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

## Current Numbers (Phase 2, 2026-09-09)
- 5,494 functions (673 FLIRT library, 190 thunks, 39 no-return), 1.46 MB code
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
0 recon ✅ · 1 disasm + symbols ✅ · 2 POD reader ✅ · 3 lift to C · 4 shims + BSS ·
5 build & link · 6 renderer (37-call `APIDLL*` → D3D11) · 7 bring-up

## Git Workflow
- `main` branch only unless told otherwise. Private repo.
- Stage specific files, never `git add -A` (game binaries live in the tree).
- Push after each meaningful batch; keep README status in sync.
