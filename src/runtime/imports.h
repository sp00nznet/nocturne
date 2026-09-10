/*
 * Nocturne Recompilation - import bridge helpers.
 *
 * Shims run inside the recomp register/stack model, not the host's. When lifted
 * code calls an import, RECOMP_ICALL has already pushed a dummy return address,
 * so from ESP the simulated stack reads: [ret][arg0][arg1]... one 32-bit slot
 * each.
 *
 *   ARG(n)     read argument n (0-based)
 *   ARGP(n,T)  read argument n as a host pointer of type T* (maps VA -> host)
 *   RET(v)     set the return value (EAX)
 *   STDRET(k)  stdcall epilogue: pop the dummy ret addr plus k argument slots
 *
 * STDRET's count has to be exactly right. Pop too few or too many and the
 * simulated stack silently desynchronises, so every value read after that call
 * is wrong and the symptom appears nowhere near the cause. That is why
 * gen_imports.py derives the counts from the SDK import libraries' stdcall
 * decoration instead of anyone typing them.
 */
#ifndef NOCTURNE_IMPORTS_H
#define NOCTURNE_IMPORTS_H

#include <stdio.h>
#include "recomp_types.h"

#define ARG(n)      MEM32(g_esp + 4 + (n)*4)
#define ARGP(n, T)  ((T*)(void*)(uintptr_t)ADDR(ARG(n)))
#define RET(v)      do { g_eax = (uint32_t)(v); } while (0)
#define STDRET(k)   do { g_esp += 4 + (uint32_t)(k)*4; } while (0)

/* Log an unimplemented import once, by name. */
#define IMPORT_STUB(nm) do {                          \
    static int _warned = 0;                           \
    if (!_warned) {                                   \
        fprintf(stderr, "[import-stub] %s\n", (nm));  \
        _warned = 1;                                  \
    }                                                 \
} while (0)

/* Generated bridge tables (imports_gen.c). */
extern const recomp_dispatch_entry_t nocturne_import_bridges[];
extern const uint32_t nocturne_import_bridge_count;
extern const uint32_t nocturne_iat_slots[];
extern const uint32_t nocturne_iat_slot_count;

/* Point every IAT slot at itself, so that a lifted `call dword ptr [slot]`
 * -- which lifts to RECOMP_ICALL(MEM32(slot)) -- yields the slot's own VA and
 * recomp_lookup_import can find its bridge. The file's original slot contents
 * are RVAs into the hint/name table and resolve to nothing. */
void nocturne_install_iat(void);

#endif /* NOCTURNE_IMPORTS_H */
