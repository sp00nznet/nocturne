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
#include <mmsystem.h>   /* WIN32_LEAN_AND_MEAN drops it; timeGetTime and the
                         * MMSYSERR_/JOYERR_/MCIERR_ codes live here. */

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

/* Files are the only handles with anything behind them; events and mutexes are
 * bare numbers, so closing one is genuinely nothing. Defined with the file
 * table, below. */
static int close_file_handle(uint32_t v);
void imp_CloseHandle(void) { close_file_handle(ARG(0)); RET(1); STDRET(1); }

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

/* ============================================================
 * File I/O
 *
 * The host *is* Win32, so every one of these is a forward to the real API. The
 * only translation needed is the handle: a HANDLE is 64-bit here and the game
 * stores it in a 32-bit slot, so open handles live in a table and the game gets
 * a tagged index. Buffers need no translation at all -- simulated memory is
 * ordinary host memory, so ReadFile writes straight into the image.
 *
 * ponytail: 64 open files, 16 open finds, no reuse-after-overflow. The engine
 * streams from a handful of POD archives; if a leak ever exhausts the table the
 * failure is loud (CreateFileA returns INVALID_HANDLE_VALUE) rather than subtle.
 * ============================================================ */

#define H_FILE_TAG   0x40000000u
#define H_FIND_TAG   0x50000000u
#define H_TAG_MASK   0xF0000000u
#define MAX_FILES    64
#define MAX_FINDS    16

static HANDLE g_file_h[MAX_FILES];
static HANDLE g_find_h[MAX_FINDS];

/* A simulated VA as a host pointer, with NULL preserved: an optional out-pointer
 * argument is NULL far more often than not, and ADDR(0) is not a null check. */
static void* simp(uint32_t va) { return va ? (void*)ADDR(va) : NULL; }

static uint32_t h_alloc(HANDLE* tab, int n, uint32_t tag, HANDLE h) {
    for (int i = 0; i < n; i++) {
        if (!tab[i]) { tab[i] = h; return tag | (uint32_t)i; }
    }
    CloseHandle(h);
    return 0xFFFFFFFFu;   /* INVALID_HANDLE_VALUE */
}

static HANDLE h_get(uint32_t v, uint32_t tag, HANDLE* tab, int n) {
    if ((v & H_TAG_MASK) != tag) return NULL;
    uint32_t i = v & ~H_TAG_MASK;
    return i < (uint32_t)n ? tab[i] : NULL;
}

#define FILE_H(v)  h_get((v), H_FILE_TAG, g_file_h, MAX_FILES)
#define FIND_H(v)  h_get((v), H_FIND_TAG, g_find_h, MAX_FINDS)

/* Every shim that can fail records the host's error, because the engine checks
 * GetLastError after an open to tell "missing" from "busy". */
static uint32_t win_fail(uint32_t ret) { g_last_error = GetLastError(); return ret; }

static int close_file_handle(uint32_t v) {
    HANDLE h = FILE_H(v);
    if (!h) return 0;
    CloseHandle(h);
    g_file_h[v & ~H_TAG_MASK] = NULL;
    return 1;
}

void imp_CreateFileA(void) {
    const char* name = (const char*)simp(ARG(0));
    HANDLE h = name ? CreateFileA(name, ARG(1), ARG(2), NULL, ARG(4), ARG(5), NULL)
                    : INVALID_HANDLE_VALUE;
    /* ARG(3) lpSecurityAttributes and ARG(6) hTemplateFile are simulated
     * pointers/handles that cannot be passed through; the engine passes NULL for
     * both, and honouring a non-NULL one would need a real translation. */
    RET(h == INVALID_HANDLE_VALUE ? win_fail(0xFFFFFFFFu)
                                  : h_alloc(g_file_h, MAX_FILES, H_FILE_TAG, h));
    STDRET(7);
}

void imp_ReadFile(void) {
    HANDLE h = FILE_H(ARG(0));
    DWORD got = 0;
    BOOL ok = h && ReadFile(h, simp(ARG(1)), ARG(2), &got, NULL);
    if (ARG(3)) MEM32(ARG(3)) = got;
    RET(ok ? 1 : win_fail(0));
    STDRET(5);
}

void imp_WriteFile(void) {
    HANDLE h = FILE_H(ARG(0));
    DWORD put = 0;
    BOOL ok = h && WriteFile(h, simp(ARG(1)), ARG(2), &put, NULL);
    if (ARG(3)) MEM32(ARG(3)) = put;
    RET(ok ? 1 : win_fail(0));
    STDRET(5);
}

void imp_SetFilePointer(void) {
    HANDLE h = FILE_H(ARG(0));
    LONG hi = ARG(2) ? (LONG)MEM32(ARG(2)) : 0;
    DWORD lo = h ? SetFilePointer(h, (LONG)ARG(1), ARG(2) ? &hi : NULL, ARG(3))
                 : INVALID_SET_FILE_POINTER;
    if (ARG(2)) MEM32(ARG(2)) = (uint32_t)hi;
    /* INVALID_SET_FILE_POINTER is a legal offset for a >4 GB file, so the caller
     * is meant to disambiguate with GetLastError. Set it either way. */
    RET(lo == INVALID_SET_FILE_POINTER ? win_fail(lo) : lo);
    STDRET(4);
}

void imp_GetFileType(void) {
    /* The std handles from imp_GetStdHandle are console, everything in the table
     * is a disk file, and anything else is unknown. */
    uint32_t v = ARG(0);
    RET(FILE_H(v) ? FILE_TYPE_DISK : (v >= 0x10 && v <= 0x1F ? FILE_TYPE_CHAR : 0));
    STDRET(1);
}

void imp_FlushFileBuffers(void) {
    HANDLE h = FILE_H(ARG(0));
    RET(h && FlushFileBuffers(h) ? 1 : win_fail(0));
    STDRET(1);
}

void imp_GetFileTime(void) {
    HANDLE h = FILE_H(ARG(0));
    FILETIME c, a, w;
    BOOL ok = h && GetFileTime(h, &c, &a, &w);
    if (ok) {
        if (ARG(1)) memcpy((void*)ADDR(ARG(1)), &c, 8);
        if (ARG(2)) memcpy((void*)ADDR(ARG(2)), &a, 8);
        if (ARG(3)) memcpy((void*)ADDR(ARG(3)), &w, 8);
    }
    RET(ok ? 1 : win_fail(0));
    STDRET(4);
}

void imp_SetFileTime(void) {
    HANDLE h = FILE_H(ARG(0));
    FILETIME c, a, w;
    if (ARG(1)) memcpy(&c, (void*)ADDR(ARG(1)), 8);
    if (ARG(2)) memcpy(&a, (void*)ADDR(ARG(2)), 8);
    if (ARG(3)) memcpy(&w, (void*)ADDR(ARG(3)), 8);
    RET(h && SetFileTime(h, ARG(1) ? &c : NULL, ARG(2) ? &a : NULL,
                         ARG(3) ? &w : NULL) ? 1 : win_fail(0));
    STDRET(4);
}

/* WIN32_FIND_DATAA is all DWORDs and char arrays -- no pointers, 4-byte
 * alignment throughout -- so its 64-bit host layout is byte-identical to the
 * 32-bit one the game expects and it can be copied straight across. */
static void find_data_out(uint32_t va, const WIN32_FIND_DATAA* fd) {
    if (va) memcpy((void*)ADDR(va), fd, sizeof(*fd));
}

void imp_FindFirstFileA(void) {
    const char* pat = (const char*)simp(ARG(0));
    WIN32_FIND_DATAA fd;
    HANDLE h = pat ? FindFirstFileA(pat, &fd) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) { RET(win_fail(0xFFFFFFFFu)); STDRET(2); return; }
    find_data_out(ARG(1), &fd);
    RET(h_alloc(g_find_h, MAX_FINDS, H_FIND_TAG, h));
    STDRET(2);
}

void imp_FindNextFileA(void) {
    HANDLE h = FIND_H(ARG(0));
    WIN32_FIND_DATAA fd;
    BOOL ok = h && FindNextFileA(h, &fd);
    if (ok) find_data_out(ARG(1), &fd);
    RET(ok ? 1 : win_fail(0));
    STDRET(2);
}

void imp_FindClose(void) {
    uint32_t v = ARG(0);
    HANDLE h = FIND_H(v);
    if (h) { FindClose(h); g_find_h[v & ~H_TAG_MASK] = NULL; }
    RET(h ? 1 : 0);
    STDRET(1);
}

void imp_GetFileAttributesA(void) {
    const char* p = (const char*)simp(ARG(0));
    DWORD a = p ? GetFileAttributesA(p) : INVALID_FILE_ATTRIBUTES;
    RET(a == INVALID_FILE_ATTRIBUTES ? win_fail(a) : a);
    STDRET(1);
}

void imp_SetFileAttributesA(void) {
    const char* p = (const char*)simp(ARG(0));
    RET(p && SetFileAttributesA(p, ARG(1)) ? 1 : win_fail(0));
    STDRET(2);
}

void imp_DeleteFileA(void) {
    const char* p = (const char*)simp(ARG(0));
    RET(p && DeleteFileA(p) ? 1 : win_fail(0));
    STDRET(1);
}

void imp_MoveFileA(void) {
    const char* a = (const char*)simp(ARG(0));
    const char* b = (const char*)simp(ARG(1));
    RET(a && b && MoveFileA(a, b) ? 1 : win_fail(0));
    STDRET(2);
}

void imp_CreateDirectoryA(void) {
    const char* p = (const char*)simp(ARG(0));
    RET(p && CreateDirectoryA(p, NULL) ? 1 : win_fail(0));
    STDRET(2);
}

void imp_GetCurrentDirectoryA(void) {
    char buf[MAX_PATH];
    DWORD n = GetCurrentDirectoryA(sizeof(buf), buf);
    RET(n ? sim_copy_out(ARG(1), ARG(0), buf) : win_fail(0));
    STDRET(2);
}

void imp_SetCurrentDirectoryA(void) {
    const char* p = (const char*)simp(ARG(0));
    RET(p && SetCurrentDirectoryA(p) ? 1 : win_fail(0));
    STDRET(1);
}

void imp_GetFullPathNameA(void) {
    const char* p = (const char*)simp(ARG(0));
    char buf[MAX_PATH];
    char* part = NULL;
    DWORD n = p ? GetFullPathNameA(p, sizeof(buf), buf, &part) : 0;
    if (!n || n >= sizeof(buf)) { RET(win_fail(n)); STDRET(4); return; }
    uint32_t wrote = sim_copy_out(ARG(2), ARG(1), buf);
    /* lpFilePart points *into* the caller's buffer, so it has to be that
     * buffer's VA plus the same offset -- never the host pointer. */
    if (ARG(3)) MEM32(ARG(3)) = part ? ARG(2) + (uint32_t)(part - buf) : 0;
    RET(wrote);
    STDRET(4);
}

/* ============================================================
 * Host object handles
 *
 * Windows, icons, cursors, brushes and DCs are all 64-bit HANDLEs here and
 * 32-bit slots over there. One table maps between them, tagged so a value that
 * never came from it (a stub's 0, a garbage pointer) is rejected rather than
 * indexed. Lookups dedupe: LoadCursorA called twice must hand back the same
 * number both times, because the engine compares them.
 * ============================================================ */

#define H_OBJ_TAG   0x70000000u
#define MAX_OBJS    256

static void*    g_obj[MAX_OBJS];
static uint32_t g_obj_n = 0;

/* Host handle -> tagged 32-bit value, creating the entry on first sight. */
static uint32_t h_put(void* h) {
    if (!h) return 0;
    for (uint32_t i = 0; i < g_obj_n; i++)
        if (g_obj[i] == h) return H_OBJ_TAG | i;
    if (g_obj_n >= MAX_OBJS) return 0;
    g_obj[g_obj_n] = h;
    return H_OBJ_TAG | g_obj_n++;
}

static void* h_ptr(uint32_t v) {
    if ((v & H_TAG_MASK) != H_OBJ_TAG) return NULL;
    uint32_t i = v & ~H_TAG_MASK;
    return i < g_obj_n ? g_obj[i] : NULL;
}

#define HWND_OF(v)  ((HWND)h_ptr(v))

/* ============================================================
 * Calling lifted code from the host
 *
 * A window procedure is host code calling game code -- the opposite direction
 * from everything else here. The simulated stack is a real stack, so a call in
 * is just: push the arguments below the current ESP, push the dummy return
 * address, run the body, and put ESP back where it was. Restoring ESP rather
 * than trusting the callee's epilogue makes this work whether the lifted body
 * is stdcall or cdecl, which matters because a window procedure is the one
 * place the convention is dictated by Windows and not by the game's compiler.
 *
 * Re-entrancy is fine and expected: this runs while lifted code is already deep
 * in a message pump, and the new frame simply sits below the pump's.
 * ============================================================ */

static uint32_t call_lifted(uint32_t va, int argc, const uint32_t* argv) {
    recomp_func_t f = recomp_lookup(va);
    if (!f) {
        fprintf(stderr, "[shims] callback into unlifted VA 0x%08X\n", va);
        return 0;
    }
    uint32_t saved_esp = g_esp;
    for (int i = argc - 1; i >= 0; i--) PUSH32(g_esp, argv[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
    g_esp = saved_esp;
    return g_eax;
}

/* ============================================================
 * Window classes and the window procedure bridge
 * ============================================================ */

/* WNDCLASSA, 32-bit layout (40 bytes). */
#define WC_STYLE         0
#define WC_WNDPROC       4
#define WC_CLS_EXTRA     8
#define WC_WND_EXTRA     12
#define WC_HINSTANCE     16
#define WC_HICON         20
#define WC_HCURSOR       24
#define WC_HBRBACKGROUND 28
#define WC_MENUNAME      32
#define WC_CLASSNAME     36

#define MAX_CLASSES 8
static struct { ATOM atom; uint32_t proc; } g_classes[MAX_CLASSES];
static int g_class_n = 0;

static uint32_t class_proc(ATOM a) {
    for (int i = 0; i < g_class_n; i++)
        if (g_classes[i].atom == a) return g_classes[i].proc;
    return 0;
}

/* The single host window procedure. Every registered class points here; which
 * lifted procedure to call is decided per window from its class atom, so two
 * classes with different procedures still work. */
/* The message being dispatched right now, innermost first.
 *
 * Some messages carry a host pointer in lParam -- WM_NCCREATE and WM_CREATE
 * carry a CREATESTRUCT, WM_GETMINMAXINFO a MINMAXINFO -- and a host pointer is
 * 64 bits, so what the game receives is a truncated half of one. That is fine
 * for a message it ignores, but not when it hands lParam straight back to
 * DefWindowProcA, which does dereference it: DefWindowProcA(WM_NCCREATE) with a
 * broken CREATESTRUCT returns FALSE, and returning FALSE from WM_NCCREATE
 * aborts CreateWindowEx. So remember the real lParam for the message in flight
 * and give it back on the way out.
 *
 * ponytail: pass-through only. If the engine ever *reads* a pointer lParam it
 * will fault on the truncated address rather than read something plausible,
 * which is the failure worth having -- marshal that message's struct then. */
typedef struct { HWND hwnd; UINT msg; LPARAM lp; } cur_msg_t;
static cur_msg_t g_cur_msg;

static LRESULT CALLBACK host_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    uint32_t proc = class_proc((ATOM)GetClassLongPtrA(hwnd, GCW_ATOM));
    if (!proc) return DefWindowProcA(hwnd, msg, wp, lp);

    /* Saved and restored around the call, so nested dispatches nest correctly. */
    cur_msg_t outer = g_cur_msg;
    g_cur_msg.hwnd = hwnd; g_cur_msg.msg = msg; g_cur_msg.lp = lp;
    /* h_put, not h_ptr: WM_NCCREATE and WM_CREATE arrive before CreateWindowExA
     * has returned, so this is where the window first gets a 32-bit identity. */
    uint32_t args[4] = { h_put(hwnd), msg, (uint32_t)wp, (uint32_t)lp };
    uint32_t r = call_lifted(proc, 4, args);
    g_cur_msg = outer;
    return (LRESULT)(LONG_PTR)(int32_t)r;
}

void imp_RegisterClassA(void) {
    uint32_t wc = ARG(0);
    if (!wc || g_class_n >= MAX_CLASSES) { RET(0); STDRET(1); return; }

    WNDCLASSA c;
    memset(&c, 0, sizeof(c));
    c.style         = MEM32(wc + WC_STYLE);
    c.lpfnWndProc   = host_wndproc;
    c.cbClsExtra    = (int)MEM32(wc + WC_CLS_EXTRA);
    c.cbWndExtra    = (int)MEM32(wc + WC_WND_EXTRA);
    c.hInstance     = GetModuleHandleA(NULL);
    c.hIcon         = (HICON)h_ptr(MEM32(wc + WC_HICON));
    c.hCursor       = (HCURSOR)h_ptr(MEM32(wc + WC_HCURSOR));
    c.hbrBackground = (HBRUSH)h_ptr(MEM32(wc + WC_HBRBACKGROUND));
    /* Class and menu names are simulated pointers, and simulated memory is host
     * memory, so the strings themselves need no copy. */
    c.lpszMenuName  = (LPCSTR)simp(MEM32(wc + WC_MENUNAME));
    c.lpszClassName = (LPCSTR)simp(MEM32(wc + WC_CLASSNAME));

    ATOM a = RegisterClassA(&c);
    if (!a) { RET(win_fail(0)); STDRET(1); return; }
    g_classes[g_class_n].atom = a;
    g_classes[g_class_n].proc = MEM32(wc + WC_WNDPROC);
    fprintf(stderr, "[shims] RegisterClassA(\"%s\") wndproc=sub_%08X atom=%u\n",
            c.lpszClassName ? c.lpszClassName : "?", g_classes[g_class_n].proc, a);
    g_class_n++;
    RET(a);
    STDRET(1);
}

void imp_CreateWindowExA(void) {
    /* ARG(9) hMenu and ARG(11) lpParam are passed through as NULL: the menu
     * would be a simulated handle with no host object behind it, and lpParam
     * only reaches the game again inside a CREATESTRUCT full of host pointers,
     * which is worse than not offering it at all.
     * ponytail: no menu, no create param. Marshal a CREATESTRUCT if one is ever
     * actually read. */
    HWND h = CreateWindowExA(ARG(0), (LPCSTR)simp(ARG(1)), (LPCSTR)simp(ARG(2)),
                             ARG(3), (int)ARG(4), (int)ARG(5),
                             (int)ARG(6), (int)ARG(7),
                             HWND_OF(ARG(8)), NULL, GetModuleHandleA(NULL), NULL);
    fprintf(stderr, "[shims] CreateWindowExA(\"%s\", style=%08X, %dx%d) -> %p\n",
            (const char*)simp(ARG(1)), ARG(3), (int)ARG(6), (int)ARG(7), (void*)h);
    if (!h) fprintf(stderr, "[shims] CreateWindowExA failed, err %lu\n", GetLastError());
    RET(h ? h_put(h) : win_fail(0));
    STDRET(12);
}

void imp_DefWindowProcA(void) {
    HWND h = HWND_OF(ARG(0));
    UINT m = ARG(1);
    /* Same window, same message as the one in flight: this is the ordinary
     * "not mine, you take it" hand-back, so give the real lParam, not the
     * truncated copy the game is holding. */
    LPARAM lp = (h == g_cur_msg.hwnd && m == g_cur_msg.msg)
                    ? g_cur_msg.lp : (LPARAM)(int32_t)ARG(3);
    RET((uint32_t)(LONG_PTR)DefWindowProcA(h, m, (WPARAM)ARG(2), lp));
    STDRET(4);
}

/* ============================================================
 * The message pump
 *
 * MSG, 32-bit layout (28 bytes). hwnd is the only field needing translation.
 * ============================================================ */

#define MSG_HWND    0
#define MSG_MESSAGE 4
#define MSG_WPARAM  8
#define MSG_LPARAM  12
#define MSG_TIME    16
#define MSG_PT_X    20
#define MSG_PT_Y    24

static void msg_out(uint32_t va, const MSG* m) {
    MEM32(va + MSG_HWND)    = h_put(m->hwnd);
    MEM32(va + MSG_MESSAGE) = m->message;
    MEM32(va + MSG_WPARAM)  = (uint32_t)m->wParam;
    MEM32(va + MSG_LPARAM)  = (uint32_t)m->lParam;
    MEM32(va + MSG_TIME)    = m->time;
    MEM32(va + MSG_PT_X)    = (uint32_t)m->pt.x;
    MEM32(va + MSG_PT_Y)    = (uint32_t)m->pt.y;
}

static void msg_in(uint32_t va, MSG* m) {
    memset(m, 0, sizeof(*m));
    m->hwnd    = HWND_OF(MEM32(va + MSG_HWND));
    m->message = MEM32(va + MSG_MESSAGE);
    m->wParam  = (WPARAM)MEM32(va + MSG_WPARAM);
    m->lParam  = (LPARAM)(int32_t)MEM32(va + MSG_LPARAM);
    m->time    = MEM32(va + MSG_TIME);
    m->pt.x    = (LONG)MEM32(va + MSG_PT_X);
    m->pt.y    = (LONG)MEM32(va + MSG_PT_Y);
}

void imp_PeekMessageA(void) {
    MSG m;
    BOOL got = PeekMessageA(&m, HWND_OF(ARG(1)), ARG(2), ARG(3), ARG(4));
    if (got && ARG(0)) msg_out(ARG(0), &m);
    RET(got ? 1 : 0);
    STDRET(5);
}

void imp_TranslateMessage(void) {
    MSG m;
    if (ARG(0)) { msg_in(ARG(0), &m); RET(TranslateMessage(&m) ? 1 : 0); }
    else RET(0);
    STDRET(1);
}

void imp_DispatchMessageA(void) {
    MSG m;
    if (ARG(0)) { msg_in(ARG(0), &m); RET((uint32_t)(LONG_PTR)DispatchMessageA(&m)); }
    else RET(0);
    STDRET(1);
}

void imp_PostQuitMessage(void) { PostQuitMessage((int)ARG(0)); STDRET(1); }

/* ============================================================
 * Window state
 * ============================================================ */

void imp_ShowWindow(void)          { RET(ShowWindow(HWND_OF(ARG(0)), (int)ARG(1))); STDRET(2); }
void imp_UpdateWindow(void)        { RET(UpdateWindow(HWND_OF(ARG(0))) ? 1 : 0); STDRET(1); }
void imp_BringWindowToTop(void)    { RET(BringWindowToTop(HWND_OF(ARG(0))) ? 1 : 0); STDRET(1); }
void imp_SetForegroundWindow(void) { RET(SetForegroundWindow(HWND_OF(ARG(0))) ? 1 : 0); STDRET(1); }
void imp_SetFocus(void)            { RET(h_put(SetFocus(HWND_OF(ARG(0))))); STDRET(1); }
void imp_IsIconic(void)            { RET(IsIconic(HWND_OF(ARG(0))) ? 1 : 0); STDRET(1); }
void imp_SetCursor(void)           { RET(h_put(SetCursor((HCURSOR)h_ptr(ARG(0))))); STDRET(1); }
void imp_SetCursorPos(void)        { RET(SetCursorPos((int)ARG(0), (int)ARG(1)) ? 1 : 0); STDRET(2); }

void imp_MoveWindow(void) {
    RET(MoveWindow(HWND_OF(ARG(0)), (int)ARG(1), (int)ARG(2),
                   (int)ARG(3), (int)ARG(4), ARG(5) ? TRUE : FALSE) ? 1 : 0);
    STDRET(6);
}

/* RECT is four LONGs in both worlds, so it copies straight across. */
void imp_GetClientRect(void) {
    RECT r = {0, 0, 0, 0};
    BOOL ok = GetClientRect(HWND_OF(ARG(0)), &r);
    if (ARG(1)) memcpy((void*)ADDR(ARG(1)), &r, sizeof(r));
    RET(ok ? 1 : 0);
    STDRET(2);
}

void imp_SetRectEmpty(void) {
    if (ARG(0)) memset((void*)ADDR(ARG(0)), 0, sizeof(RECT));
    RET(1);
    STDRET(1);
}

void imp_InvalidateRect(void) {
    RECT r;
    if (ARG(1)) memcpy(&r, (void*)ADDR(ARG(1)), sizeof(r));
    RET(InvalidateRect(HWND_OF(ARG(0)), ARG(1) ? &r : NULL,
                       ARG(2) ? TRUE : FALSE) ? 1 : 0);
    STDRET(3);
}

void imp_FindWindowA(void) {
    /* The engine calls this to refuse a second instance. Answering with the host
     * lookup is right: our own window is registered under the same class name,
     * so a second copy of the recompilation finds the first exactly as the
     * original did. */
    RET(h_put(FindWindowA((LPCSTR)simp(ARG(0)), (LPCSTR)simp(ARG(1)))));
    STDRET(2);
}

void imp_MessageBoxA(void) {
    RET(MessageBoxA(HWND_OF(ARG(0)), (LPCSTR)simp(ARG(1)),
                    (LPCSTR)simp(ARG(2)), ARG(3)));
    STDRET(4);
}

/* Predefined icons and cursors: the resource id is an integer atom (IDI_*,
 * IDC_*) when the high word is zero, and a simulated string pointer otherwise.
 * The game's own icons live in the original .rsrc, which is mapped but not
 * registered with the host loader, so a load against its module handle can only
 * be answered with the system default. */
static LPCSTR res_name(uint32_t v) {
    return (v >> 16) ? (LPCSTR)simp(v) : MAKEINTRESOURCEA(v & 0xFFFF);
}

void imp_LoadIconA(void) {
    RET(h_put(LoadIconA(NULL, ARG(0) ? IDI_APPLICATION : res_name(ARG(1)))));
    STDRET(2);
}

void imp_LoadCursorA(void) {
    RET(h_put(LoadCursorA(NULL, ARG(0) ? IDC_ARROW : res_name(ARG(1)))));
    STDRET(2);
}

void imp_GetStockObject(void) { RET(h_put(GetStockObject((int)ARG(0)))); STDRET(1); }

/* ============================================================
 * Time
 *
 * The frame loop needs these to advance or it either spins or believes no time
 * passes at all.
 * ============================================================ */

void imp_timeGetTime(void)     { RET(timeGetTime()); STDRET(0); }
void imp_timeBeginPeriod(void) { RET(timeBeginPeriod(ARG(0))); STDRET(1); }
void imp_timeEndPeriod(void)   { RET(timeEndPeriod(ARG(0))); STDRET(1); }
void imp_Sleep(void)           { Sleep(ARG(0)); STDRET(1); }

void imp_QueryPerformanceCounter(void) {
    LARGE_INTEGER v;
    BOOL ok = QueryPerformanceCounter(&v);
    if (ARG(0)) { MEM32(ARG(0)) = v.LowPart; MEM32(ARG(0) + 4) = (uint32_t)v.HighPart; }
    RET(ok ? 1 : 0);
    STDRET(1);
}

void imp_QueryPerformanceFrequency(void) {
    LARGE_INTEGER v;
    BOOL ok = QueryPerformanceFrequency(&v);
    if (ARG(0)) { MEM32(ARG(0)) = v.LowPart; MEM32(ARG(0) + 4) = (uint32_t)v.HighPart; }
    RET(ok ? 1 : 0);
    STDRET(1);
}

/* SYSTEMTIME is eight WORDs, identical in both worlds. */
void imp_GetLocalTime(void) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (ARG(0)) memcpy((void*)ADDR(ARG(0)), &st, sizeof(st));
    STDRET(1);
}

/* ============================================================
 * Devices that are not implemented yet
 *
 * A stub that returns 0 is not neutral. Half of Win32 uses 0 for "success" --
 * MMSYSERR_NOERROR, DS_OK, DD_OK are all zero -- so a stub that returns it is
 * claiming the call worked while leaving the caller's output struct untouched.
 * The caller then reads uninitialised stack as if it were a device description,
 * and the failure lands somewhere else entirely: waveOutGetDevCapsA returning
 * "success" is what put a garbage pointer into a CRT printf and faulted on a
 * read of 0xFFFFFFE0, several frames from the actual mistake.
 *
 * So an unimplemented device reports that it is absent, which is a thing the
 * engine already knows how to handle -- the game shipped for machines with no
 * sound card and no joystick.
 * ============================================================ */

void imp_waveOutGetDevCapsA(void) { RET(MMSYSERR_NODRIVER); STDRET(3); }
void imp_waveOutOpen(void)        { RET(MMSYSERR_NODRIVER); STDRET(6); }
void imp_joyGetDevCapsA(void)     { RET(MMSYSERR_NODRIVER); STDRET(3); }
void imp_joyGetPos(void)          { RET(JOYERR_UNPLUGGED);  STDRET(2); }
void imp_mciSendStringA(void)     { RET(MCIERR_INVALID_DEVICE_NAME); STDRET(4); }

/* DirectSound and DirectDraw hand back an interface pointer through an out
 * parameter. Reporting DS_OK/DD_OK without writing one leaves the game holding
 * whatever was on the stack and calling through it. Phase 6 replaces these with
 * the real thing; until then they are honestly absent. */
void imp_DirectSoundCreate(void) { RET(0x88780078u); STDRET(3); }  /* DSERR_NODRIVER */
void imp_DirectDrawCreate(void)  { RET(0x80004005u); STDRET(3); }  /* DDERR_GENERIC */

/* The game asking to exit has to actually exit. A stub that returns lets the
 * caller run on past its own decision to quit -- and everything after that is
 * code the original would never have executed, so the crash it eventually
 * produces describes nothing real. */
void imp_ExitProcess(void) {
    uint32_t code = ARG(0);
    /* g_cur_func names the subsystem that gave up, which is most of the
     * diagnosis when the exit is the engine's own "cannot continue" path. */
    fprintf(stderr, "[shims] ExitProcess(%u) from sub_%08X -- %u indirect calls\n",
            code, g_cur_func, g_icall_count);
    fflush(NULL);
    STDRET(1);          /* never observed, but the contract is the contract */
    ExitProcess(code);
}
