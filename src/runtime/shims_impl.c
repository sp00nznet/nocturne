/*
 * Nocturne Recompilation - hand-written import shims.
 *
 * Everything gen_imports.py does not hand-write is a stub that logs once and
 * returns 0. That is fine until the caller needs a real answer, and the Watcom
 * CRT needs real answers almost immediately: it asks for a heap before it will
 * do anything else at all.
 *
 * Shims run inside the recomp register/stack model, so arguments come off the
 * simulated stack with ARG(n) and the epilogue is STDRET(argc) -- see imports.h.
 * A shim that hands back a pointer must hand back a *simulated* VA, never a host
 * pointer: lifted code will do arithmetic on it and store it in the 32-bit image.
 */
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "recomp_types.h"
#include "imports.h"

/* ============================================================
 * The simulated heap
 *
 * One arena in the 32-bit VA space, above the stack. Address space is reserved
 * up front so the arena can never be split by something else, and pages are
 * committed per allocation so we are not charging the pagefile for 512 MB the
 * game will not touch.
 *
 * ponytail: bump allocator -- VirtualFree does not reclaim. The original asks
 * the OS for a few large blocks and sub-allocates them with its own heap, so
 * churn here is low. If a profile ever shows the arena running dry, put a real
 * free list behind virt_alloc rather than growing the arena.
 * ============================================================ */

#define ARENA_SIZE   0x20000000u   /* 512 MB of reserved address space */
#define PAGE_SZ      0x1000u

static uint32_t g_arena_base = 0;
static uint32_t g_arena_next = 0;
static uint32_t g_arena_end  = 0;

/* Round up to a page. */
static uint32_t page_up(uint32_t n) {
    return (n + PAGE_SZ - 1) & ~(PAGE_SZ - 1);
}

int shims_init(uint32_t base) {
    base = (base + 0xFFFFu) & ~0xFFFFu;         /* allocation granularity */
    if (!VirtualAlloc((void*)(uintptr_t)base, ARENA_SIZE, MEM_RESERVE,
                      PAGE_READWRITE)) {
        fprintf(stderr, "[shims] arena reserve @ 0x%08X (%u MB) failed (%lu)\n",
                base, ARENA_SIZE / 1048576u, GetLastError());
        return 0;
    }
    g_arena_base = g_arena_next = base;
    g_arena_end  = base + ARENA_SIZE;
    fprintf(stderr, "[shims] heap arena 0x%08X-0x%08X\n", g_arena_base, g_arena_end);
    return 1;
}

uint32_t shims_arena_base(void) { return g_arena_base; }
uint32_t shims_arena_end(void)  { return g_arena_end; }

static uint32_t arena_commit(uint32_t va, uint32_t size) {
    if (!VirtualAlloc((void*)(uintptr_t)va, size, MEM_COMMIT, PAGE_READWRITE))
        return 0;
    return va;
}

/* Returns a simulated VA, or 0. */
static uint32_t virt_alloc(uint32_t want_va, uint32_t size) {
    if (!size) return 0;
    size = page_up(size);

    if (want_va) {
        /* A caller asking for a specific address only gets it if it is inside
         * the arena; anywhere else it would collide with the mapped image or
         * the stack, and silently handing back a colliding block is worse than
         * failing the call. */
        if (want_va < g_arena_base || (uint64_t)want_va + size > g_arena_end)
            return 0;
        return arena_commit(want_va & ~(PAGE_SZ - 1), size);
    }

    if ((uint64_t)g_arena_next + size > g_arena_end) {
        fprintf(stderr, "[shims] heap arena exhausted (%u MB requested)\n",
                size / 1048576u);
        return 0;
    }
    uint32_t va = g_arena_next;
    if (!arena_commit(va, size)) {
        fprintf(stderr, "[shims] commit 0x%08X (%u bytes) failed (%lu)\n",
                va, size, GetLastError());
        return 0;
    }
    g_arena_next = va + size;
    return va;
}

/* ============================================================
 * Memory imports
 * ============================================================ */

void imp_VirtualAlloc(void) {
    uint32_t addr = ARG(0), size = ARG(1);
    /* ARG(2) flAllocationType, ARG(3) flProtect: the arena is RW and committed
     * on demand, so RESERVE and COMMIT collapse to the same thing here. */
    g_eax = virt_alloc(addr, size);
    STDRET(4);
}

void imp_VirtualFree(void) {
    /* ponytail: bump allocator, so this is a no-op that reports success.
     * Decommitting would hand pages back that virt_alloc will never re-issue. */
    RET(1);
    STDRET(3);
}

/* MEMORY_BASIC_INFORMATION, 32-bit layout (28 bytes). */
#define MBI_BASE      0
#define MBI_ALLOCBASE 4
#define MBI_ALLOCPROT 8
#define MBI_SIZE      12
#define MBI_STATE     16
#define MBI_PROTECT   20
#define MBI_TYPE      24
#define MBI_SIZEOF    28

/* Where a VA lives in the simulated address space. The runtime owns the image
 * and stack bounds; the arena is ours. */
uint32_t recomp_image_base(void);
uint32_t recomp_image_end(void);
uint32_t recomp_stack_base(void);
uint32_t recomp_stack_end(void);

void imp_VirtualQuery(void) {
    uint32_t addr = ARG(0), buf = ARG(1), len = ARG(2);
    if (!buf || len < MBI_SIZEOF) { RET(0); STDRET(3); return; }

    uint32_t base, size, state;
    if (addr >= recomp_image_base() && addr < recomp_image_end()) {
        base = recomp_image_base(); size = recomp_image_end() - base;
        state = MEM_COMMIT;
    } else if (addr >= recomp_stack_base() && addr < recomp_stack_end()) {
        base = recomp_stack_base(); size = recomp_stack_end() - base;
        state = MEM_COMMIT;
    } else if (addr >= g_arena_base && addr < g_arena_next) {
        base = g_arena_base; size = g_arena_next - g_arena_base;
        state = MEM_COMMIT;
    } else {
        base = addr & ~(PAGE_SZ - 1); size = PAGE_SZ;
        state = MEM_FREE;
    }

    MEM32(buf + MBI_BASE)      = addr & ~(PAGE_SZ - 1);
    MEM32(buf + MBI_ALLOCBASE) = base;
    MEM32(buf + MBI_ALLOCPROT) = PAGE_READWRITE;
    MEM32(buf + MBI_SIZE)      = size - ((addr & ~(PAGE_SZ - 1)) - base);
    MEM32(buf + MBI_STATE)     = state;
    MEM32(buf + MBI_PROTECT)   = (state == MEM_FREE) ? 0 : PAGE_READWRITE;
    MEM32(buf + MBI_TYPE)      = (state == MEM_FREE) ? 0 : MEM_PRIVATE;
    RET(MBI_SIZEOF);
    STDRET(3);
}

/* ============================================================
 * Putting host data into simulated memory
 *
 * Anything a shim hands back as a pointer has to live at a 32-bit VA the lifted
 * code can dereference, so it gets copied into the arena. These blocks are made
 * once and cached: the CRT asks for the command line repeatedly and compares the
 * pointer, so returning a fresh copy each time would be both wasteful and wrong.
 * ============================================================ */

static uint32_t sim_put(const void* src, uint32_t n) {
    uint32_t va = virt_alloc(0, n);
    if (va) memcpy((void*)ADDR(va), src, n);
    return va;
}

static uint32_t sim_put_str(const char* s) {
    return sim_put(s, (uint32_t)strlen(s) + 1);
}

static uint32_t sim_put_wstr(const char* s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t va = virt_alloc(0, n * 2);
    if (!va) return 0;
    uint16_t* w = (uint16_t*)ADDR(va);
    for (uint32_t i = 0; i < n; i++) w[i] = (uint16_t)(unsigned char)s[i];
    return va;
}

/* Copy a NUL-terminated string into a caller-supplied simulated buffer, Win32
 * style: truncate to `cap`, always terminate, return the length written. */
static uint32_t sim_copy_out(uint32_t buf, uint32_t cap, const char* s) {
    if (!buf || !cap) return 0;
    uint32_t n = (uint32_t)strlen(s);
    if (n > cap - 1) n = cap - 1;
    memcpy((void*)ADDR(buf), s, n);
    MEM8(buf + n) = 0;
    return n;
}

/* The program identity the CRT is about to build argv and __argv0 from. */
static const char* g_prog_path = "C:\\NOCTURNE\\NOCTURNE.EXE";
static const char* g_cmd_line  = "C:\\NOCTURNE\\NOCTURNE.EXE";

void shims_set_program(const char* path) {
    if (path && *path) { g_prog_path = path; g_cmd_line = path; }
}

void imp_GetCommandLineA(void) {
    static uint32_t va = 0;
    if (!va) va = sim_put_str(g_cmd_line);
    RET(va);
    STDRET(0);
}

void imp_GetCommandLineW(void) {
    static uint32_t va = 0;
    if (!va) va = sim_put_wstr(g_cmd_line);
    RET(va);
    STDRET(0);
}

void imp_GetModuleFileNameA(void) {
    /* ARG(0) is the module handle; we only have the one image. */
    RET(sim_copy_out(ARG(1), ARG(2), g_prog_path));
    STDRET(3);
}

void imp_GetModuleFileNameW(void) {
    uint32_t buf = ARG(1), cap = ARG(2);
    if (!buf || !cap) { RET(0); STDRET(3); return; }
    uint32_t n = (uint32_t)strlen(g_prog_path);
    if (n > cap - 1) n = cap - 1;
    uint16_t* w = (uint16_t*)ADDR(buf);
    for (uint32_t i = 0; i < n; i++) w[i] = (uint16_t)(unsigned char)g_prog_path[i];
    w[n] = 0;
    RET(n);
    STDRET(3);
}

/* A Win32 environment block is a run of NUL-terminated KEY=VALUE strings ending
 * in a second NUL. The CRT walks it to build `environ`, so it has to be
 * well-formed even though the engine reads almost nothing from it. */
void imp_GetEnvironmentStrings(void) {
    static uint32_t va = 0;
    if (!va) {
        static const char block[] =
            "PATH=C:\\WINDOWS;C:\\WINDOWS\\SYSTEM\0"
            "TEMP=C:\\WINDOWS\\TEMP\0"
            "WINDIR=C:\\WINDOWS\0";
        va = sim_put(block, (uint32_t)sizeof(block));  /* sizeof keeps both NULs */
    }
    RET(va);
    STDRET(0);
}

/* The block is arena memory that is never reclaimed, so there is nothing to do
 * -- but the slot still has to be popped. */
void imp_FreeEnvironmentStringsA(void) { RET(1); STDRET(1); }

/* ============================================================
 * Process / thread identity
 *
 * The CRT asks these before it will initialise. None of them need to be real;
 * they need to be *consistent* and non-zero, because the CRT stores them and
 * compares them later.
 * ============================================================ */

void imp_GetModuleHandleA(void) {
    /* A module handle on Win32 is the image base. NULL means "this program",
     * which is exactly the image we mapped. We have no other modules, so a
     * named lookup answers for the exe and nothing else. */
    RET(recomp_image_base());
    STDRET(1);
}

void imp_GetCurrentThreadId(void)  { RET(0x1000); STDRET(0); }
void imp_GetCurrentProcessId(void) { RET(0x2000); STDRET(0); }
void imp_GetCurrentThread(void)    { RET(0xFFFFFFFEu); STDRET(0); }
void imp_GetCurrentProcess(void)   { RET(0xFFFFFFFFu); STDRET(0); }

/* GetVersion: LOBYTE(LOWORD) major, HIBYTE(LOWORD) minor, HIWORD build, and the
 * top bit clear means NT. Report Windows 2000 (5.0.2195) -- one of the three
 * systems the shipping readme lists, and the newest of them, so any version
 * gate in the engine takes its most modern path. */
void imp_GetVersion(void) { RET(0x08930005u); STDRET(0); }

/* Standard handles. Distinct, non-zero, and not INVALID_HANDLE_VALUE. */
void imp_GetStdHandle(void) {
    uint32_t which = ARG(0);            /* -11 stdout, -12 stderr, -10 stdin */
    RET(0x10 + (which & 0xF));
    STDRET(1);
}

/* Synchronisation objects. The engine creates a couple at startup and mostly
 * checks them for non-NULL; real waiting arrives with the threading work. */
static uint32_t g_next_handle = 0x100;
void imp_CreateEventA(void)  { RET(g_next_handle++); STDRET(4); }
void imp_CreateMutexA(void)  { RET(g_next_handle++); STDRET(3); }
void imp_SetEvent(void)      { RET(1); STDRET(1); }
void imp_ReleaseMutex(void)  { RET(1); STDRET(1); }

/* WAIT_OBJECT_0: nothing is ever contended while everything is one thread. */
void imp_WaitForSingleObject(void) { RET(0); STDRET(2); }

void imp_CloseHandle(void) { RET(1); STDRET(1); }

/* ============================================================
 * Thread-local storage
 *
 * The Watcom CRT keeps its per-thread state in TLS. Single-threaded for now, so
 * one flat slot array is the whole implementation.
 * ponytail: 64 slots, one thread. Revisit when the engine's own threads run.
 * ============================================================ */

#define TLS_SLOTS 64
static uint32_t g_tls[TLS_SLOTS];
static int      g_tls_used[TLS_SLOTS];

void imp_TlsAlloc(void) {
    for (int i = 0; i < TLS_SLOTS; i++) {
        if (!g_tls_used[i]) {
            g_tls_used[i] = 1;
            g_tls[i] = 0;
            RET(i);
            STDRET(0);
            return;
        }
    }
    RET(0xFFFFFFFFu);   /* TLS_OUT_OF_INDEXES */
    STDRET(0);
}

void imp_TlsFree(void) {
    uint32_t i = ARG(0);
    if (i < TLS_SLOTS) g_tls_used[i] = 0;
    RET(1);
    STDRET(1);
}

void imp_TlsGetValue(void) {
    uint32_t i = ARG(0);
    RET(i < TLS_SLOTS ? g_tls[i] : 0);
    STDRET(1);
}

void imp_TlsSetValue(void) {
    uint32_t i = ARG(0);
    if (i < TLS_SLOTS) g_tls[i] = ARG(1);
    RET(1);
    STDRET(2);
}

/* ============================================================
 * Critical sections
 *
 * Single-threaded, so these are genuinely nothing -- but they must still pop
 * their argument, which is the only reason they exist as shims at all.
 * ============================================================ */

void imp_InitializeCriticalSection(void) { STDRET(1); }
void imp_DeleteCriticalSection(void)     { STDRET(1); }
void imp_EnterCriticalSection(void)      { STDRET(1); }
void imp_LeaveCriticalSection(void)      { STDRET(1); }

/* ============================================================
 * Errors
 * ============================================================ */

static uint32_t g_last_error = 0;
void imp_GetLastError(void) { RET(g_last_error); STDRET(0); }
void imp_SetLastError(void) { g_last_error = ARG(0); STDRET(1); }
