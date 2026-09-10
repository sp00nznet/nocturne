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
#define ALLOC_GRAN   0x10000u      /* Win32 allocation granularity: 64 KB */

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

/* Defined below, next to the rest of the arena. Declared here because the TIB is
 * allocated out of it and an implicit declaration would assume int. */
static uint32_t virt_alloc(uint32_t want_va, uint32_t size);

uint32_t shims_arena_base(void) { return g_arena_base; }
uint32_t shims_arena_end(void)  { return g_arena_end; }

/* ============================================================
 * The simulated TIB
 *
 * `fs:` is thread-relative on Win32, and the lifter knows it -- it emits
 * FS_BASE + offset for every fs: access rather than treating the segment as
 * flat. The runtime just has to point FS_BASE (g_fs_base) at a block shaped
 * like a TIB. Leave it 0 and `fs:[0]` reads address 0, which is what the null
 * dereference during CRT startup was.
 *
 * Only the first few fields matter here. fs:[0] is the head of the structured
 * exception handler chain, which the Watcom CRT links a record into on the way
 * up; 0xFFFFFFFF is the real end-of-chain sentinel. StackBase/StackLimit are
 * read by stack-probing code.
 *
 * We never dispatch SEH -- exceptions go to the host's handler -- so the chain
 * is only ever written and walked by the program itself. It needs to be
 * writable memory that looks right, and nothing more.
 * ============================================================ */

#define TIB_EXCEPTION_LIST  0x00
#define TIB_STACK_BASE      0x04   /* high address: the top of the stack */
#define TIB_STACK_LIMIT     0x08   /* low address: the bottom */
#define TIB_SELF            0x18   /* linear address of the TIB itself */
#define TIB_THREAD_ID       0x24
#define TIB_PEB             0x30
#define TIB_SIZE            0x1000

uint32_t shims_make_tib(uint32_t stack_base, uint32_t stack_top) {
    uint32_t tib = virt_alloc(0, TIB_SIZE);
    if (!tib) return 0;
    memset((void*)ADDR(tib), 0, TIB_SIZE);
    MEM32(tib + TIB_EXCEPTION_LIST) = 0xFFFFFFFFu;   /* end of chain */
    MEM32(tib + TIB_STACK_BASE)     = stack_top;
    MEM32(tib + TIB_STACK_LIMIT)    = stack_base;
    MEM32(tib + TIB_SELF)           = tib;
    MEM32(tib + TIB_THREAD_ID)      = 0x1000;        /* matches GetCurrentThreadId */
    MEM32(tib + TIB_PEB)            = 0;             /* no PEB until something asks */
    fprintf(stderr, "[shims] TIB 0x%08X\n", tib);
    return tib;
}

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

    /* Win32 guarantees that VirtualAlloc with lpAddress = NULL returns memory
     * aligned to the *allocation granularity* (64 KB), not merely to a page.
     * That is not a detail: heap managers of this era routinely find a block's
     * header by masking the low 16 bits off a pointer into it, so handing back
     * a merely page-aligned block makes them compute a header address that is
     * not one. Honour the guarantee. */
    uint32_t base = (g_arena_next + ALLOC_GRAN - 1) & ~(ALLOC_GRAN - 1);
    if ((uint64_t)base + size > g_arena_end) {
        fprintf(stderr, "[shims] heap arena exhausted (%u MB requested)\n",
                size / 1048576u);
        return 0;
    }
    g_arena_next = base;
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
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    /* ARG(3) flProtect: the arena is RW and committed on demand, so RESERVE and
     * COMMIT collapse to the same thing here. */
    uint32_t got = virt_alloc(addr, size);
    /* The first handful of allocations are the CRT building its heap, and what
     * it asks for -- and whether it got it -- is the first thing you want to
     * know when the heap comes back empty. Capped so it stays quiet once the
     * game is running and allocating in earnest. */
    static int logged = 0;
    if (logged < 16) {
        fprintf(stderr, "[shims] VirtualAlloc(addr=%08X, size=%u, type=%08X) -> %08X\n",
                addr, size, type, got);
        logged++;
    }
    g_eax = got;
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
 * Global heap and memory status
 *
 * The Watcom CRT will not hand out a single byte until it believes the machine
 * has memory. GlobalMemoryStatus fills a caller-supplied struct, and a stub that
 * returns 0 without touching it leaves a zeroed MEMORYSTATUS -- a machine with
 * no physical memory and none available -- so the CRT's calloc fails every
 * request, including the 244-byte per-thread block it needs before it can do
 * anything else.
 *
 * Report a plausible 1999 machine. The shipping readme asks for 64 MB, 96 MB for
 * hardware 3D; 256 MB is comfortably above that without being large enough to
 * upset size arithmetic in code that never expected a big number.
 * ============================================================ */

#define MS_LENGTH          0
#define MS_MEMORY_LOAD     4
#define MS_TOTAL_PHYS      8
#define MS_AVAIL_PHYS     12
#define MS_TOTAL_PAGEFILE 16
#define MS_AVAIL_PAGEFILE 20
#define MS_TOTAL_VIRTUAL  24
#define MS_AVAIL_VIRTUAL  28
#define MS_SIZEOF         32

#define PHYS_TOTAL   (256u * 1024u * 1024u)
#define PHYS_AVAIL   (192u * 1024u * 1024u)

void imp_GlobalMemoryStatus(void) {
    uint32_t p = ARG(0);
    if (p) {
        MEM32(p + MS_LENGTH)          = MS_SIZEOF;
        MEM32(p + MS_MEMORY_LOAD)     = 25;             /* percent in use */
        MEM32(p + MS_TOTAL_PHYS)      = PHYS_TOTAL;
        MEM32(p + MS_AVAIL_PHYS)      = PHYS_AVAIL;
        MEM32(p + MS_TOTAL_PAGEFILE)  = PHYS_TOTAL;
        MEM32(p + MS_AVAIL_PAGEFILE)  = PHYS_AVAIL;
        /* Virtual: report what the arena can actually still hand out, so the
         * engine's own sizing decisions match reality rather than a fiction. */
        MEM32(p + MS_TOTAL_VIRTUAL)   = ARENA_SIZE;
        MEM32(p + MS_AVAIL_VIRTUAL)   = g_arena_end - g_arena_next;
    }
    STDRET(1);
}

/* GlobalAlloc returns a handle. For GMEM_FIXED that handle *is* the pointer, and
 * GlobalLock on it is the identity -- which is the shape everything here relies
 * on, so movable blocks are simply never produced. */
void imp_GlobalAlloc(void) {
    /* ARG(0) uFlags: GMEM_ZEROINIT (0x40) is the only one that changes anything,
     * and arena pages arrive zeroed either way. */
    RET(virt_alloc(0, ARG(1)));
    STDRET(2);
}

void imp_GlobalFree(void)   { RET(0); STDRET(1); }   /* NULL means success */
void imp_GlobalLock(void)   { RET(ARG(0)); STDRET(1); }
void imp_GlobalUnlock(void) { RET(0); STDRET(1); }

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
