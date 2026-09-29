/*
 * Nocturne Recompilation - runtime core.
 *
 * Owns the simulated machine the lifted code runs inside: the register file, the
 * memory image, the stack, and the three lookup paths RECOMP_ICALL/ITAIL use to
 * turn a 32-bit VA back into a C function.
 *
 * Memory model: the original PE is mapped at its real image base (0x00400000) by
 * the shared loader, so g_mem_base stays 0 and every MEM32(va) is a direct read
 * of the program's own data. That requires the host executable to be linked at a
 * high base so the target's range is free -- see CMakeLists.
 *
 * Nocturne's .bss is 42 MB. That is not an allocation to make at startup and
 * hand out; it is part of the image, mapped and zeroed with everything else.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"
#include "image_loader.h"
#include "crash_report.h"

/* ============================================================
 * Register file
 * ============================================================ */

uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
uint32_t g_ebx = 0, g_esi = 0, g_edi = 0, g_ebp = 0;
uint16_t g_seg_cs = 0, g_seg_ds = 0, g_seg_es = 0;
uint16_t g_seg_fs = 0, g_seg_gs = 0, g_seg_ss = 0;
uint32_t g_fs_base = 0, g_gs_base = 0;

double   g_st[8]   = {0};
int      g_fp_top  = 0;
uint16_t g_fpu_cw  = 0x037F;   /* x87 default: round-to-nearest, 64-bit mantissa */
uint64_t g_mm[8]   = {0};      /* MMX file; Nocturne is an MMX build */

ptrdiff_t g_mem_base = 0;      /* 1:1 mapping -- see the note above */

uint32_t g_cur_func = 0;
uint32_t g_icall_trace[ICALL_TRACE_SIZE] = {0};
uint32_t g_icall_from[ICALL_TRACE_SIZE] = {0};
uint32_t g_icall_trace_idx = 0;
uint32_t g_icall_count = 0;

#ifdef RECOMP_TRACE
uint32_t g_enter_trace[RECOMP_ENTER_SIZE] = {0};
uint32_t g_enter_idx = 0;
#endif

/* ============================================================
 * Memory layout
 * ============================================================ */

#define NOCTURNE_IMAGE_BASE  0x00400000u

/* Watcom's CRT probes and the engine's deep call chains both want room. */
#define STACK_SIZE           0x00400000u   /* 4 MB */
#define ALLOC_GRANULARITY    0x00010000u   /* VirtualAlloc's base granularity */

static uint32_t g_stack_base = 0;
static uint32_t g_stack_top = 0;
static uint32_t g_image_span = 0;

/* Region bounds, so the shims can answer VirtualQuery without duplicating the
 * layout constants. */
uint32_t recomp_image_base(void) { return NOCTURNE_IMAGE_BASE; }
uint32_t recomp_image_end(void)  { return NOCTURNE_IMAGE_BASE + g_image_span; }
uint32_t recomp_stack_base(void) { return g_stack_base; }
uint32_t recomp_stack_end(void)  { return g_stack_top; }

int shims_init(uint32_t base);
uint32_t shims_arena_base(void);
uint32_t shims_arena_end(void);
uint32_t shims_make_tib(uint32_t stack_base, uint32_t stack_top);

/* Declared in recomp_types.h, defined per project. Nothing extra to dump unless
 * the build enabled the per-function entry tracer. */
#ifdef RECOMP_TRACE
/* RECOMP_BREAK=<hex va>: the first time that function is entered, print the
 * entries that led there (from a deeper ring than the header's), run-length
 * collapsed. A breakpoint for "what ran just before the engine gave up". */
#define BREAK_RING 16384u
static uint32_t g_break_ring[BREAK_RING];
static uint32_t g_break_va = 0xFFFFFFFFu;

void recomp_trace_enter(uint32_t va) {
    g_enter_trace[g_enter_idx & (RECOMP_ENTER_SIZE - 1)] = va;
    g_break_ring[g_enter_idx & (BREAK_RING - 1)] = va;
    g_enter_idx++;
    if (g_break_va == 0xFFFFFFFFu) {
        const char* b = getenv("RECOMP_BREAK");
        g_break_va = b ? (uint32_t)strtoul(b, NULL, 16) : 0;
    }
    /* RECOMP_REACH=va,va,...: report the first entry of each listed function. */
    static uint32_t reach[512]; static int nreach = -1;
    if (nreach < 0) {
        nreach = 0;
        for (const char* r = getenv("RECOMP_REACH"); r && *r && nreach < 512; ) {
            reach[nreach++] = (uint32_t)strtoul(r, (char**)&r, 16);
            if (*r == ',') r++;
        }
    }
    for (int i = 0; i < nreach; i++)
        if (reach[i] == va) {
            fprintf(stderr, "[reach] %08X #%u\n", va, g_enter_idx);
            reach[i] = 0;
        }
    if (va != g_break_va || !va) return;
    g_break_va = 0;                             /* once */
    fprintf(stderr, "[break] sub_%08X reached; preceding entries:\n", va);
    uint32_t prev = 0, run = 0, n = g_enter_idx < BREAK_RING ? g_enter_idx : BREAK_RING;
    for (uint32_t i = g_enter_idx - n; i != g_enter_idx; i++) {
        uint32_t f = g_break_ring[i & (BREAK_RING - 1)];
        if (f == prev) { run++; continue; }
        if (prev) fprintf(stderr, run > 1 ? " %08X*%u" : " %08X", prev, run);
        prev = f; run = 1;
    }
    fprintf(stderr, " %08X\n", prev);
}
#endif

void recomp_dump_trace(const char* why) {
#ifdef RECOMP_TRACE
    fprintf(stderr, "  last %d function entries (oldest first):\n", RECOMP_ENTER_SIZE);
    for (int i = 0; i < RECOMP_ENTER_SIZE; i++) {
        uint32_t idx = (g_enter_idx - RECOMP_ENTER_SIZE + i) & (RECOMP_ENTER_SIZE - 1);
        if (g_enter_trace[idx])
            fprintf(stderr, "    sub_%08X\n", g_enter_trace[idx]);
    }
#else
    (void)why;
#endif
}

static const char* describe_region(uint32_t va);   /* defined below */

/* The Watcom CRT's heap state, printed with every crash report.
 *
 * Bring-up is currently stuck on malloc(244) returning NULL, and these are the
 * globals that decide whether it can: two gates the grow path checks before it
 * will even call VirtualAlloc, the free-list head the grown block gets linked
 * into, and the per-thread block whose null value is what actually faults.
 * Having them in the report turns one build-and-run per hypothesis into one run.
 */
static void report_crt_heap(void) {
    static const struct { uint32_t va; const char* name; } watch[] = {
        { 0x005C1FE8u, "grow gate A      " },
        { 0x005C1CC8u, "grow gate B      " },
        { 0x005C1680u, "heap free-list   " },
        { 0x005C1684u, "heap cached block" },
        { 0x005C1688u, "largest-free hint" },
        { 0x02DE4E3Cu, "per-thread block " },
    };
    fprintf(stderr, "  CRT heap globals:\n");
    for (unsigned i = 0; i < sizeof(watch) / sizeof(watch[0]); i++)
        fprintf(stderr, "    [%08X] %s = %08X\n",
                watch[i].va, watch[i].name, MEM32(watch[i].va));

    /* The per-thread block is a 244-byte buffer in the CRT main wrapper's stack
     * frame, and +0x54 is the slot the SEH installer writes its argument to and
     * then dereferences. Show it, plus where the stack pointer is now: if the
     * block sits below the live stack pointer, the slot is being overwritten
     * rather than mis-stored, and those are different bugs. */
    uint32_t blk = MEM32(0x02DE4E3Cu);
    fprintf(stderr, "    per-thread block %08X %s, +0x54 = %08X\n",
            blk, describe_region(blk), blk ? MEM32(blk + 0x54) : 0);
    fprintf(stderr, "    esp now %08X, stack %08X-%08X (block is %s live esp)\n",
            g_esp, g_stack_base, g_stack_top,
            (blk && blk < g_esp) ? "BELOW" : "above");
}

/* Turn a simulated VA into a region name for the crash report. A bad pointer's
 * region is usually the whole diagnosis: "read of 00000000" is a null deref,
 * "read of 03200004 (heap arena)" is a use of freed or uninitialised memory. */
static const char* describe_region(uint32_t va) {
    if (va < 0x10000u)                          return "(null page)";
    if (va >= NOCTURNE_IMAGE_BASE && va < NOCTURNE_IMAGE_BASE + g_image_span)
                                                return "(image)";
    if (va >= g_stack_base && va < g_stack_top) return "(stack)";
    if (va >= shims_arena_base() && va < shims_arena_end())
                                                return "(heap arena)";
    return "(unmapped)";
}

/* Place the stack immediately above the mapped image.
 *
 * Below it is not safe: Nocturne's image spans 42 MB from 0x00400000, so only
 * ~4 MB of address space exists underneath and a 4 MB stack based at 0x00100000
 * runs to 0x00500000 -- straight into the image. That collision is what
 * ERROR_INVALID_ADDRESS was reporting. Deriving the base from the actual span
 * means it cannot silently overlap whatever the next target's image looks like.
 */
static int map_stack(uint32_t image_base, uint32_t span) {
    uint32_t base = (image_base + span + ALLOC_GRANULARITY - 1)
                    & ~(ALLOC_GRANULARITY - 1);
    void* p = VirtualAlloc((void*)(uintptr_t)base, STACK_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        fprintf(stderr, "[runtime] stack VirtualAlloc @ 0x%08X (%u MB) failed (%lu)\n",
                base, STACK_SIZE / 1048576u, GetLastError());
        return 0;
    }
    memset(p, 0, STACK_SIZE);
    g_stack_base = base;
    g_stack_top = base + STACK_SIZE;
    fprintf(stderr, "[runtime] stack 0x%08X-0x%08X\n", base, g_stack_top);
    return 1;
}

/* ============================================================
 * Dispatch
 * ============================================================ */

/* Binary search over the generated table, which run_lift.py emits sorted by VA. */
recomp_func_t recomp_lookup(uint32_t va) {
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t k = recomp_dispatch_table[mid].address;
        if (k == va) return recomp_dispatch_table[mid].func;
        if (k < va) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* Host thunks: VAs at the top of the 32-bit space that name host C functions.
 * COM vtables and GetProcAddress results hand these to lifted code, which calls
 * through them like any other function pointer. Nothing is mapped up here, so a
 * thunk VA can never collide with image, stack or heap.
 *
 * g_host_thunk_id is the index of the thunk being called, set on lookup: that is
 * how one host function can serve a whole vtable (it is read immediately, before
 * anything else can do a lookup). */
#define HOST_THUNK_BASE  0xFFF00000u
#define HOST_THUNK_MAX   1024u
static recomp_func_t g_host_thunks[HOST_THUNK_MAX];
static uint32_t g_host_thunk_n = 0;
uint32_t g_host_thunk_id = 0;

uint32_t recomp_host_thunk(recomp_func_t f) {
    if (g_host_thunk_n >= HOST_THUNK_MAX) {
        fprintf(stderr, "[runtime] host thunk table full\n");
        abort();
    }
    g_host_thunks[g_host_thunk_n] = f;
    return HOST_THUNK_BASE + 4u * g_host_thunk_n++;
}

recomp_func_t recomp_lookup_manual(uint32_t va) {
    uint32_t i = (va - HOST_THUNK_BASE) / 4u;
    if (va < HOST_THUNK_BASE || i >= g_host_thunk_n) return NULL;
    g_host_thunk_id = i;
    return g_host_thunks[i];
}

/* Import bridges. The table is small (171) and generated in IAT order, which is
 * not sorted, so this is a linear scan. It is called once per import call; if it
 * ever shows up in a profile, sort the generated table and bisect it.
 * ponytail: linear scan over 171 entries, sort + bisect if it ever matters. */
recomp_func_t recomp_lookup_import(uint32_t va) {
    for (uint32_t i = 0; i < nocturne_import_bridge_count; i++)
        if (nocturne_import_bridges[i].address == va)
            return nocturne_import_bridges[i].func;
    return NULL;
}

void nocturne_install_iat(void) {
    for (uint32_t i = 0; i < nocturne_iat_slot_count; i++) {
        uint32_t slot = nocturne_iat_slots[i];
        MEM32(slot) = slot;
    }
}

/* ============================================================
 * Entry
 * ============================================================ */

#define NOCTURNE_ENTRY  0x00567152u   /* PE entry point (Watcom CRT startup) */

/* RECOMP_WATCH=1: every 2 s, say where lifted code is. A hang or a spin loop
 * prints nothing on its own; this names the function it is spinning in and the
 * indirect calls it keeps making. Racy reads of plain globals, by design. */
static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    /* Also a poor man's sampling profiler: g_cur_func every millisecond, the
     * top entries printed with each report. A loop shows up as its members. */
    enum { NS = 4096 };
    static uint32_t key[NS], cnt[NS];
    for (;;) {
        memset(key, 0, sizeof(key)); memset(cnt, 0, sizeof(cnt));
        for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 2000; ) {
            Sleep(1);
            uint32_t f = g_cur_func, h = (f * 2654435761u) % NS;
            while (key[h] && key[h] != f) h = (h + 1) % NS;
            key[h] = f; cnt[h]++;
        }
        fprintf(stderr, "[watch] top:");
        for (int k = 0; k < 12; k++) {
            uint32_t best = 0;
            for (uint32_t i = 1; i < NS; i++) if (cnt[i] > cnt[best]) best = i;
            if (!cnt[best]) break;
            fprintf(stderr, " %08X:%u", key[best], cnt[best]);
            cnt[best] = 0;
        }
        fprintf(stderr, "\n");
        uint32_t n = g_icall_trace_idx;
        /* Key ring head/tail, the game's main hwnd, and its quit flag: what
         * the engine's blocking getKey() (0x558B00) spins on. */
        fprintf(stderr, "[watch] keyring %u/%u hwnd %08X quit %u\n", MEM32(0x2DE0848u),
                MEM32(0x2DE084Cu), MEM32(0x2DE2098u), MEM32(0x2DE20A0u));
        fprintf(stderr, "[watch] in sub_%08X, %u icalls; last:", g_cur_func, g_icall_count);
        for (int i = 1; i <= 6; i++)
            fprintf(stderr, " %08X<-%08X", g_icall_trace[(n - i) & (ICALL_TRACE_SIZE - 1)],
                    g_icall_from[(n - i) & (ICALL_TRACE_SIZE - 1)]);
        fprintf(stderr, "\n");
    }
}

int main(int argc, char** argv) {
    const char* image = (argc > 1) ? argv[1] : "nocturne.exe";

    uint32_t span = recomp_load_image(image, NOCTURNE_IMAGE_BASE);
    if (!span) {
        fprintf(stderr, "[runtime] could not map %s\n", image);
        return 1;
    }
    fprintf(stderr, "[runtime] mapped %s at 0x%08X, %.1f MB\n",
            image, NOCTURNE_IMAGE_BASE, span / 1048576.0);

    g_image_span = span;
    if (!map_stack(NOCTURNE_IMAGE_BASE, span)) return 1;

    /* The heap arena goes above the stack, so image / stack / heap are three
     * disjoint regions climbing the low 32 bits in a fixed order. */
    if (!shims_init(g_stack_top)) return 1;

    /* fs: is thread-relative; the lifter emits FS_BASE + offset for it, so the
     * base has to point at a TIB before any lifted code touches fs:[0]. */
    g_fs_base = shims_make_tib(g_stack_base, g_stack_top);
    if (!g_fs_base) return 1;

    recomp_set_region_describer(describe_region);
    recomp_set_extra_reporter(report_crt_heap);
    recomp_install_crash_handler();

    nocturne_install_iat();
    fprintf(stderr, "[runtime] %u lifted functions, %u import bridges\n",
            recomp_dispatch_count, nocturne_import_bridge_count);

    g_esp = g_stack_top - 0x100;    /* leave a little headroom above the frame */
    g_ebp = 0;

    recomp_func_t entry = recomp_lookup(NOCTURNE_ENTRY);
    if (!entry) {
        fprintf(stderr, "[runtime] entry 0x%08X is not in the dispatch table\n",
                NOCTURNE_ENTRY);
        return 1;
    }

    if (getenv("RECOMP_WATCH")) CreateThread(NULL, 0, watchdog, NULL, 0, NULL);

    PUSH32(g_esp, RECOMP_RETADDR);
    entry();

    fprintf(stderr, "[runtime] entry returned; %u indirect calls\n", g_icall_count);
    return 0;
}
