/*
 * Nocturne Recompilation - the renderer DLL, reimplemented natively.
 *
 * nocturne.ini names a renderer DLL (tridx7.dll) exporting 37 cdecl APIDLL*
 * functions. Those DLLs are 32-bit DirectX 6 code and cannot load into this
 * x64 process, so LoadLibraryA hands back a token and GetProcAddress hands back
 * host thunks into the functions below. docs/RENDERER_API.md is the contract;
 * section numbers in comments refer to it.
 *
 * Direct3D 9 fixed function is the closest living relative of what tridx7
 * drives: the same pre-transformed vertex (FVF 0x1c4), the same render states,
 * vertex fog through specular alpha. So the port is mostly transcription.
 *
 * The one real translation is frame access. The exe draws its 2D (HUD, menus,
 * movies) by writing into the back buffer through its own row table, and every
 * address it holds is a 32-bit simulated one. lockFrame therefore reads the
 * back buffer back into an arena buffer and points the row table at that;
 * unlockFrame uploads it again.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"

uint32_t recomp_host_thunk(recomp_func_t f);
uint32_t shims_alloc(uint32_t size);
HWND shims_hwnd(uint32_t v);
void video_fit_window(HWND hwnd, uint32_t w, uint32_t h);
HWND video_window(void);

/* ============================================================
 * State
 * ============================================================ */

static struct {
    HWND     hwnd;
    uint32_t table;             /* APIDLLinit pointer table (sim VA), §2 */
    uint32_t w, h;
    uint32_t row_table;         /* exe row table (sim VA) */
    uint32_t saved_rows;        /* its original contents (sim VA) */
    uint32_t frame;             /* arena copy of the back buffer while locked */
    int      locked;
    int      in_scene;
    uint32_t pal;               /* setColorTable16 palette (sim VA), for untextured colour */
    uint32_t fog_rgb;

    IDirect3D9*        d3d;
    IDirect3DDevice9*  dev;
    IDirect3DSurface9* back;
    IDirect3DSurface9* depth;
    IDirect3DSurface9* sysmem;  /* readback/upload staging for lockFrame */
    IDirect3DSurface9* master;  /* masterZBuffer(0) */

    IDirect3DTexture9* cur_tex;
    int                cur_alpha;   /* the last select/update came with an alpha plane */
    uint32_t           state_key[4];
    int                state_valid;
} R;

/* Pointer-table entries (§2): each is the VA of an exe global int. */
#define T_BLEND      0x24
#define T_FLATLIGHT  0x28
#define T_ALPHA      0x2c
#define T_COLORIDX   0x30
#define T_TEXSIZE    0x48
#define T_FARZ       0x50
#define T_DITHER     0x54
#define T_FILTER     0x60
#define T_ZLINEAR    0x68

static uint32_t tget(uint32_t off)             { return MEM32(MEM32(R.table + off)); }
static void     tset(uint32_t off, uint32_t v) { MEM32(MEM32(R.table + off)) = v; }

/* ============================================================
 * Batch (§4 Drawing)
 * ============================================================ */

typedef struct { float x, y, z, rhw; DWORD color, spec; float u, v; } tlv_t;
#define TLV_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)

#define BATCH_MAX 16000
static tlv_t  g_vb[BATCH_MAX];
static WORD   g_ib[BATCH_MAX * 3];
static int    g_nv = 0, g_ni = 0;

static void flush(void) {
    if (!g_ni || !R.dev) { g_nv = g_ni = 0; return; }
    /* One D3D9 scene per batch: it keeps every StretchRect, GetRenderTargetData
     * and UpdateSurface below safely outside BeginScene/EndScene. */
    IDirect3DDevice9_BeginScene(R.dev);
    IDirect3DDevice9_DrawIndexedPrimitiveUP(R.dev, D3DPT_TRIANGLELIST, 0, (UINT)g_nv,
                                            (UINT)g_ni / 3, g_ib, D3DFMT_INDEX16,
                                            g_vb, sizeof(tlv_t));
    IDirect3DDevice9_EndScene(R.dev);
    g_nv = g_ni = 0;
}

/* ============================================================
 * Render state from polygon flags (§7)
 * ============================================================ */

#define RS(s, v) IDirect3DDevice9_SetRenderState(R.dev, (s), (DWORD)(v))
#define TSS(s, v) IDirect3DDevice9_SetTextureStageState(R.dev, 0, (s), (DWORD)(v))

static uint32_t effective_flags(uint32_t flags) {
    if ((flags & 1) && R.cur_alpha) flags |= 0x22;
    if (flags & 0x20) flags &= ~8u;
    return flags;
}

static void apply_state(uint32_t flags) {
    uint32_t key[4] = { flags, tget(T_BLEND), (uint32_t)(uintptr_t)R.cur_tex, tget(T_FILTER) };
    if (R.state_valid && !memcmp(key, R.state_key, sizeof(key))) return;
    flush();
    memcpy(R.state_key, key, sizeof(key));
    R.state_valid = 1;

    int textured = flags & 1;
    if (textured) {
        IDirect3DDevice9_SetTexture(R.dev, 0, (IDirect3DBaseTexture9*)R.cur_tex);
        TSS(D3DTSS_COLOROP, D3DTOP_MODULATE);
        TSS(D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        DWORD f = tget(T_FILTER) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        IDirect3DDevice9_SetSamplerState(R.dev, 0, D3DSAMP_MAGFILTER, f);
        IDirect3DDevice9_SetSamplerState(R.dev, 0, D3DSAMP_MINFILTER, f);
    } else {
        IDirect3DDevice9_SetTexture(R.dev, 0, NULL);
        TSS(D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        TSS(D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    }
    TSS(D3DTSS_COLORARG1, textured ? D3DTA_TEXTURE : D3DTA_DIFFUSE);
    TSS(D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    TSS(D3DTSS_ALPHAARG1, textured ? D3DTA_TEXTURE : D3DTA_DIFFUSE);
    TSS(D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    int blend = (flags & 2) != 0;
    RS(D3DRS_ALPHABLENDENABLE, blend);
    RS(D3DRS_ALPHATESTENABLE, blend);
    RS(D3DRS_SRCBLEND, (flags & 0x20) ? D3DBLEND_SRCALPHA : D3DBLEND_ONE);
    RS(D3DRS_DESTBLEND, tget(T_BLEND) == 1 ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    RS(D3DRS_SHADEMODE, (flags & 4) ? D3DSHADE_GOURAUD : D3DSHADE_FLAT);
    RS(D3DRS_FOGENABLE, (flags & 8) != 0);

    uint32_t z = flags & 0xC0;
    RS(D3DRS_ZENABLE, z ? D3DZB_TRUE : D3DZB_FALSE);
    RS(D3DRS_ZWRITEENABLE, (z & 0x80) != 0);
    RS(D3DRS_ZFUNC, (z & 0x40) ? D3DCMP_LESSEQUAL : D3DCMP_ALWAYS);
}

static void initial_state(void) {
    RS(D3DRS_LIGHTING, FALSE);
    RS(D3DRS_CULLMODE, D3DCULL_NONE);
    RS(D3DRS_SPECULARENABLE, TRUE);
    RS(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    RS(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    RS(D3DRS_FOGCOLOR, R.fog_rgb);
    RS(D3DRS_ALPHAREF, 0);
    RS(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    RS(D3DRS_DITHERENABLE, tget(T_DITHER) != 0);
    IDirect3DDevice9_SetSamplerState(R.dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(R.dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetFVF(R.dev, TLV_FVF);
    R.state_valid = 0;
}

/* endScene/init reset (0x10002d50): force a known state, leave the exe's
 * alpha and blend globals at their defaults. */
static void reset_state(void) {
    flush();
    R.state_valid = 0;
    tset(T_ALPHA, 0xFF);
    tset(T_BLEND, 0);
}

/* ============================================================
 * Vertex conversion (§6.2, §6.3)
 * ============================================================ */

static float depth_of(int32_t z) {
    float far_z = (float)(int32_t)tget(T_FARZ);
    if (far_z <= 0) far_z = 1;
    if (tget(T_ZLINEAR)) {
        float s = (float)z / far_z;
        return s > 1.0f ? 1.0f : s;
    }
    float t = (float)z * 256.0f / far_z;
    if (t < 1) t = 1;
    if (t > 256) t = 256;
    return 1.0f - 1.0f / t;
}

/* Per-polygon light when the vertex does not carry one (no flag 4). */
static void flat_light(uint32_t flags, int* L, int* O) {
    *L = 255; *O = 0;
    if ((flags & 0x10) && !(flags & 0x200)) {
        int l = ((int32_t)tget(T_FLATLIGHT) - 0x100) >> 4;
        if (l > 255) { *O = l - 256 > 255 ? 255 : l - 256; l = 255; }
        *L = l < 0 ? 0 : l;
    }
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void convert(uint32_t va, float rhw, uint32_t flags, int fL, int fO, tlv_t* o) {
    int32_t sx = (int32_t)MEM32(va + 0x10), sy = (int32_t)MEM32(va + 0x14);
    int32_t z = (int32_t)MEM32(va + 0x08);
    int32_t u = (int32_t)MEM32(va + 0x18), v = (int32_t)MEM32(va + 0x1c);
    int32_t lt = (int32_t)MEM32(va + 0x20), g = (int32_t)MEM32(va + 0x24);
    int32_t b = (int32_t)MEM32(va + 0x28), af = (int32_t)MEM32(va + 0x2c);

    o->x = sx / 65536.0f;
    o->y = sy / 65536.0f;
    o->z = depth_of(z);
    o->rhw = rhw;
    o->u = u / 16777216.0f;
    o->v = v / 16777216.0f;

    int F = (flags & 8) ? clamp255(255 - (af >> 8)) : 255;
    int L = fL, O = fO;
    if (flags & 0x200) { L = 255; O = 0; }
    else if (flags & 4) {
        L = (lt - 0x100) >> 4;
        O = 0;
        if (L > 255) { O = L - 256 > 255 ? 255 : L - 256; L = 255; }
        L = clamp255(L);
    }
    uint32_t rgb_vtx = (uint32_t)clamp255(lt >> 8) << 16 | (uint32_t)clamp255(g >> 8) << 8
                     | (uint32_t)clamp255(b >> 8);

    if (flags & 1) {
        int A = (flags & 0x100) ? clamp255(af >> 8) : (int)(tget(T_ALPHA) & 0xFF);
        o->spec = (DWORD)F << 24 | (DWORD)O << 16 | (DWORD)O << 8 | (DWORD)O;
        o->color = (flags & 0x200) ? ((DWORD)A << 24 | rgb_vtx)
                 : ((DWORD)A << 24 | (DWORD)L << 16 | (DWORD)L << 8 | (DWORD)L);
    } else {
        o->spec = (DWORD)F << 24;
        if (flags & 0x200) {
            o->color = 0xFF000000u | rgb_vtx;
        } else {
            uint32_t idx = (flags & 4) ? ((uint32_t)u >> 16) & 0xFF : tget(T_COLORIDX) & 0xFF;
            const uint8_t* p = R.pal ? (const uint8_t*)ADDR(R.pal) + idx * 3 : NULL;
            o->color = 0xFF000000u | (p ? ((DWORD)p[0] << 16 | (DWORD)p[1] << 8 | p[2]) : 0);
        }
    }
}

static int room(int nv, int ni) {
    if (g_nv + nv > BATCH_MAX || g_ni + ni > BATCH_MAX * 3) flush();
    return nv <= BATCH_MAX && ni <= BATCH_MAX * 3;
}

/* A polygon as a triangle fan over n vertex VAs, rhw = maxZ/z. */
static int draw_poly(const uint32_t* vas, int n, uint32_t flags) {
    if (!R.in_scene || !R.dev || n < 3) return 0;
    flags = effective_flags(flags);
    apply_state(flags);
    int32_t maxz = 1;
    for (int i = 0; i < n; i++) {
        int32_t z = (int32_t)MEM32(vas[i] + 8);
        if (z > maxz) maxz = z;
    }
    if (!room(n, (n - 2) * 3)) return 0;
    int fL, fO;
    flat_light(flags, &fL, &fO);
    int base = g_nv;
    for (int i = 0; i < n; i++) {
        int32_t z = (int32_t)MEM32(vas[i] + 8);
        convert(vas[i], (float)maxz / (float)(z > 0 ? z : 1), flags, fL, fO, &g_vb[g_nv++]);
    }
    for (int i = 0; i < n - 2; i++) {
        g_ib[g_ni++] = (WORD)base;
        g_ib[g_ni++] = (WORD)(base + i + 1);
        g_ib[g_ni++] = (WORD)(base + i + 2);
    }
    return 1;
}

/* One poly-list triangle/fan: vertex VA, 8.24 u/v written back into the exe
 * vertex first (the DLL does, §4), rhw = 256/z. */
static void list_vertex(uint32_t va, int32_t u, int32_t v, uint32_t flags, int fL, int fO) {
    MEM32(va + 0x18) = (uint32_t)u;
    MEM32(va + 0x1c) = (uint32_t)v;
    int32_t z = (int32_t)MEM32(va + 8);
    convert(va, 256.0f / (float)(z > 0 ? z : 1), flags, fL, fO, &g_vb[g_nv++]);
}

/* ============================================================
 * Device
 * ============================================================ */

static void release(IUnknown** p) { if (*p) { IUnknown_Release(*p); *p = NULL; } }
#define REL(p) release((IUnknown**)&(p))

static void drop_device(void) {
    g_nv = g_ni = 0;
    REL(R.master); REL(R.sysmem); REL(R.depth); REL(R.back); REL(R.dev);
    R.cur_tex = NULL;
}

static void clear_texture_cache(void);

static int make_device(uint32_t w, uint32_t h) {
    if (!R.d3d) R.d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!R.d3d) { fprintf(stderr, "[renderer] Direct3DCreate9 failed\n"); return 0; }
    clear_texture_cache();
    drop_device();
    if (!R.hwnd) R.hwnd = video_window();
    video_fit_window(R.hwnd, w, h);

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = R.hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24X8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    D3DDISPLAYMODE dm;
    IDirect3D9_GetAdapterDisplayMode(R.d3d, D3DADAPTER_DEFAULT, &dm);
    /* Pre-transformed vertices never touch the vertex pipeline, so software
     * vertex processing costs nothing and is accepted more widely. */
    DWORD flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    HRESULT hr = IDirect3D9_CreateDevice(R.d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, R.hwnd,
                                         flags, &pp, &R.dev);
    if (FAILED(hr)) {
        fprintf(stderr, "[renderer] CreateDevice %ux%u failed (%08lX), desktop %ux%u fmt %d, "
                "adapters %u\n", w, h, hr, dm.Width, dm.Height, dm.Format,
                IDirect3D9_GetAdapterCount(R.d3d));
        return 0;
    }
    IDirect3DDevice9_GetBackBuffer(R.dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &R.back);
    IDirect3DDevice9_GetDepthStencilSurface(R.dev, &R.depth);
    IDirect3DDevice9_CreateOffscreenPlainSurface(R.dev, w, h, D3DFMT_X8R8G8B8,
                                                 D3DPOOL_SYSTEMMEM, &R.sysmem, NULL);
    IDirect3DDevice9_CreateDepthStencilSurface(R.dev, w, h, D3DFMT_D24X8,
                                               D3DMULTISAMPLE_NONE, 0, FALSE, &R.master, NULL);
    R.w = w; R.h = h;
    initial_state();
    IDirect3DDevice9_Clear(R.dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    fprintf(stderr, "[renderer] D3D9 device %ux%u\n", w, h);
    return 1;
}

/* ============================================================
 * Textures (§4 Textures): one D3D texture per (size, name)
 * ============================================================ */

typedef struct tex_s {
    char name[64];
    uint32_t size;
    IDirect3DTexture9* t;
    struct tex_s* next;
} tex_t;

#define TEX_HASH 1024
static tex_t* g_tex[TEX_HASH];

static void clear_texture_cache(void) {
    for (int i = 0; i < TEX_HASH; i++)
        while (g_tex[i]) { tex_t* n = g_tex[i]->next; REL(g_tex[i]->t); free(g_tex[i]); g_tex[i] = n; }
}

static tex_t* tex_find(const char* name, uint32_t size, int create) {
    uint32_t h = size;
    for (const char* c = name; *c; c++) h = h * 31u + (uint8_t)*c;
    h %= TEX_HASH;
    for (tex_t* t = g_tex[h]; t; t = t->next)
        if (t->size == size && !strcmp(t->name, name)) return t;
    if (!create) return NULL;
    tex_t* t = calloc(1, sizeof(*t));
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->size = size;
    t->next = g_tex[h];
    g_tex[h] = t;
    return t;
}

static void tex_upload(tex_t* t, uint32_t pixels, uint32_t pal, uint32_t alpha) {
    uint32_t n = t->size;
    if (!t->t && FAILED(IDirect3DDevice9_CreateTexture(R.dev, n, n, 1, 0, D3DFMT_A8R8G8B8,
                                                       D3DPOOL_MANAGED, &t->t, NULL)))
        return;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(t->t, 0, &lr, NULL, 0))) return;
    const uint8_t* px = (const uint8_t*)ADDR(pixels);
    const uint8_t* pl = (const uint8_t*)ADDR(pal);
    const uint8_t* al = alpha ? (const uint8_t*)ADDR(alpha) : NULL;
    for (uint32_t y = 0; y < n; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)lr.pBits + y * (uint32_t)lr.Pitch);
        for (uint32_t x = 0; x < n; x++) {
            uint32_t i = px[y * n + x];
            uint32_t rgb = (uint32_t)pl[i * 3] << 16 | (uint32_t)pl[i * 3 + 1] << 8 | pl[i * 3 + 2];
            row[x] = al ? ((uint32_t)al[y * n + x] << 24 | rgb)
                        : (rgb ? 0xFF000000u | rgb : 0);   /* palette black is transparent */
        }
    }
    IDirect3DTexture9_UnlockRect(t->t, 0);
}

static void texture_call(int always_upload) {
    uint32_t key = ARG(0), pixels = ARG(2), pal = ARG(3), alpha = ARG(4);
    uint32_t size = tget(T_TEXSIZE);
    RET(1);
    if (!R.dev || !key || !pixels || !pal) return;
    const char* name = (const char*)ADDR(key + 8);
    tex_t* t = tex_find(name, size, 1);
    int fresh = !t->t;
    if (fresh || always_upload) {
        flush();
        tex_upload(t, pixels, pal, alpha);
    }
    R.cur_tex = t->t;
    R.cur_alpha = alpha != 0;
}

/* ============================================================
 * Frame access
 * ============================================================ */

static int lock_frame(void) {
    if (!R.dev || R.locked) return R.locked;
    if (R.in_scene) { reset_state(); R.in_scene = 0; }
    flush();
    uint32_t pitch = R.w * 4;
    if (!R.frame) R.frame = shims_alloc(pitch * R.h);
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(R.dev, R.back, R.sysmem)) ||
        FAILED(IDirect3DSurface9_LockRect(R.sysmem, &lr, NULL, D3DLOCK_READONLY)))
        return 0;
    for (uint32_t y = 0; y < R.h; y++)
        memcpy((uint8_t*)ADDR(R.frame + y * pitch), (uint8_t*)lr.pBits + y * (uint32_t)lr.Pitch, pitch);
    IDirect3DSurface9_UnlockRect(R.sysmem);
    for (uint32_t y = 0; y < R.h; y++)
        MEM32(R.row_table + 4 * y) = R.frame + y * pitch;
    R.locked = 1;
    return 1;
}

static int unlock_frame(void) {
    if (!R.locked) return 0;
    R.locked = 0;
    uint32_t pitch = R.w * 4;
    memcpy((void*)ADDR(R.row_table), (void*)ADDR(R.saved_rows), 4 * R.h);
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(R.sysmem, &lr, NULL, 0))) return 1;
    for (uint32_t y = 0; y < R.h; y++)
        memcpy((uint8_t*)lr.pBits + y * (uint32_t)lr.Pitch, (uint8_t*)ADDR(R.frame + y * pitch), pitch);
    IDirect3DSurface9_UnlockRect(R.sysmem);
    IDirect3DDevice9_UpdateSurface(R.dev, R.sysmem, NULL, R.back, NULL);
    return 1;
}

/* ============================================================
 * The 37 exports (all cdecl: pop only the return address)
 * ============================================================ */

#define API(name) static void name(void)
#define DONE(v) do { RET(v); STDRET(0); } while (0)

API(APIDLLInformation) {
    uint32_t out = ARG(1);
    fprintf(stderr, "[renderer] Information(%08X, %08X)\n", ARG(0), out);
    if (out) {
        memset((void*)ADDR(out), 0, 0x1DC8);
        strcpy((char*)ADDR(out), "Direct3D 9 (native recompilation)");
        strcpy((char*)ADDR(out + 0x100), "Terminal Reality Inc.");
        MEM16(out + 0x200) = 0x0100;
        MEM16(out + 0x202) = 0x0100;
        MEM16(out + 0x204) = 0xFFFF;
        MEM32(out + 0x208) = 3;
        MEM32(out + 0x20c) = 0x10;
    }
    DONE(0);
}

API(APIDLLinit) {
    R.hwnd = shims_hwnd(ARG(0));
    R.table = shims_alloc(0x8C);                    /* the DLL keeps a copy */
    memcpy((void*)ADDR(R.table), (void*)ADDR(ARG(1)), 0x8C);
    tset(T_ALPHA, 0xFF);
    tset(T_BLEND, 0);
    fprintf(stderr, "[renderer] init (hwnd arg %08X -> %p)\n", ARG(0), (void*)R.hwnd);
    DONE(1);
}

API(APIDLLkill)            { DONE(1); }
API(APIDLLselectCard)      { DONE(1); }
API(APIDLLrestoreVideoMode) { if (R.locked) unlock_frame(); DONE(1); }
API(APIDLLsetMipMapLevel)  { DONE(1); }
API(APIDLLaddParticle)     { DONE(0); }
API(APIDLLflushParticleList) { DONE(0); }
API(APIDLLadd3dLine)       { DONE(0); }
API(APIDLLflushLineList)   { DONE(0); }
API(APIDLLGetDisplayContext)     { if (ARG(0)) MEM32(ARG(0)) = 0; DONE(0); }
API(APIDLLReleaseDisplayContext) { DONE(0); }

API(APIDLLbuildCardList) {
    static uint32_t name = 0;
    if (!name) {
        name = shims_alloc(64);
        strcpy((char*)ADDR(name), "Direct3D 9");
    }
    if (ARG(0)) MEM32(ARG(0)) = 1;
    if (ARG(1)) MEM32(ARG(1)) = name;
    if (ARG(2)) MEM32(ARG(2)) = name;
    if (ARG(3)) MEM32(ARG(3)) = 0;
    if (ARG(4)) MEM32(ARG(4)) = 0;
    DONE(1);
}

API(APIDLLgetVideoMemory) {
    if (!ARG(0) || !ARG(1) || !ARG(2)) DONE(0);
    else {
        MEM32(ARG(0)) = 64u << 20;      /* plenty of local and AGP memory */
        MEM32(ARG(1)) = 64u << 20;
        MEM32(ARG(2)) = 0;
        DONE(1);
    }
}

API(APIDLLsetVideoMode2) {
    uint32_t w = ARG(0), h = ARG(1), bpp = ARG(2);
    R.row_table = ARG(3);
    fprintf(stderr, "[renderer] setVideoMode2 %ux%ux%u\n", w, h, bpp);
    R.saved_rows = shims_alloc(4 * h);
    memcpy((void*)ADDR(R.saved_rows), (void*)ADDR(R.row_table), 4 * h);
    R.frame = 0;
    R.locked = R.in_scene = 0;
    DONE(make_device(w, h));
}

API(APIDLLsetVideoMode) {
    R.row_table = ARG(0);
    R.saved_rows = shims_alloc(4 * 480);
    memcpy((void*)ADDR(R.saved_rows), (void*)ADDR(R.row_table), 4 * 480);
    DONE(make_device(640, 480));
}

API(APIDLLtoggle) {
    if (R.dev && !R.locked) {
        flush();
        IDirect3DDevice9_Present(R.dev, NULL, NULL, NULL, NULL);
    }
    DONE(1);
}

API(APIDLLbeginScene) {
    if (R.in_scene) DONE(0);
    else { R.in_scene = 1; DONE(1); }
}

API(APIDLLendScene) {
    if (!R.in_scene) DONE(0);
    else { reset_state(); R.in_scene = 0; DONE(1); }
}

API(APIDLLlockFrame)       { DONE(lock_frame()); }
API(APIDLLunlockFrame)     { DONE(unlock_frame()); }
/* The hold buffer only exists above 480 lines; at 640x480 it is the frame. */
API(APIDLLlockHoldBuffer)  { DONE(lock_frame()); }
API(APIDLLunlockHoldBuffer) { DONE(unlock_frame()); }

API(APIDLLselectTexture)   { texture_call(0); STDRET(0); }
API(APIDLLupdateTexture)   { texture_call(1); STDRET(0); }

API(APIDLLsetColorTable16) {
    R.pal = ARG(0);
    /* X8R8G8B8: shifts 16/8/0, scale 1, log2 0 (§2 +0x00..+0x20). */
    static const uint32_t shift[3] = { 16, 8, 0 };
    for (int c = 0; c < 3; c++) {
        tset(0x0c * (uint32_t)c + 0, shift[c]);
        tset(0x0c * (uint32_t)c + 4, 1);
        tset(0x0c * (uint32_t)c + 8, 0);
    }
    if (ARG(0) && ARG(1)) {
        const uint8_t* p = (const uint8_t*)ADDR(ARG(0));
        for (uint32_t i = 0; i < 256; i++)
            MEM16(ARG(1) + 2 * i) = (uint16_t)((uint32_t)p[i * 3] << 16 |
                                               (uint32_t)p[i * 3 + 1] << 8 | p[i * 3 + 2]);
    }
    DONE(1);
}

API(APIDLLdrawPolygon) {
    uint32_t verts = ARG(0);
    int n = (int)ARG(1);
    uint32_t vas[66];
    if (n > 66) n = 66;
    for (int i = 0; i < n; i++) vas[i] = verts + 0x30u * (uint32_t)i;
    DONE(draw_poly(vas, n, ARG(2)));
}

API(APIDLLdrawPolygon2) {
    uint32_t vp = ARG(0);
    int n = (int)ARG(1);
    uint32_t vas[256];
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) vas[i] = MEM32(vp + 4u * (uint32_t)i);
    DONE(draw_poly(vas, n, ARG(2)));
}

/* drawPolyList: Face* array; Face { ?, n, ?[4], FaceRef ref[4] @+0x18 },
 * FaceRef { vertIndex, u, v } with u,v 8.24. */
API(APIDLLdrawPolyList) {
    uint32_t verts = ARG(0), faces = ARG(1), flags = effective_flags(ARG(3));
    int nf = (int)ARG(2);
    if (!R.in_scene || !R.dev) DONE(0);
    else {
        apply_state(flags);
        int fL, fO;
        flat_light(flags, &fL, &fO);
        for (int f = 0; f < nf; f++) {
            uint32_t face = MEM32(faces + 4u * (uint32_t)f);
            int n = (int)MEM32(face + 4);
            if (n < 3 || n > 4 || !room(n, (n - 2) * 3)) continue;
            int base = g_nv;
            for (int k = 0; k < n; k++) {
                uint32_t r = face + 0x18 + 12u * (uint32_t)k;
                list_vertex(verts + 0x30u * MEM32(r), (int32_t)MEM32(r + 4),
                            (int32_t)MEM32(r + 8), flags, fL, fO);
            }
            for (int k = 0; k < n - 2; k++) {
                g_ib[g_ni++] = (WORD)base;
                g_ib[g_ni++] = (WORD)(base + k + 1);
                g_ib[g_ni++] = (WORD)(base + k + 2);
            }
        }
        DONE(1);
    }
}

/* drawPolyList2: Face16* array; Face16 { u16 idx[3], u[3], v[3] }, u,v 0.16. */
API(APIDLLdrawPolyList2) {
    uint32_t verts = ARG(0), faces = ARG(1), flags = effective_flags(ARG(3));
    int nf = (int)ARG(2);
    if (!R.in_scene || !R.dev) DONE(0);
    else {
        apply_state(flags);
        int fL, fO;
        flat_light(flags, &fL, &fO);
        for (int f = 0; f < nf; f++) {
            uint32_t face = MEM32(faces + 4u * (uint32_t)f);
            if (!room(3, 3)) continue;
            for (int k = 0; k < 3; k++) {
                uint32_t idx = MEM16(face + 2u * (uint32_t)k);
                int32_t u = (int32_t)((uint32_t)MEM16(face + 6 + 2u * (uint32_t)k) << 8);
                int32_t v = (int32_t)((uint32_t)MEM16(face + 12 + 2u * (uint32_t)k) << 8);
                g_ib[g_ni++] = (WORD)g_nv;
                list_vertex(verts + 0x30u * idx, u, v, flags, fL, fO);
            }
        }
        DONE(1);
    }
}

API(APIDLLsync) { flush(); DONE(1); }

API(APIDLLclear) {
    if (R.dev) {
        flush();
        DWORD c = 0;
        if (R.in_scene && R.pal) {
            const uint8_t* p = (const uint8_t*)ADDR(R.pal);
            c = (DWORD)p[0] << 16 | (DWORD)p[1] << 8 | p[2];
            tset(T_COLORIDX, 0);
        }
        IDirect3DDevice9_Clear(R.dev, 0, NULL, D3DCLEAR_TARGET, c, 1.0f, 0);
    }
    DONE(1);
}

API(APIDLLsetFogColor) {
    R.fog_rgb = (ARG(0) & 0xFF) << 16 | (ARG(1) & 0xFF) << 8 | (ARG(2) & 0xFF);
    if (R.dev) { flush(); RS(D3DRS_FOGCOLOR, R.fog_rgb); }
    /* The DLL always leaves the scene closed (§4 Clears). */
    if (R.in_scene) { reset_state(); R.in_scene = 0; }
    DONE(1);
}

API(APIDLLclearZBuffer) {
    if (R.dev) { flush(); IDirect3DDevice9_Clear(R.dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0); }
    DONE(1);
}

API(APIDLLclearZBox) {
    if (R.dev) {
        flush();
        D3DRECT r = { (LONG)ARG(0), (LONG)ARG(2), (LONG)ARG(1) + 1, (LONG)ARG(3) + 1 };
        IDirect3DDevice9_Clear(R.dev, 1, &r, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    }
    DONE(1);
}

/* Only master 0 and only full-screen are ever used (§4). */
API(APIDLLmasterZBuffer) {
    if (!R.dev || ARG(0) != 0) DONE(0);
    else {
        flush();
        IDirect3DDevice9_StretchRect(R.dev, R.depth, NULL, R.master, NULL, D3DTEXF_NONE);
        DONE(1);
    }
}

API(APIDLLrestoreZBuffer) {
    if (!R.dev || ARG(0) != 0) DONE(0);
    else {
        flush();
        IDirect3DDevice9_StretchRect(R.dev, R.master, NULL, R.depth, NULL, D3DTEXF_NONE);
        DONE(1);
    }
}

/* ============================================================
 * GetProcAddress
 * ============================================================ */

static const struct { const char* name; recomp_func_t fn; } k_api[] = {
#define E(n) { #n, n }
    E(APIDLLInformation), E(APIDLLinit), E(APIDLLkill), E(APIDLLtoggle),
    E(APIDLLsetVideoMode), E(APIDLLsetVideoMode2), E(APIDLLrestoreVideoMode),
    E(APIDLLbeginScene), E(APIDLLendScene), E(APIDLLlockFrame), E(APIDLLunlockFrame),
    E(APIDLLselectTexture), E(APIDLLupdateTexture), E(APIDLLsetMipMapLevel),
    E(APIDLLdrawPolygon), E(APIDLLdrawPolygon2), E(APIDLLdrawPolyList),
    E(APIDLLdrawPolyList2), E(APIDLLaddParticle), E(APIDLLflushParticleList),
    E(APIDLLadd3dLine), E(APIDLLflushLineList), E(APIDLLclear), E(APIDLLsetFogColor),
    E(APIDLLsync), E(APIDLLclearZBuffer), E(APIDLLclearZBox), E(APIDLLsetColorTable16),
    E(APIDLLGetDisplayContext), E(APIDLLReleaseDisplayContext), E(APIDLLmasterZBuffer),
    E(APIDLLrestoreZBuffer), E(APIDLLgetVideoMemory), E(APIDLLselectCard),
    E(APIDLLbuildCardList), E(APIDLLlockHoldBuffer), E(APIDLLunlockHoldBuffer),
#undef E
};

uint32_t video_renderer_proc(const char* name) {
    static uint32_t thunk[sizeof(k_api) / sizeof(k_api[0])];
    for (unsigned i = 0; name && i < sizeof(k_api) / sizeof(k_api[0]); i++)
        if (!strcmp(name, k_api[i].name)) {
            if (!thunk[i]) thunk[i] = recomp_host_thunk(k_api[i].fn);
            return thunk[i];
        }
    fprintf(stderr, "[renderer] GetProcAddress(%s) -> not provided\n", name ? name : "?");
    return 0;
}
