# Phase 7 — Bring-up

Where execution actually gets to, what stopped it, and what it took to find out.

## Where it gets to

All of Watcom CRT startup, then `WinMain`, a window class and a window —
**34,987 indirect calls and a clean exit**. The full log is at the bottom of this
document; the short version is that startup is no longer the problem.

Three faults got it there. The third turned out to be the cause of the first, and
it was a lifting bug rather than anything to do with Nocturne.

## The crash reporter earned its keep immediately

Lifted code faulting in a host debugger shows you `sub_004A0550`, somewhere
inside a 900,000-line generated file. True, and useless. `recomp_install_crash_handler()`
(now upstream in pcrecomp) reports the *simulated* machine instead:

```
=== recomp: CRASH (access violation) ===
  bad read of 0000000000000000 (null page)
=== recomp: state at fault ===
  current lifted function: sub_0056EED8
  eax=00000000 ecx=00000893 edx=03200000 ebx=00000000
  esp=031FFDD4 ebp=00000000 esi=00000000 edi=00000000
```

Two faults, each diagnosed from one run.

## Fault 1 — the marker leak (FIXED, same cause as fault 3)

The first fault was a write to `0xDEAD0000`, with `esi` holding the same value.
That is `RECOMP_RETADDR`, the dummy return address `RECOMP_CALL` pushes — so a
stack imbalance had leaked it into a value. Building with the entry tracer
(`-DRECOMP_TRACE`) gave the call path, and the faulting function disassembles to:

```
0056E9D0  push ebx / push esi / sub esp,0x20
0056E9D5  mov  ebx, [esp+0x2c]      ; arg0 -- an out-pointer
0056E9E5  call dword ptr cs:[0x5755dc]   ; VirtualQuery(addr, &mbi, 0x1c)
0056EA27  mov  [ebx], eax           ; *arg0 = stack top
0056EA29  mov  esi, [esp+0x30]      ; arg1 -- a second out-pointer
0056EA2D  test esi, esi
0056EA2F  je   0056EA33             ; ...which it null-checks
0056EA31  mov  [esi], edx           ; <-- faulted
```

This is Watcom's stack-limit routine, and `arg1` read back as the marker: the
read ran past the arguments the caller actually pushed.

At the time this looked like a caller/callee argument-count mismatch. It was not
— it is fault 3, four bytes of drift from an earlier call, and both are fixed by
the same change. Setting `RECOMP_RETADDR` to 0 made it inert enough to keep
moving (the routine null-checks its out-pointers), which was the right call to
keep bring-up going but bought a wrong diagnosis with it.

## Fault 2 — no FS segment, no TEB (FIXED)

Execution then reaches `sub_0056EED8`:

```
0056EEDD  call dword ptr [0x5c1abc]  ; -> per-thread CRT data
0056EEE3  mov  [eax+0x54], ebx
0056EEE8  mov  eax, dword ptr fs:[eax]   ; <-- fs:[0]: head of the SEH chain
0056EEF6  mov  [eax], ebx
0056EF13  mov  dword ptr fs:[edx], eax   ; fs:[0] = our new handler
```

This is the CRT installing its structured exception handler by linking a record
into the `fs:[0]` chain. The runtime has `g_fs_base = 0` and no TEB, so `fs:[0]`
resolves to address 0 and reads the null page.

The lifter already knew `fs:` was thread-relative — it emits `FS_BASE + offset`
for every `fs:` access rather than treating the segment as flat, and there are
only **three `fs:` sites in the whole binary**. The runtime simply never set the
base. `shims_make_tib()` now allocates a TIB-shaped block in the arena and
`g_fs_base` points at it, with `fs:[0]` seeded to `0xFFFFFFFF` (the real
end-of-chain sentinel) and StackBase/StackLimit filled in.

Confirmed working: the register dump at the next fault shows `ebx=FFFFFFFF`,
which is that sentinel, read back through `fs:[0]`. No re-lift was needed.

We never dispatch SEH — exceptions go to the host handler — so the chain only
ever needs to be writable memory that looks right.

## Fault 3 — falling off the end of a function is not returning (FIXED)

This one was the root cause of fault 1 as well, and it took three wrong guesses
to get to. Recording the wrong turns, because the wrong turns are the lesson.

### What it looked like

`sub_0056EED8` faulted writing through an argument that arrived as 0. Working
backwards: the argument came from `[esp+8]` in `sub_00567458`, which came from
`lea eax,[ebp-8]` in `sub_0056DF10` — a perfectly valid stack address. Every
static check passed. The heap, which three hypotheses had blamed, turned out to
be *fine*: adding a crash-time dump of the CRT's heap globals showed the block
allocated and linked and the per-thread pointer set.

```
[005C1680] heap free-list    = 03210000
[02DE4E3C] per-thread block  = 031FFDF8
```

### What it was

The per-thread-block accessor lifted to this:

```c
eax = MEM32(0x2DE4E3C);   /* mov eax, [0x2de4e3c] */
eax = eax;                /* lea eax, [eax]       */
return; /* end of function */
```

No `ret`. IDA catalogues `sub_005671DC` as `[0x005671DC, 0x005671E4)` — the
`ret` at `0x005671E4` is its own function, because it doubles as the no-op
single-threaded heap lock (`[0x5C1AD8]` and `[0x5C1AE0]` both point at it). So
the accessor *falls through* into it.

The lift driver ended any body that did not end in `ret` with a bare `return;`.
That silently drops the `ret`, so the dummy return address `RECOMP_CALL` pushed
is never popped: **four bytes of simulated stack leaked per call.** The accessor
is called constantly, and in `sub_00567458` the drift lands exactly here:

```
0x567477  call [0x5c1abc]   ; esp drifts -4, never restored
0x567483  add esp, 8
0x567486  mov ebx, [esp+8]  ; reads 4 bytes off -> the dummy return address
0x56748B  call 0x56eed8     ; passes it as an out-pointer
```

With `RECOMP_RETADDR` at its default that argument was `0xDEAD0000` — fault 1.
With the mitigation setting it to 0 it became a null write — fault 3. One bug.

### The fix

A body whose last instruction is neither `ret` nor an unconditional `jmp` now
tail-calls its fall-through address instead of returning, so the next function's
`ret` does the pop. **272 sites in this binary.**

The companion gap surfaced immediately as `ITAIL: unresolved VA`: those
fall-through targets have to be dispatchable, and a backward `jmp` into a
*different* function's body is an alternate entry too — the recovery scan had
been treating every jump into a covered range as ordinary control flow, which is
only true within the same function. Both are fixed upstream in
[pcrecomp](https://github.com/sp00nznet/pcrecomp) `e0f2d84`, with self-test
coverage including the negative case.

Recovered entries went 52 → **297**; total lifted functions 5,546 → **5,791**.

### Three hypotheses, tested and wrong

Recording these so the next person does not spend the cycles again. All three
were about the heap, and the heap was never the problem:

1. **`GlobalMemoryStatus` reporting a machine with no memory.** A stub returning
   0 leaves a zeroed `MEMORYSTATUS`. Implemented it properly — no change, and
   the trace shows the CRT never calls it on this path.
2. **Allocation granularity.** `VirtualAlloc(NULL, …)` is documented to return
   64 KB-aligned memory and heap managers of this era mask the low 16 bits to
   find a block header. Fixed — no change.
3. **Segment pushes miscounted.** `push es/fs/gs` are 4 bytes each in 32-bit
   mode and an argument read depended on it. The lifter already emitted
   `PUSH32` correctly.

Fixes 1 and 2 are right on their own terms and stay in. The lesson: three
build-and-run cycles against guesses produced nothing, and one crash-time dump
of the actual state produced the answer. That is what
`recomp_set_extra_reporter()` upstream is now for.

## It runs

```
[runtime] mapped nocturne.exe at 0x00400000, 42.0 MB
[shims] heap arena 0x03200000-0x23200000
[shims] TIB 0x03200000
[runtime] 5791 lifted functions, 171 import bridges
[shims] VirtualAlloc(addr=00000000, size=65536, type=00001000) -> 03210000
[import-stub] SetUnhandledExceptionFilter
[import-stub] GetCPInfo
[import-stub] CharUpperBuffA
[import-stub] FindWindowA          <- single-instance check
[import-stub] timeGetTime
[import-stub] GetCurrentDirectoryA
[import-stub] SetCurrentDirectoryA
[import-stub] LoadIconA
[import-stub] LoadCursorA
[import-stub] GetStockObject
[import-stub] RegisterClassA       <- window class
[import-stub] CreateWindowExA      <- window
[import-stub] SetFilePointer
[import-stub] GetFileType
[import-stub] ExitProcess
[runtime] entry returned; 34987 indirect calls
```

**Exit code 0.** The whole Watcom CRT startup, locale initialisation, the
single-instance check, working-directory setup, `WinMain`, window class
registration and window creation — 34,987 indirect calls, no crash.

It gets no further only because every shim on that path returns failure. Two
indirect calls remain unresolved (`0x005671C6`, `0x004CEC00`), both to addresses
IDA never catalogued as code at all, reached through function-pointer tables.
Neither is fatal.

## Shims so far

32 of 171 imports have real bodies; the other 139 log once and return 0.

| Group | What |
|---|---|
| Memory | `VirtualAlloc`, `VirtualFree`, `VirtualQuery` over one reserved 512 MB arena above the stack, committed per allocation |
| Identity | `GetModuleHandleA`, `GetVersion`, `GetCurrent{Thread,Process}{,Id}`, `GetStdHandle` |
| Program | `GetCommandLineA/W`, `GetModuleFileNameA/W`, `GetEnvironmentStrings` — what the CRT builds `argv` and `environ` from |
| TLS | `TlsAlloc/Free/Get/SetValue`, 64 flat slots |
| Sync | events, mutexes, critical sections, `WaitForSingleObject` — all no-ops while everything is one thread |

The heap is a bump allocator; `VirtualFree` does not reclaim. The original asks
the OS for a few large blocks and sub-allocates them itself, so churn is low.

## The counts stay derived

`gen_imports.py` derives all 171 stdcall argument counts from the SDK import
libraries — and then verifies, on every run, that each hand-written shim's
`STDRET` matches the derived count:

```
  32 hand-written shims verified against derived counts
```

Deriving 171 numbers and then hand-typing them back into the shim bodies would
put the mistake straight back where it was taken from. A wrong `STDRET` produces
exactly the symptom above: a desynchronised stack and a marker leaking into a
register, crashing somewhere unrelated. Worth having the machine check.

## Next

1. **File I/O shims.** `CreateFileA` / `ReadFile` / `SetFilePointer` /
   `GetFileType` backed by the real game directory. That is what stands between
   here and mounting a POD — and `tools/pod.py` can already say exactly what the
   engine should find inside one, which makes the first mount self-checking.
2. **The window.** `RegisterClassA` / `CreateWindowExA` currently return 0. A
   real window plus a message pump gets the engine into its main loop.
3. **The second marker leak.** Building with `-DRECOMP_RETADDR=0xDEAD0000u`
   still faults in `sub_0056DD80`, past window creation, with several registers
   holding the marker. The default is back to 0 so bring-up can continue; turn
   the poison on to hunt it.
4. Then the renderer: 37 `APIDLL*` calls, testable against real textures well
   before the engine can ask for them.
