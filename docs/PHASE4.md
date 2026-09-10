# Phase 4 — Shim layer, BSS image, and first execution

The recompiled binary links, runs, maps its own 42 MB image, and executes lifted
Watcom CRT startup code out through the import bridges.

```
[runtime] mapped nocturne.exe at 0x00400000, 42.0 MB
[runtime] stack 0x02E00000-0x03200000
[runtime] 5546 lifted functions, 171 import bridges
[import-stub] GetModuleHandleA
[import-stub] VirtualQuery
[import-stub] GetCurrentThreadId
[import-stub] GetStdHandle
[import-stub] CreateEventA
[import-stub] VirtualAlloc
```

That sequence is the Watcom C/C++32 runtime initialising, in order, through six
different bridges. It proves the whole chain end to end: image mapped at its real
VA → IAT slots installed → lifted `call dword ptr [slot]` → `RECOMP_ICALL` →
import lookup → shim → stdcall-correct return.

It stops there because `VirtualAlloc` is still a stub returning 0, so the CRT
gets no heap. Real shim bodies are bring-up work (Phase 7).

## Import arg counts are derived, not typed

Every shim must pop exactly the argument slots the real API pops. Get one wrong
and the simulated stack silently desynchronises — every value read after that
call is garbage, and the symptom surfaces nowhere near the cause.

The sibling projects keep those counts in a hand-typed table of a few hundred
entries. That is a lot of chances to mistype a number that fails silently, and
the table has in fact drifted: it gives `waveOutOpen` **7** arguments. The real
one takes **6**.

So `gen_imports.py` does not type them. The Windows SDK's 32-bit import libraries
already carry the answer — stdcall exports are decorated `_Name@N`, where N is
the argument byte count, emitted by the compiler from the real header. We read
the counts straight out of `kernel32.lib`, `user32.lib`, and friends:

```
_CreateFileA@28   -> 7 args
_CreateWindowExA@48 -> 12 args
_socket@12        -> 3 args
_waveOutOpen@24   -> 6 args
```

All **171 imports across 8 DLLs** resolved this way, with zero manual entries.
DirectSound is imported by ordinal, so those two are resolved against the system
DLL's export table first (`ordinal 1` → `DirectSoundCreate`) and then looked up
by name. The generator **refuses to emit anything it could not derive** rather
than defaulting — a guessed count is exactly the failure this design avoids.

## The 42 MB BSS

It is not an allocation. The shared loader maps the original PE's sections at
their real VAs and zeroes the image, so `.bss` is simply part of that mapping and
every `MEM32(va)` in lifted code is a direct read of the program's own data.

This is where Watcom bit again, and this one was dangerous.

`image_loader.c` decided whether a section was uninitialized by testing
`SizeOfRawData == 0`. MSVC sets both `SizeOfRawData` and `PointerToRawData` to 0
for `.bss`, so the test worked by coincidence. Watcom does not: it puts the
*memory* size in `SizeOfRawData` and leaves `PointerToRawData` at 0. So on
Nocturne the loader treated a 42 MB `.bss` as initialized and copied
`SizeOfRawData` bytes **from file offset 0** — a 42 MB read out of a 1.9 MB
buffer, over a region that is supposed to stay zeroed.

Fixed upstream: the test is now `PointerToRawData == 0`, which is what "no file
backing" actually means in PE, and the copy length is clamped to both the bytes
the file really holds and the mapped span, so a truncated or hostile header
cannot turn into an overread. The successful 42 MB map above is that fix's check.

## Alternate entry points, found automatically

The first link attempt failed with four unresolved externals out of 5,546
functions. All four turned out to be *inside* existing function bodies:

```
0x0040B1A0  inside sub_40B150 [0x0040B150,0x0040B1A1)  +80
0x0042D150  inside sub_42D130 [0x0042D130,0x0042D176)  +32
0x0042D170  inside sub_42D130 [0x0042D130,0x0042D176)  +64
0x0056E6C4  inside sub_56E6B8 [0x0056E6B8,0x0056E6E2)  +12
```

A direct `call` to a mid-body address is an alternate entry point — IDA merged
what were several small routines into one function. The CPU will execute from
there, so it needs its own lifted body.

The driver had a `FORCE_RECOVER` list for exactly this, hand-maintained. Adding
four magic addresses to it would have worked and taught us nothing, so instead
the recovery scan now detects the whole class: a direct `call` target that is
covered by a known body but is not that body's entry gets recovered
automatically. Only `call` counts — a `jmp` into a covered range is ordinary
intra-function control flow. `FORCE_RECOVER` stays empty.

Recovered functions went 48 → 52, and the link succeeded.

## Build

```bash
py -3.11 run_lift.py analysis/nocturne.exe src/recomp/gen   # 5,546 functions
py -3.11 gen_imports.py                                     # 171 bridges
cmake -S . -B build -G Ninja && cmake --build build
```

CMake requires an x64 host: the target's whole range (image at `0x00400000` plus
42 MB, stack above it) then sits far below anything the host occupies, and the
1:1 mapping holds with `g_mem_base` at 0.

The stack is placed immediately *above* the mapped image, at a base derived from
the actual span. Below does not fit — only ~4 MB of address space exists under
`0x00400000`, and the obvious 4 MB stack based at `0x00100000` runs to
`0x00500000`, straight into the image. That collision was the first run's
`ERROR_INVALID_ADDRESS`.

## Status

| | |
|---|---|
| Import bridges | **171 / 171** derived, 0 manual |
| Functions linked | 5,546 |
| Executable | links clean, 20.2 MB release / 24.3 MB debug |
| Image mapped | 42.0 MB at `0x00400000` |
| Executes | Watcom CRT startup, 6 bridges deep |
| Shim bodies | 0 — all stubs; this is Phase 7 |
