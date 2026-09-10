# Phase 7 — Bring-up

Where execution actually gets to, what stopped it, and what it took to find out.

## Where it gets to

The Watcom CRT now runs a long way. It takes a heap, discovers its stack bounds,
builds `argv` and `environ`, and reaches the point of installing its structured
exception handler — hundreds of lifted functions deep, through 32 real shims.

```
[runtime] mapped nocturne.exe at 0x00400000, 42.0 MB
[runtime] stack 0x02E00000-0x03200000
[shims] heap arena 0x03200000-0x23200000
[runtime] 5546 lifted functions, 171 import bridges
```

No `[import-stub]` lines at all now: everything the CRT asks for on this path has
a real body.

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

## Fault 1 — the marker leak (mitigated)

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
read ran past the arguments the caller actually pushed. The caller/callee
argument-count mismatch behind that is **still open**.

`recomp_types.h` documents the mitigation for exactly this case — set
`RECOMP_RETADDR` to 0 so a leaked marker is inert. Because the routine
null-checks both out-pointers, a leaked 0 makes it skip the store rather than
fault, and bring-up continues. The build sets it; it is a mitigation, not a fix,
and the imbalance is the first thing to chase next.

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

## Fault 3 — the CRT's malloc returns NULL (open)

Execution stops at the same instruction, but for a different reason now. The
chain is fully traced:

```
00567227  mov [0x02DE4E3C], eax    ; store the per-thread CRT block
0056720C  ...  call sub_0056E56C   ; which allocates it
0056E57E    call sub_00565C50      ; calloc(1, [0x5C20CC] = 244)
00565C5B      call sub_005635B0    ; -> malloc(244)
005635C0        (270 bytes of heap bookkeeping)
```

`0x02DE4E3C` lives in `.bss`, so it starts as 0. Its single writer is
`0x00567227`, and the trace confirms that instruction's function *did* run — so
the store happened and stored zero. `malloc(244)` is failing, and every later
`fs:[0]` user reads a null per-thread block.

The allocation itself is not the problem: the shim log shows the CRT's one
request succeeding.

```
[shims] VirtualAlloc(addr=00000000, size=65536, type=00001000) -> 03210000
```

64 KB, `MEM_COMMIT`, satisfied. So the failure is inside the CRT's own heap
bookkeeping over that block.

### Three hypotheses, tested and wrong

Recording these so the next person does not spend the cycles again:

1. **`GlobalMemoryStatus` reporting a machine with no memory.** A stub returning
   0 leaves a zeroed `MEMORYSTATUS`, which would be a good reason for a heap to
   refuse. Implemented it properly — no change, and the trace shows the CRT never
   calls it on this path.
2. **Allocation granularity.** `virt_alloc` returned page-aligned addresses,
   but Win32 guarantees `VirtualAlloc(NULL, …)` is aligned to the 64 KB
   allocation granularity, and heap managers of this era routinely find a block
   header by masking the low 16 bits. Fixed — the address is now `0x03210000` —
   no change.
3. **Segment pushes miscounted.** `sub_005635C0`'s prologue does
   `push es/fs/gs`, which are 4 bytes each in 32-bit mode, and its argument read
   at `[esp+0x2c]` depends on that. If the lifter modelled them as 2 bytes the
   size argument would be garbage. It does not: it emits
   `PUSH32(esp, _seg_es)`. Correct already.

Fixes 1 and 2 are right on their own terms and stay in. Neither was the blocker.

### Next

Stop guessing and instrument. `sub_005635C0` is 270 bytes of free-list walking;
the useful next step is a runtime probe on its internals — or on the heap
globals it reads — rather than another rebuild against a hypothesis. Worth
checking early: whether the block returned by `VirtualAlloc` is ever recorded in
the CRT's heap root at all, which is a single global to watch.

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

1. Instrument `sub_005635C0` and find why `malloc(244)` returns NULL.
2. Find the argument-count mismatch behind the marker leak, and put
   `RECOMP_RETADDR` back to a poisoned value.
3. Keep walking: file I/O shims, then `WinMain`, then the POD mount that
   `tools/pod.py` can already check the engine's answers against.
