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
- **Assets are POD archives** (Terminal Reality format; same family as Fury³ /
  Hellbender). `engine\pod.cpp` is the reader, class `CPodFile`, with CRC audit.

## Current Numbers (Phase 1, 2026-09-09)
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

Both are candidates to upstream into pcrecomp once a second Watcom target proves
them.

## Roadmap
0 recon ✅ · 1 disasm + symbols ✅ · 2 POD reader · 3 lift to C · 4 shims + BSS ·
5 build & link · 6 renderer (37-call `APIDLL*` → D3D11) · 7 bring-up

## Git Workflow
- `main` branch only unless told otherwise. Private repo.
- Stage specific files, never `git add -A` (game binaries live in the tree).
- Push after each meaningful batch; keep README status in sync.
