# Phase 3 — Lift to C

The whole binary is now C, and the C compiles.

| | |
|---|---|
| Functions lifted | **5,542** (5,494 from IDA + 48 recovered) |
| Lift errors | **0** |
| Output | 885,836 lines across 14 chunks, 41.6 MB |
| Lift time | ~9 s |
| Compile | **0 errors, 0 warnings**, 15 objects, 37.5 MB |

```bash
py -3.11 run_lift.py analysis/nocturne.exe src/recomp/gen
```

`run_lift.py` is the Fury³/Hellbender driver retargeted. Two things needed
changing, both because Nocturne is a Watcom build: the code section is `AUTO`
rather than `.text` (the driver now selects on the `is_code` characteristic
instead of the name, which is target-independent), and `VirtualSize` is 0 in
every section header, so the code slice goes through `Section.effective_size`.

48 functions were recovered that IDA missed — reachable only through jmp-thunk
chains and tail calls, with no direct `call` and no standard prologue. Left out,
they surface at runtime as unresolved indirect tail calls and stall the game.

## Three bugs in the shared toolchain

None of these were Nocturne-specific. All three are fixed upstream in
[pcrecomp](https://github.com/sp00nznet/pcrecomp), so every project on the
toolchain gets them.

### 1. The lifter/driver locals contract was written down nowhere

The lifter emits statements, not function bodies, so it cannot declare the
per-function scratch its own output references — each project driver wrote the
preamble by hand from a copied list. When the lifter started emitting `_flag_k`,
every driver's copy silently went stale, and the first Nocturne lift produced
5,542 functions that all failed to compile on the same undeclared identifier.

Fixed by putting the contract where the knowledge actually lives:
`lift32.FUNCTION_LOCALS`. Drivers now ask instead of remembering.

### 2. The MMX register file had no alias

`recomp_types.h` declares the register file as `g_mm[8]` and exposes it to lifted
code through `#define _mm g_mm`, the same way `g_st` becomes `_st`. The alias
line was never added when MMX support landed, so the declaration existed and was
simply unreachable — 213 MMX bodies failed to compile against a global that was
right there. One line.

Nocturne needed this because it is an MMX build; the earlier targets on this
toolchain never exercised the path.

### 3. `shl` computed the carry flag at the wrong width

The bit a left shift pushes into CF is bit *(width − count)* of the original
value. The lifter hardcoded width 32:

```c
if (1) _cf = (((LO16(eax)) >> (32 - (1))) & 1u);   /* shl ax, 1 */
```

On a 16-bit operand that shifts a 16-bit value right by 31 — so **CF came out 0
for every narrow left shift, always**, and any `shl`/`jc` pair downstream took
the wrong branch. It is a silent wrong-answer bug: the code compiles and runs,
it just decides incorrectly. MSVC's `C4333` is what surfaced it, on exactly one
line out of 885,836.

The same hardcoded width was wrong in three sibling places (`shld`'s carry, and
the bits `shld`/`shrd` pull in from their source operand). Patching only the line
the compiler complained about would have left the other three. All four now take
the width from the destination operand via a new `op_bits()` helper.

Blast radius in this binary is small and worth stating precisely: **two sites**,
one 16-bit and one 8-bit. The value is that it was found at all — the other
projects on this toolchain have been carrying it.

## What "it compiles" does and doesn't mean

It means 5,542 function bodies are syntactically valid C that a real compiler
accepts, with no unresolved identifiers against the runtime header. That is a
genuine milestone and it is not the same as correct: nothing has been linked,
no import bridge exists yet, the 42 MB BSS has no home, and not one instruction
has executed. Phases 4 and 5 are where those get answered.

The compile is currently driven by hand:

```bash
cl /nologo /c /W1 /I src\recomp\gen src\recomp\gen\*.c
```

A CMake build replaces that in Phase 5.
