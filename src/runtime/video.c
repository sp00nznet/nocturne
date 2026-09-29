/*
 * Nocturne Recompilation - video: DirectDraw and the renderer DLL.
 *
 * DirectDraw. wincore\wddvmem.cpp uses a very small slice of it: one IDirectDraw
 * (v1), a primary surface with a back buffer, Lock/Unlock/Flip, and an 8-bit
 * palette. The engine draws into the locked back buffer itself, so DirectDraw is
 * only a presentation surface here. Surfaces are plain arena memory the lifted
 * code writes into directly; Flip hands the back buffer to GDI, which already
 * knows how to show 8/16/24/32-bit DIBs, palettes included.
 *
 * COM objects live in simulated memory as { vtbl, index }. Every vtable slot is
 * a host thunk (recomp_host_thunk) into one dispatcher, which recovers the
 * interface and method from g_host_thunk_id. Each method pops exactly the
 * argument count its ddraw.h prototype declares -- the table below is that
 * header transcribed, including `this`.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"

uint32_t recomp_host_thunk(recomp_func_t f);
extern uint32_t g_host_thunk_id;
uint32_t shims_alloc(uint32_t size);
void recomp_dump_trace(const char* why);
HWND shims_hwnd(uint32_t v);

#define DD_OK               0u
#define DDERR_GENERIC       0x80004005u
#define DDERR_UNSUPPORTED   0x80004001u
#define E_NOINTERFACE_      0x80004002u
#define DDERR_NOTFOUND      0x887600FFu

/* ============================================================
 * Method tables (argument counts include `this`)
 * ============================================================ */

enum { IF_DDRAW, IF_SURFACE, IF_PALETTE, IF_COUNT };

static const char* const k_dd_names[] = {
    "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper",
    "CreatePalette", "CreateSurface", "DuplicateSurface", "EnumDisplayModes",
    "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode",
    "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine",
    "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode",
    "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
};
static const uint8_t k_dd_argc[] = {
    3, 1, 1, 1, 4, 5, 4, 3, 5, 5, 1, 3, 2, 3, 2, 2, 2, 2, 2, 1, 3, 4, 3,
};

static const char* const k_surf_names[] = {
    "QueryInterface", "AddRef", "Release", "AddAttachedSurface",
    "AddOverlayDirtyRect", "Blt", "BltBatch", "BltFast",
    "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders",
    "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper",
    "GetColorKey", "GetDC", "GetFlipStatus", "GetOverlayPosition", "GetPalette",
    "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
    "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition",
    "SetPalette", "Unlock", "UpdateOverlay", "UpdateOverlayDisplay",
    "UpdateOverlayZOrder",
};
static const uint8_t k_surf_argc[] = {
    3, 1, 1, 2, 2, 6, 4, 6, 3, 3, 4, 3, 3, 2, 2, 2, 3, 2, 2, 3, 2, 2, 2, 3,
    1, 5, 2, 1, 2, 3, 3, 2, 2, 6, 2, 3,
};

static const char* const k_pal_names[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries",
    "Initialize", "SetEntries",
};
static const uint8_t k_pal_argc[] = { 3, 1, 1, 2, 5, 4, 5 };

static const struct {
    const char* name; const char* const* names; const uint8_t* argc; int n;
} k_if[IF_COUNT] = {
    { "IDirectDraw",        k_dd_names,   k_dd_argc,   23 },
    { "IDirectDrawSurface", k_surf_names, k_surf_argc, 36 },
    { "IDirectDrawPalette", k_pal_names,  k_pal_argc,  7  },
};

static uint32_t g_vtbl[IF_COUNT];      /* simulated VA of each vtable */
static uint32_t g_thunk_first = 0;     /* host thunk id of IF_DDRAW slot 0 */

/* ============================================================
 * Objects
 * ============================================================ */

typedef struct {
    uint32_t va;            /* the simulated COM object */
    int      kind;
    /* surface */
    uint32_t w, h, bpp, pitch, bits;
    int      primary;
    int      back;          /* index of the attached back buffer, or -1 */
    int      pal;           /* index of the attached palette, or -1 */
    /* palette */
    RGBQUAD  entries[256];
} obj_t;

#define MAX_OBJ 64
static obj_t    g_objs[MAX_OBJ];
static int      g_nobj = 0;
static uint32_t g_obj_pool = 0, g_obj_pool_used = 0;

static struct {
    HWND     hwnd;
    uint32_t w, h, bpp;
    int      primary;       /* index of the primary surface, or -1 */
} g_dd = { NULL, 640, 480, 32, -1 };

static void build_vtables(void) {
    uint32_t mem = shims_alloc(4096);
    for (int f = 0; f < IF_COUNT; f++) {
        g_vtbl[f] = mem;
        mem += 4u * (uint32_t)k_if[f].n;
    }
    g_obj_pool = mem;
}

static void com_dispatch(void);

static void build_thunks(void) {
    build_vtables();
    for (int f = 0; f < IF_COUNT; f++)
        for (int m = 0; m < k_if[f].n; m++) {
            uint32_t t = recomp_host_thunk(com_dispatch);
            if (f == 0 && m == 0) g_thunk_first = (t - 0xFFF00000u) / 4u;
            MEM32(g_vtbl[f] + 4u * (uint32_t)m) = t;
        }
}

static int obj_new(int kind) {
    if (g_nobj >= MAX_OBJ) return -1;
    int i = g_nobj++;
    obj_t* o = &g_objs[i];
    memset(o, 0, sizeof(*o));
    o->kind = kind; o->back = -1; o->pal = -1;
    o->va = g_obj_pool + 8u * (uint32_t)i;
    MEM32(o->va) = g_vtbl[kind];
    MEM32(o->va + 4) = (uint32_t)i;
    return i;
}

static obj_t* obj_of(uint32_t va) {
    if (!va) return NULL;
    uint32_t i = MEM32(va + 4);
    return i < (uint32_t)g_nobj && g_objs[i].va == va ? &g_objs[i] : NULL;
}

/* ============================================================
 * Presentation
 * ============================================================ */

/* Size the window's client area to the mode, scaled up by the largest integer
 * that still fits the desktop. The game created a borderless popup for an
 * exclusive fullscreen mode; a captioned window is friendlier on a modern
 * desktop and changes nothing the engine can see. */
static void fit_window(void);

/* The renderer sets its own mode on the same window. */
void video_fit_window(HWND hwnd, uint32_t w, uint32_t h) {
    if (hwnd) g_dd.hwnd = hwnd;
    g_dd.w = w; g_dd.h = h;
    fit_window();
}

static void fit_window(void) {
    if (!g_dd.hwnd) return;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int scale = 1;
    while ((scale + 1) * (int)g_dd.w <= sw * 9 / 10 &&
           (scale + 1) * (int)g_dd.h <= sh * 9 / 10) scale++;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    RECT r = { 0, 0, (LONG)g_dd.w * scale, (LONG)g_dd.h * scale };
    AdjustWindowRect(&r, style, FALSE);
    SetWindowLongA(g_dd.hwnd, GWL_STYLE, (LONG)style);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    SetWindowPos(g_dd.hwnd, HWND_TOP, (sw - ww) / 2, (sh - wh) / 2, ww, wh,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static void present(obj_t* s) {
    if (!g_dd.hwnd || !s || !s->bits) return;
    struct { BITMAPINFOHEADER h; DWORD c[256]; } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(bi.h);
    bi.h.biWidth = (LONG)s->w;
    bi.h.biHeight = -(LONG)s->h;             /* top-down */
    bi.h.biPlanes = 1;
    bi.h.biBitCount = (WORD)s->bpp;
    bi.h.biCompression = BI_RGB;
    if (s->bpp == 8) {
        obj_t* p = g_dd.primary >= 0 && g_objs[g_dd.primary].pal >= 0
                 ? &g_objs[g_objs[g_dd.primary].pal] : NULL;
        if (p) memcpy(bi.c, p->entries, sizeof(p->entries));
        bi.h.biClrUsed = 256;
    } else if (s->bpp == 16) {
        bi.h.biCompression = BI_BITFIELDS;
        bi.c[0] = 0xF800; bi.c[1] = 0x07E0; bi.c[2] = 0x001F;
    }
    /* RECOMP_SHOT=<dir>: dump every 30th presented frame as a BMP, so a run
     * without a screen to look at still shows what the game drew. */
    static int frame = 0;
    /* RECOMP_TRACE builds: show how the game got into its present loop. */
    int nframe = frame++;
    static int dumped = 0;
    if (s->bpp == 32 && ++dumped == 5) recomp_dump_trace("present loop");
    const char* shot = getenv("RECOMP_SHOT");
    if (shot && nframe % 30 == 0) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\frame_%05d.bmp", shot, nframe);
        FILE* f = fopen(path, "wb");
        if (f) {
            uint32_t hdr = (uint32_t)sizeof(bi), img = s->pitch * s->h;
            BITMAPFILEHEADER fh = { 0x4D42, 14 + hdr + img, 0, 0, 14 + hdr };
            fwrite(&fh, 14, 1, f);
            fwrite(&bi, hdr, 1, f);
            fwrite((void*)ADDR(s->bits), img, 1, f);
            fclose(f);
        }
    }
    RECT rc;
    GetClientRect(g_dd.hwnd, &rc);
    HDC dc = GetDC(g_dd.hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, (int)s->w, (int)s->h,
                  (void*)ADDR(s->bits), (BITMAPINFO*)&bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_dd.hwnd, dc);
}

/* RECOMP_KEYS="ms:vk,ms:vk,...": post key presses to the game window at those
 * times after it gets its display mode -- scripted input for driving menus
 * from a run with no desktop, e.g. "3000:13,6000:13" presses Enter twice. */
static DWORD WINAPI key_script(LPVOID hwnd) {
    const char* p = getenv("RECOMP_KEYS");
    DWORD t0 = GetTickCount();
    while (p && *p) {
        unsigned ms = 0, vk = 0;
        if (sscanf(p, "%u:%u", &ms, &vk) != 2) break;
        while (GetTickCount() - t0 < ms) Sleep(10);
        UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE)
            sc |= 0x100;                /* extended: the engine keys on lParam>>16 & 0x1FF */
        fprintf(stderr, "[keys] vk %u\n", vk);
        PostMessageA((HWND)hwnd, WM_KEYDOWN, vk, 1 | (sc << 16));
        Sleep(80);
        PostMessageA((HWND)hwnd, WM_KEYUP, vk, 1 | (sc << 16) | 0xC0000000u);
        p = strchr(p, ',');
        if (p) p++;
    }
    return 0;
}

static void start_key_script(HWND hwnd) {
    static int started = 0;
    if (!started++ && hwnd && getenv("RECOMP_KEYS"))
        CreateThread(NULL, 0, key_script, hwnd, 0, NULL);
}

/* ============================================================
 * Methods
 * ============================================================ */

/* DDSURFACEDESC (v1), 0x6C bytes. */
#define SD_FLAGS      0x04
#define SD_HEIGHT     0x08
#define SD_WIDTH      0x0C
#define SD_PITCH      0x10
#define SD_BACKCOUNT  0x14
#define SD_SURFACE    0x24
#define SD_PIXFMT     0x48
#define SD_CAPS       0x68
#define DDSD_CAPS     0x1u
#define DDSD_HEIGHT   0x2u
#define DDSD_WIDTH    0x4u
#define DDSD_PITCH    0x8u
#define DDSD_BACKBUFFERCOUNT 0x20u
#define DDSD_LPSURFACE 0x800u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSCAPS_BACKBUFFER     0x4u
#define DDSCAPS_PRIMARYSURFACE 0x200u

static int surface_new(uint32_t w, uint32_t h, uint32_t bpp) {
    int i = obj_new(IF_SURFACE);
    if (i < 0) return -1;
    obj_t* s = &g_objs[i];
    s->w = w; s->h = h; s->bpp = bpp;
    s->pitch = (w * bpp / 8 + 3) & ~3u;
    s->bits = shims_alloc(s->pitch * h);
    return s->bits ? i : -1;
}

static void pixfmt_out(uint32_t pf, uint32_t bpp) {
    memset((void*)ADDR(pf), 0, 32);
    MEM32(pf + 0) = 32;
    MEM32(pf + 4) = bpp == 8 ? 0x60u : 0x40u;   /* PALETTEINDEXED8|RGB : RGB */
    MEM32(pf + 12) = bpp;
    if (bpp == 16)      { MEM32(pf + 16) = 0xF800; MEM32(pf + 20) = 0x07E0; MEM32(pf + 24) = 0x001F; }
    else if (bpp >= 24) { MEM32(pf + 16) = 0xFF0000; MEM32(pf + 20) = 0xFF00; MEM32(pf + 24) = 0xFF; }
}

static void desc_out(uint32_t d, obj_t* s) {
    memset((void*)ADDR(d), 0, 0x6C);
    MEM32(d) = 0x6C;
    MEM32(d + SD_FLAGS) = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH |
                          DDSD_PIXELFORMAT | DDSD_LPSURFACE;
    MEM32(d + SD_HEIGHT) = s->h;
    MEM32(d + SD_WIDTH) = s->w;
    MEM32(d + SD_PITCH) = s->pitch;
    MEM32(d + SD_SURFACE) = s->bits;
    pixfmt_out(d + SD_PIXFMT, s->bpp);
    MEM32(d + SD_CAPS) = s->primary ? DDSCAPS_PRIMARYSURFACE : 0;
}

static uint32_t dd_method(int m) {
    switch (m) {
    case 1: return 2;                                   /* AddRef */
    case 2: return 0;                                   /* Release */
    case 5: {                                           /* CreatePalette */
        int i = obj_new(IF_PALETTE);
        if (i < 0) return DDERR_GENERIC;
        uint32_t src = ARG(2);
        if (src)
            for (int k = 0; k < 256; k++) {
                uint32_t e = MEM32(src + 4u * (uint32_t)k);    /* PALETTEENTRY r,g,b,f */
                g_objs[i].entries[k].rgbRed   = (BYTE)(e);
                g_objs[i].entries[k].rgbGreen = (BYTE)(e >> 8);
                g_objs[i].entries[k].rgbBlue  = (BYTE)(e >> 16);
            }
        MEM32(ARG(3)) = g_objs[i].va;
        return DD_OK;
    }
    case 6: {                                           /* CreateSurface */
        uint32_t d = ARG(1), fl = MEM32(d + SD_FLAGS), caps = MEM32(d + SD_CAPS);
        if (caps & DDSCAPS_PRIMARYSURFACE) {
            int p = surface_new(g_dd.w, g_dd.h, g_dd.bpp);
            if (p < 0) return DDERR_GENERIC;
            g_objs[p].primary = 1;
            g_dd.primary = p;
            if ((fl & DDSD_BACKBUFFERCOUNT) && MEM32(d + SD_BACKCOUNT)) {
                int b = surface_new(g_dd.w, g_dd.h, g_dd.bpp);
                if (b < 0) return DDERR_GENERIC;
                g_objs[p].back = b;
            }
            MEM32(ARG(2)) = g_objs[p].va;
            fprintf(stderr, "[ddraw] primary %ux%ux%u, %s back buffer\n",
                    g_dd.w, g_dd.h, g_dd.bpp, g_objs[p].back >= 0 ? "with" : "no");
            return DD_OK;
        }
        uint32_t w = (fl & DDSD_WIDTH) ? MEM32(d + SD_WIDTH) : g_dd.w;
        uint32_t h = (fl & DDSD_HEIGHT) ? MEM32(d + SD_HEIGHT) : g_dd.h;
        uint32_t bpp = (fl & DDSD_PIXELFORMAT) ? MEM32(d + SD_PIXFMT + 12) : g_dd.bpp;
        int s = surface_new(w, h, bpp);
        if (s < 0) return DDERR_GENERIC;
        MEM32(ARG(2)) = g_objs[s].va;
        return DD_OK;
    }
    case 11: {                                          /* GetCaps: DDCAPS dwSize */
        if (ARG(1)) memset((void*)ADDR(ARG(1)), 0, MEM32(ARG(1)));
        if (ARG(2)) memset((void*)ADDR(ARG(2)), 0, MEM32(ARG(2)));
        return DD_OK;
    }
    case 12: {                                          /* GetDisplayMode */
        obj_t tmp; memset(&tmp, 0, sizeof(tmp));
        tmp.w = g_dd.w; tmp.h = g_dd.h; tmp.bpp = g_dd.bpp;
        tmp.pitch = g_dd.w * g_dd.bpp / 8;
        desc_out(ARG(1), &tmp);
        return DD_OK;
    }
    case 10: case 19: case 22: return DD_OK;    /* FlipToGDI, RestoreDisplayMode, WaitVB */
    case 20:                                            /* SetCooperativeLevel */
        g_dd.hwnd = shims_hwnd(ARG(1));
        start_key_script(g_dd.hwnd);
        return DD_OK;
    case 21:                                            /* SetDisplayMode */
        g_dd.w = ARG(1); g_dd.h = ARG(2); g_dd.bpp = ARG(3);
        fprintf(stderr, "[ddraw] SetDisplayMode %ux%ux%u\n", g_dd.w, g_dd.h, g_dd.bpp);
        fit_window();
        return DD_OK;
    }
    return DDERR_UNSUPPORTED;
}

static uint32_t surf_method(obj_t* s, int m) {
    switch (m) {
    case 1: return 2;
    case 2: return 0;
    case 11:                                            /* Flip */
        present(s->back >= 0 ? &g_objs[s->back] : s);
        return DD_OK;
    case 12:                                            /* GetAttachedSurface */
        if (s->back < 0) return DDERR_NOTFOUND;
        MEM32(ARG(2)) = g_objs[s->back].va;
        return DD_OK;
    case 21: pixfmt_out(ARG(1), s->bpp); return DD_OK;  /* GetPixelFormat */
    case 22: desc_out(ARG(1), s); return DD_OK;         /* GetSurfaceDesc */
    case 24: case 27: return DD_OK;                     /* IsLost, Restore */
    case 25: desc_out(ARG(2), s); return DD_OK;         /* Lock */
    case 32:                                            /* Unlock */
        /* In 32-bit modes wddvmem copies its frame straight into the primary
         * and never flips, so the primary's unlock is the present. */
        if (s->primary) present(s);
        return DD_OK;
    case 31: {                                          /* SetPalette */
        obj_t* p = obj_of(ARG(1));
        s->pal = p ? (int)(p - g_objs) : -1;
        return DD_OK;
    }
    case 29: return DD_OK;                              /* SetColorKey */
    }
    return DDERR_UNSUPPORTED;
}

static uint32_t pal_method(obj_t* p, int m) {
    switch (m) {
    case 1: return 2;
    case 2: return 0;
    case 4: case 6: {                                   /* GetEntries, SetEntries */
        uint32_t start = ARG(2), n = ARG(3), e = ARG(4);
        for (uint32_t k = 0; k < n && start + k < 256; k++) {
            RGBQUAD* q = &p->entries[start + k];
            if (m == 6) {
                uint32_t v = MEM32(e + 4u * k);
                q->rgbRed = (BYTE)v; q->rgbGreen = (BYTE)(v >> 8); q->rgbBlue = (BYTE)(v >> 16);
            } else {
                MEM32(e + 4u * k) = q->rgbRed | (q->rgbGreen << 8) | (q->rgbBlue << 16);
            }
        }
        if (m == 6 && g_dd.primary >= 0 && g_dd.bpp == 8) {
            obj_t* pr = &g_objs[g_dd.primary];
            present(pr->back >= 0 ? &g_objs[pr->back] : pr);
        }
        return DD_OK;
    }
    }
    return DDERR_UNSUPPORTED;
}

static void com_dispatch(void) {
    uint32_t id = g_host_thunk_id - g_thunk_first;
    int f = 0, m = (int)id;
    while (f < IF_COUNT && m >= k_if[f].n) { m -= k_if[f].n; f++; }
    uint32_t r = DDERR_GENERIC;
    obj_t* o = obj_of(ARG(0));
    if (m == 0) {
        r = E_NOINTERFACE_;                             /* v1 is all we offer */
    } else if (o) {
        r = f == IF_DDRAW   ? dd_method(m)
          : f == IF_SURFACE ? surf_method(o, m)
          :                   pal_method(o, m);
    }
    if (r == DDERR_UNSUPPORTED) {
        static uint8_t warned[IF_COUNT][40];
        if (!warned[f][m]++)
            fprintf(stderr, "[ddraw] unimplemented %s::%s\n", k_if[f].name, k_if[f].names[m]);
    }
    RET(r);
    STDRET(k_if[f].argc[m]);
}

/* DirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*) */
uint32_t video_ddraw_create(uint32_t out) {
    if (!g_vtbl[0]) build_thunks();
    int i = obj_new(IF_DDRAW);
    if (i < 0 || !out) return DDERR_GENERIC;
    MEM32(out) = g_objs[i].va;
    return DD_OK;
}

/* The game window, as DirectDraw last saw it (the renderer falls back to it). */
HWND video_window(void) { return g_dd.hwnd; }
