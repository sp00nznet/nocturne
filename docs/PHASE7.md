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

## Fault 2 — no FS segment, no TEB (open)

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

The fix is small and well understood: allocate a TEB-shaped block in the arena
and point `g_fs_base` at it. The SEH chain then becomes a linked list in
simulated memory that nobody walks, since we do not dispatch SEH — the CRT is
satisfied, and exceptions keep going to the host handler.

Related and worth resolving at the same time: the accessor behind
`[0x5c1abc]` is reached at `0x005671E4`, which disassembles to a bare `ret` —
the last byte of the function that starts at `0x005671DC` (`mov eax,[mem]; lea
eax,[eax]; ret`). A call landing on a function's final `ret` returns without
setting `eax`, which is why `eax` is 0 above. Whether the CRT genuinely
initialises that pointer to a no-op stub, or the pointer is being read before it
is written, is the open question.

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

1. Give `fs:` a TEB. Unblocks SEH installation.
2. Find the argument-count mismatch behind the marker leak, and put
   `RECOMP_RETADDR` back to a poisoned value.
3. Keep walking: file I/O shims, then `WinMain`, then the POD mount that
   `tools/pod.py` can already check the engine's answers against.
