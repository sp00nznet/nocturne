# Nocturne — Static Recompilation

**A from-the-binary static recompilation of _Nocturne_ (1999, Terminal Reality) —
lifting `nocturne.exe` back to C so it runs natively on Windows 11.**

Nocturne is one of the hardest games of its generation to actually play today. It
never got a modern re-release, it wants DirectDraw and a Voodoo-era rasteriser,
and the community keeps it alive with shims. This project takes the other route:
recompile the binary into readable C, and put a modern renderer behind the
interface the engine already exposes.

> Part of the [pcrecomp](https://github.com/sp00nznet/pcrecomp) family of PC
> static-recompilation projects (civ · dinopark · elfish · fury3 · hellbender ·
> gta · recoil · crimsonskies · black & white · …). Terminal Reality's earlier
> voxel engine is already covered by [fury3](https://github.com/sp00nznet/fury3)
> and [hellbender](https://github.com/sp00nznet/hellbender); Nocturne is the same
> studio, three years and one complete engine rewrite later.

---

## Status

🟢 **Phase 1 complete — the binary is disassembled and partially self-naming.**
5,494 functions catalogued, 253 of them recovered with their **original C++
names** and 522 attributed to their **original source files**, straight out of
the asserts Terminal Reality left in the retail build.

| Phase | What | State |
|------:|------|:-----:|
| 0 | Reconnaissance — PE analysis, imports, compiler ID, engine seams | ✅ done |
| 1 | Disassembly + symbol recovery — 5,494 funcs, 253 named, 522 file-attributed | ✅ done |
| 2 | POD archive reader — mount the game's data outside the engine | ⬜ next |
| 3 | Lift to C — `lift32` over the IDA bounds, chunked output + dispatch table | ⬜ |
| 4 | Shim layer — 171 import bridges, Watcom CRT, 42 MB static BSS image | ⬜ |
| 5 | Build & link — one native exe | ⬜ |
| 6 | Renderer — implement the 37-call `APIDLL*` interface on D3D11 | ⬜ |
| 7 | Bring-up — CRT → WinMain → window → POD mount → menu → in-game | ⬜ |

## Why Nocturne is a better target than it looks

Three findings from [Phase 0](docs/PHASE0.md) decide the whole shape of this project.

**1. The GPU seam already exists.** Nocturne loads its rasteriser as a DLL —
`rendererDLLPath=tridx7.dll` in `SYSTEM/nocturne.ini` — and the four rasterisers
that shipped with the game (DirectX 7, DirectX 6, generic D3D, and 3dfx Glide)
all export the *same* 37-function C API: `APIDLLinit`, `APIDLLbeginScene`,
`APIDLLdrawPolyList`, `APIDLLselectTexture`, `APIDLLlockFrame`, and so on. The
exe itself imports exactly one DirectDraw function and no Direct3D at all. So
"port the renderer" is not an archaeology project against inlined D3D7 calls —
it is a clean implementation of 37 documented-by-example functions, with four
reference implementations to read, one of which (Glide) proves the interface
carries no D3D assumptions.

**2. It's cdecl, not `__watcall`.** The binary is Watcom C/C++32 (linker 2.18,
`AUTO`/`DGROUP` sections), and Watcom's default register calling convention would
have been a real problem for a stack-oriented lifter. It isn't used here: of
~4,400 sampled call sites, 4,225 pass arguments on the stack and every single
`ret` is a bare `ret` with no stack adjustment. Caller-cleans-stack, i.e. cdecl.
The existing pcrecomp `lift32` pipeline applies without convention work.

**3. The binary names its own functions.** The retail build shipped with
`assert()` live, and the house style for assert messages was
`CClass::method - what went wrong`. Every one of those sites left a `__FILE__`
literal and a message in the data segment:

```
..\core\actor.cpp    CDemonActor::customRayIntersect should not be called for this base class
..\engine\pod.cpp    CPodFile::getAuditRecord - invalid index.  Pod not mounted?
..\core\platfrm.cpp  CPlatform::attachActor - too many!
```

Mining those recovers **99 original source filenames** across 7 directories, **88
classes** and **266 method names** — and walking the string xrefs attaches 253 of
them to actual code. We start Phase 3 with a real symbol table and a module map
for a binary that has neither.

## Recon numbers

| | Nocturne | Hellbender (for comparison) |
|---|---|---|
| Format | PE32, i386, Windows GUI | same |
| Size | 1.86 MB | 1.68 MB |
| Compiler / linker | **Watcom C/C++32**, link 2.18 | MSVC, link 3.10 |
| Image base / entry | `0x00400000` / `0x00567152` | `0x00400000` / `0x004ADD30` |
| Imports | **171 across 8 DLLs** | 507 across 11 |
| Functions (IDA) | **5,494** (673 library, 190 thunks) | 5,221 |
| Static BSS | **42 MB** | — |
| Renderer | swappable DLL, 37-call C API | in-binary software voxel |

Full writeup: **[docs/PHASE0.md](docs/PHASE0.md)**.

## Tools

Project-specific tools live in `tools/`; everything general lives upstream in
[pcrecomp](https://github.com/sp00nznet/pcrecomp).

```bash
# Recover the original source-file map and C++ symbol names from assert strings
py -3.11 tools/mine_symbols.py analysis/nocturne.exe analysis/symbols.json

# Attach those names back to code via IDA string xrefs (needs IDA + idalib)
py -3.11 tools/ida_name_from_asserts.py analysis/nocturne.exe analysis/named.json

# Function catalog + bounds (pcrecomp)
py -3.11 ../tools/tools/ida/ida_funcs.py analysis/nocturne.exe analysis/ida_funcs.json
```

## Building

You provide your own legally-obtained copy of Nocturne. **This repo ships no game
code and no game assets** — not the executable, not the renderer DLLs, not the
POD archives. Drop your `nocturne.exe` into `analysis/` and run the tools above;
everything in `analysis/` is gitignored for exactly this reason.

There is nothing to build yet. Phase 5 is where that changes.

## Known costs

Honest accounting of what makes this hard:

* **42 MB of BSS.** The engine's pools are static, not heap. The lifted image
  needs a real reserved static region, not a `malloc` at startup.
* **Watcom CRT.** 673 FLIRT-identified library functions. These get shimmed to
  the host CRT rather than lifted, and that shim boundary is the one place
  Watcom's register calling convention can still bite.
* **5,494 functions.** 2.6× Fury³, a little larger than Hellbender. The pipeline
  is proven at this scale but it is not a weekend.
* **Watcom PE quirk.** Every section header has `VirtualSize = 0` with the real
  size in `SizeOfRawData`. Tooling that walks sections must handle it — pcrecomp's
  `pe_analyze.py` currently reports a zero-length code range on this binary.

## License

Tooling and original code in this repo: MIT. _Nocturne_ and all of its assets are
the property of their respective owners; none are included here. This is a
preservation and interoperability project.

---

*Part of [pcrecomp](https://github.com/sp00nznet/pcrecomp) — "everything old is new again."*
