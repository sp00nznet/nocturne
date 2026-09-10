# Phase 0 — Reconnaissance

Everything below was derived from a retail `nocturne.exe` (v1.01, 1999-11-02,
1,948,160 bytes). No game code or data is committed to this repo.

## The binary

| | |
|---|---|
| Format | PE32, i386, Windows GUI |
| Image base / entry | `0x00400000` / `0x00567152` |
| Compiler | **Watcom C/C++32** (linker 2.18) — `WATCOM C/C++32 Run-Time system` in `.data` |
| Sections | `AUTO` (code), `.idata`, `DGROUP` (data), `.bss`, `.reloc`, `.rsrc` |
| Code | 1.46 MB across **5,494 functions** (673 FLIRT library, 190 thunks, 39 no-return) |
| BSS | **42 MB** — the engine's static pools live here, not on the heap |
| Imports | **171 functions from 8 DLLs** |

### Watcom PE quirk

Watcom writes `VirtualSize = 0` in every section header and puts the real size in
`SizeOfRawData`. Naive PE readers (including pcrecomp's `pe_analyze.py` as it
stands) compute a zero-length code range from it. Anything that walks sections on
this binary must fall back to `SizeOfRawData` when `VirtualSize` is 0.

### Calling convention — the good news

Watcom's *default* is the register convention (`__watcall`: args in EAX/EDX/EBX/ECX),
which would be a genuine problem for a stack-oriented lifter. Nocturne was not
built that way. Sampling 4,400 call sites in the first 1,200 non-library functions:

| | |
|---|---|
| calls with stack (`push`) arguments | 4,225 |
| calls with register arguments | 54 |
| `ret` forms observed | `ret` only — never `ret N` |

Bare `ret` everywhere means the **caller** cleans the stack: this is cdecl. The
pcrecomp `lift32` pipeline's assumptions hold unchanged for the app body. Watcom
register conventions may still appear inside the 673 FLIRT-identified CRT
routines, but those are the ones we shim rather than lift, so the exposure is
bounded to the shim boundary.

## Imports (171 / 8 DLLs)

| DLL | Count | Notes |
|---|---:|---|
| KERNEL32 | 89 | files, memory, threads, critical sections |
| USER32 | 30 | window + message loop |
| GDI32 | 14 | `CreateDIBSection`, fonts, text extents — 2D/menu path |
| ADVAPI32 | 5 | registry config |
| DSOUND | 2 | by ordinal (`DirectSoundCreate`) |
| DDRAW | 1 | `DirectDrawCreate` only |
| + 2 more | | |

The OS surface is *small*. Notably there is **no Direct3D import in the exe at
all** — see below.

## The renderer is a DLL, and that is the whole story

`SYSTEM/nocturne.ini` contains:

```ini
rendererDLLPath=tridx7.dll
```

The exe `LoadLibrary`s its rasteriser at runtime. Four ship with the game —
`tridx7.dll` (DirectX 7), `tridx6.dll` (DirectX 6), `trid3d.dll` (generic D3D),
`tri3dfx.dll` (Glide/Voodoo) — and **all four export the identical 37-function C
API**:

```
APIDLLinit            APIDLLsetVideoMode      APIDLLsetVideoMode2   APIDLLtoggle
APIDLLbeginScene      APIDLLendScene          APIDLLclear           APIDLLsync
APIDLLlockFrame       APIDLLunlockFrame       APIDLLlockHoldBuffer  APIDLLunlockHoldBuffer
APIDLLdrawPolygon     APIDLLdrawPolygon2      APIDLLdrawPolyList    APIDLLdrawPolyList2
APIDLLselectTexture   APIDLLupdateTexture     APIDLLsetMipMapLevel  APIDLLsetColorTable16
APIDLLclearZBuffer    APIDLLclearZBox         APIDLLmasterZBuffer   APIDLLrestoreZBuffer
APIDLLaddParticle     APIDLLflushParticleList APIDLLadd3dLine       APIDLLflushLineList
APIDLLsetFogColor     APIDLLgetVideoMemory    APIDLLbuildCardList   APIDLLselectCard
APIDLLGetDisplayContext  APIDLLReleaseDisplayContext
APIDLLInformation     APIDLLrestoreVideoMode  APIDLLkill
```

This is the single most valuable fact in this document. Terminal Reality drew a
hard abstraction line between the game and the GPU in 1999, and it is still
there. We do not have to reverse-engineer a Direct3D 7 pipeline out of the game's
guts — the guts already speak a 37-call vendor-neutral rasteriser interface, and
a modern backend is an implementation of those 37 functions. Because all four
shipped DLLs agree on the interface, we also have four independent
implementations to read it out of, and one of them (`tri3dfx.dll`) targets
hardware that has nothing to do with Direct3D, which is a strong signal that the
interface carries no D3D-shaped assumptions.

## Assert strings are a partial symbol table

The retail build shipped with `assert()` live. Every assert site left two
literals in `DGROUP`: the `__FILE__` path and the message — and Terminal
Reality's house style for assert messages was `CClass::method - what went wrong`:

```
..\core\actor.cpp    CDemonActor::customRayIntersect should not be called for this base class
..\engine\pod.cpp    CPodFile::getAuditRecord - invalid index.  Pod not mounted?
..\core\platfrm.cpp  CPlatform::attachActor - too many!
```

Mined with `tools/mine_symbols.py`:

* **99 original source files** across 7 directories
  (`core` 71, `engine` 16, `cockpit` 4, `sound` 3, `support` 3, `shape` 1, `wincore` 1)
* **88 classes**, **266 `Class::method` names**
* 87 of the 99 files are `.cpp`, 12 are `.c` — a C++ codebase built with Watcom C++

The file list reads like a table of contents for the engine: `engine\pod.cpp`
(archive reader), `engine\drender.cpp` (the renderer bridge), `engine\keyframe.cpp`,
`engine\model.cpp`, `engine\texture.cpp`, `engine\clipper.cpp`, `sound\mp3.cpp`,
`sound\snddx.cpp`, `core\main.cpp`, `core\game.cpp`, `core\script.cpp`,
`core\hero.cpp`, `core\werewolf.cpp`, `core\vampboss.cpp`, `core\zombie.cpp`,
`shape\edittool.cpp` (the level editor is still linked in).

`tools/ida_name_from_asserts.py` walks the string xrefs and attaches those names
back to code:

* **253 functions given their real names**
* **522 functions attributed to an original source file**

For a binary with zero symbols, starting Phase 3 with 253 real names and a
source-file map for 522 functions is an unusually strong position.

## Assets

Game data is in **POD archives** — Terminal Reality's own format, the same family
already handled in the Fury³ and Hellbender projects. 40-odd `.POD` files
(`chicago.pod`, `mansion.pod`, `forest.pod`, `hero.pod`, `enemy.pod`, `sound.pod`,
…) totalling ~1.4 GB. `engine\pod.cpp` in the file map is the reader, and its
assert strings name `CPodFile` methods including CRC verification
(`computeOneFileCRC`, `getAuditRecord`), so the format's integrity layer is
documented for us by the binary itself.

## What this means for the plan

Three things make Nocturne a better static-recompilation target than its size
suggests:

1. **The GPU seam already exists.** 37 exports, four reference implementations.
2. **cdecl, not `__watcall`.** The existing lifter applies without convention work.
3. **The binary tells us what its own functions are called.** 253 names and 99
   source files recovered before writing a line of C.

The costs are the 42 MB BSS (needs a real static image, not a heap allocation),
the Watcom CRT (673 library functions to shim rather than lift), and the sheer
function count (5,494 — 2.6× Fury³, roughly Hellbender-scale).
