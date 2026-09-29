# Nocturne renderer DLL API (`APIDLL*`), reverse-engineered

Source of truth: `analysis/tridx7.dll` (32-bit MSVC, image base 0x10000000) and
the calling side in `analysis/nocturne.exe` (Watcom, base 0x400000). All
addresses below are VAs in those images. `tridx6.dll` is **byte-identical** to
`tridx7.dll`. `tri3dfx.dll` is the same source compiled with small changes
(it also runs on DirectDraw, not Glide). `trid3d.dll` is an older variant.

**Despite the name, tridx7 is a DirectX 6 renderer.** It uses
`IDirectDraw4` (IID 9c59509a-39bd-11d1-8c4a-00c04fd930c5 at 0x10012098),
`IDirectDrawSurface4`, `IDirect3D3` (bb223240-... at 0x10012148),
`IDirect3DDevice3` created on `IID_IDirect3DHALDevice` (84e63de0-... at
0x10012178), `IDirect3DViewport3`, and `IDirect3DTexture2` (93281502-... at
0x100121f8). Its fatal-error caption is "DirectX6 3D Driver error", and its
string resource 1 is "DirectX6 (Some hardware)". It imports only
`DirectDrawCreate` and `DirectDrawEnumerateExA` from DDRAW, plus USER32
`MessageBoxA`, `ShowCursor`, `SetCursorPos`, and `LoadStringA`.

Items marked **UNVERIFIED** are inferences that were not proven from the
disassembly.

---

## 0. Conventions that apply to every export

* **Calling convention: cdecl.** I checked every one of the 37 exports: each
  one returns with a bare `ret`, and the exe's wrappers clean the stack
  themselves (`add esp, N`). The only `ret N` in the DLL is on internal
  DirectDraw/D3D enumeration callbacks, which are stdcall.
* `int` is 32-bit. On x64 the host has to marshal pointers, because the exe
  hands the DLL 32-bit pointers into its own image.
* Return values follow the same pattern throughout: `1` means success. `0`
  means failure, or "not in a scene / nothing locked". For the stubs, `0`
  means "not implemented".
* Most failures inside the DLL are **fatal**. They go to `fatal(msg)` at
  0x10002340, which calls `APIDLLkill()`, then
  `MessageBoxA(hwnd, msg, "DirectX6 3D Driver error", MB_ICONHAND)`, then
  `ExitProcess(1)`. The Z-buffer creation failures use caption
  "3D Adapter Error" and `ExitProcess(666)`.
* **There are no callbacks from the DLL into exe code.** The only coupling
  besides arguments is the **pointer table** passed to `APIDLLinit` (§2).
  Through it the DLL reads, and sometimes writes, exe globals.
* The DLL reads `.\system\render.ini`: `[Textures]` maxTextures32/64/128/256
  and mipMapFlag; `[Graphics]` masterZBufferCount, useHoldBuffer,
  premultiplyColorAndAlpha, directTextureFlag, allowAutoMipMapping. If
  `[Textures]` is missing it writes the file. The shipped file has
  maxTextures32=0, 64=192, 128=4, 256=32, mipMapFlag=0, masterZBufferCount=1,
  useHoldBuffer=1. The DLL also tests whether `system\fly.ini` exists (§6.3).

## 1. How the exe loads the DLL

* `nocturne.ini: rendererDLLPath=tridx7.dll`. The loader is 0x531780. It
  calls LoadLibrary through wrapper 0x553d30, stores the HMODULE in
  `0x2dc9e08`, then calls GetProcAddress through wrapper 0x553d40. A second,
  apparently unused copy of the loader sits at 0x530e60 and has no callers.
* It calls `APIDLLInformation(hModule, &dllInfo)` first, then builds a
  "required" struct with 0x532da0 and runs the version check at 0x532df0 (§3).
  If the check fails, the DLL is rejected.
* It resolves the function-pointer slots below. A missing export produces an
  assert, except where the table says "(optional)". Those slots are
  null-checked at each use.

| slot | export | slot | export |
|---|---|---|---|
| 0x2dc9d74 | init | 0x2dc9dbc | flushParticleList |
| 0x2dc9d78 | kill | 0x2dc9dc0 | add3dLine |
| 0x2dc9d7c | toggle | 0x2dc9dc4 | flushLineList |
| 0x2dc9d80 | setVideoMode | 0x2dc9dc8 | clear |
| 0x2dc9d84 | setVideoMode2 | 0x2dc9dcc | setFogColor |
| 0x2dc9d88 | restoreVideoMode | 0x2dc9dd0 | sync |
| 0x2dc9d8c | beginScene | 0x2dc9dd4 | clearZBuffer |
| 0x2dc9d90 | endScene | 0x2dc9dd8 | clearZBox |
| 0x2dc9d94 | lockFrame | 0x2dc9ddc | setColorTable16 |
| 0x2dc9d98 | unlockFrame | 0x2dc9de0 | GetDisplayContext |
| 0x2dc9d9c | selectTexture | 0x2dc9de4 | ReleaseDisplayContext |
| 0x2dc9da0 | updateTexture | 0x2dc9de8 | masterZBuffer |
| 0x2dc9da4 | setMipMapLevel | 0x2dc9dec | restoreZBuffer |
| 0x2dc9da8 | drawPolygon | 0x2dc9df0 | getVideoMemory |
| 0x2dc9dac | drawPolygon2 | 0x2dc9df4 | selectCard |
| 0x2dc9db0 | drawPolyList (optional) | 0x2dc9df8 | buildCardList |
| 0x2dc9db4 | drawPolyList2 (optional) | 0x2dc9dfc | lockHoldBuffer (optional) |
| 0x2dc9db8 | addParticle | 0x2dc9e00 | unlockHoldBuffer (optional) |

After loading, `0x2dc9e04` is set to 1 to mean "DLL loaded". Separately,
`0x1c02594` is set to 1 to mean "3D video mode active". Almost every exe
wrapper returns 0 when `0x1c02594` is 0 and does not call the DLL.

The exe calls each export through a one-line wrapper (0x5322b0 to 0x532d80).
Wrappers with **no callers** are marked "never called" in §4.

## 2. `APIDLLinit` pointer table (35 dwords, 0x8C bytes)

The exe builds this table on the stack at 0x5320ac–0x532249 and passes it as
arg 2 of `APIDLLinit`. The DLL copies it to `0x102268b8`. Each entry is a
**pointer to an exe global int**, except entries +0x70 to +0x88, which are
plain values. The DLL dereferences the pointers on every use, so the values
are always live. **W** means the DLL writes the exe global.

| off | exe global | meaning (as used by tridx7) |
|---|---|---|
| +0x00 | 0x1c00624 | **W** red shift of the back buffer pixel format |
| +0x04 | 0x1c00628 | **W** red scale = 255 / (redMask >> shift) (5 bits gives 8) |
| +0x08 | 0x1c0062c | **W** log2(red scale) (bits dropped from 8-bit red) |
| +0x0c | 0x1c00630 | **W** green shift |
| +0x10 | 0x1c00634 | **W** green scale |
| +0x14 | 0x1c00638 | **W** log2(green scale) |
| +0x18 | 0x1c0063c | **W** blue shift |
| +0x1c | 0x1c00640 | **W** blue scale |
| +0x20 | 0x1c00644 | **W** log2(blue scale) |
| +0x24 | 0x1c03998 | **W** blend mode: 0 = normal (DESTBLEND=INVSRCALPHA), 1 = additive (DESTBLEND=ONE) |
| +0x28 | 0x1c00c74 | flat light level, same encoding as vertex +0x20; read when a polygon has flag 0x10 |
| +0x2c | 0x5b763c | **W** global vertex alpha 0..255 for textured polygons without flag 0x100 |
| +0x30 | 0x1c00c70 | **W** flat colour index 0..255 for untextured polygons without flag 4 |
| +0x34..+0x40 | 0x1c00c58/5c/60/64 | x0, y0, x1, y1 of a clear rectangle; read only by an internal, unexported fill (0x10004910) |
| +0x44 | 0x5b7628 | not used by tridx7 (trid3d uses it as a texture downsample flag, UNVERIFIED) |
| +0x48 | 0x5b762c | **current texture size** (32, 64, 128 or 256; textures are square) |
| +0x4c | 0x1c02590 | not used by tridx7 |
| +0x50 | 0x5c0f8c | **far Z** (int, in the same units as vertex z) |
| +0x54 | 0x5c0f84 | dither enable, fed to D3DRENDERSTATE_DITHERENABLE |
| +0x58 | 0x5c0f88 | not used by tridx7 |
| +0x5c | 0x2dc9d68 | flip waits for the flip (DDFLIP_WAIT) when nonzero |
| +0x60 | 0x5c0f80 | bilinear filtering: MAG/MIN filter LINEAR when nonzero, else POINT |
| +0x64 | 0x2dc9d6c | **W** texture detail / VRAM level. 0: textures created at half size. >1: two back buffers. Forced to 0 when there is no non-local (AGP) VRAM |
| +0x68 | 0x1c0399c | Z mapping: nonzero means linear z/far, 0 means 1 − 1/z (§6.3) |
| +0x6c | 0x5b7640 | mip filter: LINEAR (3) when nonzero, else NONE (1) |
| +0x70..+0x88 | values 0x30, 0x2c, 0x0c, 0x20, 0x28, 0x24, 0x2c | vertex size and field offsets (§6.1); tridx7 ignores them and hard-codes the layout |

The DLL writes exe globals as side effects in these places:

* `setColorTable16` writes +0x00 to +0x20.
* Every `endScene`, and `init`, sets `*[+0x2c] = 0xFF` (alpha) and
  `*[+0x24] = 0` (normal blend) at 0x10002d50.
* `clear` sets `*[+0x30] = 0`.
* `setVideoMode` may set `*[+0x64] = 0`.

## 3. `APIDLLInformation` struct (0x1DC8 bytes) and the exe's version check

```c
void APIDLLInformation(HMODULE hDll, RendererInfo *out);  /* 0x100019a0, no meaningful return */
typedef struct {                 /* 0x1DC8 bytes; tridx7 zeroes all of it first */
  char     name[256];            /* +0x000 LoadStringA(hDll, 1) = "DirectX6 (Some hardware)" */
  char     vendor[256];          /* +0x100 "Terminal Reality Inc." */
  uint16_t version;              /* +0x200 0x0100 (major version is the high byte = 1) */
  uint16_t version2;             /* +0x202 0x0100 */
  uint16_t interfaceId;          /* +0x204 0xFFFF (wildcard) */
  uint16_t pad;                  /* +0x206 0 */
  uint32_t caps;                 /* +0x208 3 */
  uint32_t magic;                /* +0x20c 0x10 */
  uint32_t reserved[16];         /* +0x210 must be 0 */
  uint8_t  rest[0x1DC8-0x250];   /* 0 */
} RendererInfo;
```

The exe fills its required struct at 0x532da0: it memsets it to 0, sets
`version = 0x0100`, `interfaceId = 1` and `caps = 1`. The check at 0x532df0
receives `(dll, req)` and accepts the DLL only if **all** of these hold:

1. `dll.interfaceId == req.interfaceId` (1), **or** `dll.interfaceId == 0xFFFF`.
2. `(~dll.caps & req.caps) == 0`, so dll.caps must have bit 0 set.
3. `req.version != 0`, so the major bytes (byte +0x201) must match: 1 == 1.
4. `req.version2 == 0`, so that comparison is skipped. `req.vendor` is empty,
   so the strcmp of `vendor` is skipped.
5. `dll.magic == 0x10`.
6. `dll.reserved[0..15] == req.reserved[0..15]`, which are all zero.

A replacement only has to copy tridx7's values exactly.

## 4. The 37 functions

The **Exe use** column shows whether the exe calls the export in normal flow,
based on callers of the exe's wrappers. Addresses are the call sites in the exe.

### Lifecycle

| export (DLL VA) | signature | exe use |
|---|---|---|
| `APIDLLInformation` 0x100019a0 | `void (HMODULE hDll, RendererInfo *out)` | called once by the loader |
| `APIDLLinit` 0x10001a80 | `int (HWND hwnd, int **ptrTable)` | called by the loader at 0x532250 |
| `APIDLLkill` 0x10002460 | `int (void)`, returns 1 | yes (wrapper 0x5322b0) |
| `APIDLLselectCard` 0x100052c0 | `int (int cardIndex)` | yes (0x532d00 ← card menu, and after init) |
| `APIDLLbuildCardList` 0x100052e0 | `int (int *count, char **driverNames, char **descriptions, uint32 *vendorIds, uint32 *deviceIds)`, returns 1 | yes |
| `APIDLLgetVideoMemory` 0x10005280 | `int (uint32 *localTotal, uint32 *nonLocalTotal, uint32 *third)` | yes |

**init** runs these steps in order:

1. Calls `kill()`.
2. Calls `DirectDrawEnumerateExA(cb 0x100017b0, 0, 7)`. It keeps up to 16
   devices whose `GetCaps` reports `DDCAPS_3D`.
3. For each device it creates DirectDraw, runs `QI(IDirectDraw4)` and
   `GetDeviceIdentifier`, and caches szDriver (0x101398d0 + i*0x200),
   szDescription (0x10236910 + i*0x200), dwVendorId and dwDeviceId.
4. Stores hwnd (0x10138fb8) and copies the pointer table.
5. Creates DirectDraw on card `0x10014210` (default 0) and QIs
   `IDirectDraw4`.
6. Calls `GetAvailableVidMem`: total for `DDSCAPS_LOCALVIDMEM` goes to
   0x10014208 and total for `DDSCAPS_NONLOCALVIDMEM` goes to 0x1001420c.
7. QIs `IDirect3D3`.

It returns 1 on success and 0 on failure. **No display mode is set here.**

**kill** releases the hold surface, viewport, device, D3D, Z buffer, master Z
buffers, primary, and both DirectDraw objects, then returns 1.

**selectCard(n)** sets the card index and returns
`APIDLLinit(savedHwnd, internalTableCopy)`.

**buildCardList** writes the device count to `*count`. For each device i it
sets `driverNames[i]` and `descriptions[i]` to point at the DLL's string
buffers, and copies the vendor and device IDs. It depends on data from `init`.

**getVideoMemory** stores the local and non-local totals, sets `*third = 0`,
and returns 1. It returns 0 if any pointer is NULL. The exact meaning of the
third output is UNVERIFIED.

### Video mode

| export | signature | exe use |
|---|---|---|
| `APIDLLsetVideoMode2` 0x10002bb0 | `int (int width, int height, int bpp, uint8_t **rowTable)` | yes. Exe wrapper 0x5324a0 passes `(0x5b761c, 0x5b7620, max(bpp,16), 0x1bd2fa0)` |
| `APIDLLsetVideoMode` 0x10002500 | `int (uint8_t **rowTable)`; uses the width, height and bpp already stored (default 640×480×16) | **never called directly** (only through setVideoMode2) |
| `APIDLLrestoreVideoMode` 0x10002be0 | `int (void)`, returns 1 | yes |
| `APIDLLtoggle` 0x100024b0 | `int (void)`, returns 1 | yes, as the per-frame flip in 0x532ba0 |

**setVideoMode** does the following:

1. If there is no non-local VRAM it forces `*[+0x64] = 0`.
2. Picks the mip size table. When `*[+0x64]` is nonzero it is 256, 128 … 2;
   otherwise 128, 64 … 1.
3. Reads the ini keys.
4. Saves the exe row table (height dwords) to 0x10225848.
5. Calls `SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE|DDSCL_FULLSCREEN)` and
   `SetDisplayMode(w, h, bpp, 0, 0)`.
6. Creates a primary surface with `DDSCAPS_PRIMARYSURFACE|FLIP|COMPLEX|3DDEVICE|VIDEOMEMORY`
   and 1 back buffer, or 2 when `*[+0x64] > 1`. If that fails it retries
   with 1.
7. Gets the back buffer (`DDSCAPS_BACKBUFFER|3DDEVICE`).
8. Enumerates Z formats. At 16 bpp it takes the first one; at 32 bpp it takes
   the deepest. It then creates the Z buffer and `masterZBufferCount` more Z
   buffers of the same format, and attaches the Z buffer to the back buffer.
9. Colour-fills the primary and back buffer with 0.
10. Creates the device at 0x10003100: `CreateDevice(HAL, backbuffer)`,
    `EnumTextureFormats` (§5), a viewport covering the full screen with
    Z from 0 to 1, and the **hold surface** when `useHoldBuffer` is set.
11. Creates the staging textures and the texture cache at 0x10003400, then
    sets the initial render states (§7).
12. Calls `SetCursorPos(639, 479)`, hides the cursor, then runs
    clear + toggle three times.

It returns 1, or 0 (sometimes a fatal error) on failure. `useHoldBuffer` is
only read when height > 480.

**restoreVideoMode**:

1. Unlocks the frame if it is locked.
2. Calls `FlipToGDISurface`, `RestoreDisplayMode` and
   `SetCooperativeLevel(hwnd, DDSCL_NORMAL)`.
3. Releases all surfaces and shows the cursor again.

**toggle** calls `Flip(NULL, *[+0x5c] ? DDFLIP_WAIT : 0)`. It skips the flip
while the frame is locked.

### Scene and frame access

| export | signature | exe use |
|---|---|---|
| `APIDLLbeginScene` 0x10002ce0 | `int (void)` | yes (0x532340) |
| `APIDLLendScene` 0x10002d10 | `int (void)` | yes (0x532360) |
| `APIDLLlockFrame` 0x10002d90 | `int (void)` | yes (0x5322e0) |
| `APIDLLunlockFrame` 0x10002e60 | `int (void)` | yes (0x532320) |
| `APIDLLlockHoldBuffer` 0x10005350 | `int (void)` | yes, when height > 480 (0x445061) |
| `APIDLLunlockHoldBuffer` 0x100053a0 | `int (void)` | yes, when height > 480 |
| `APIDLLGetDisplayContext` 0x10004d30 | `void (HDC *out)` | **never called** |
| `APIDLLReleaseDisplayContext` 0x10004da0 | `void (HDC)` | **never called** |

* **beginScene** returns 0 if a scene is already open. Otherwise it calls
  `IDirect3DDevice3::BeginScene`, sets inScene (0x10014204), and returns 1.
* **endScene** returns 0 if no scene is open. Otherwise it:
  1. resets the state (0x10002d50, §7);
  2. flushes the batch;
  3. clears inScene;
  4. returns `EndScene() == D3D_OK`.
* **lockFrame** does the following:
  1. Calls `endScene()` if a scene is open.
  2. Locks the **back buffer** (`Lock(NULL, &desc, DDLOCK_WAIT)`).
  3. **Writes `rowTable[y] = lpSurface + y*lPitch` for y < height into the
     exe's row table** (the pointer given to setVideoMode). This is the only
     output: pixels are written through the exe's own row table, not
     returned.
  4. Sets frameLocked and returns 1.

  The pixel format is the back buffer's: 16 bpp (565 or 555) or 32 bpp. The
  exe learns the channel shifts and scales from `setColorTable16` (§2 +0x00
  to +0x20).

  After lockFrame, the exe writes 0xABCD (or 0xAABBCCDD at 32 bpp) to row 0,
  reads it back, and records a mismatch in `0x2dc9d70`. **The mapped memory
  must therefore be readable as well as writable.**

  The exe wrapper also swaps its software-rasterizer bpp `0x5b7624` to the
  hardware bpp while the frame is locked.
* **unlockFrame** returns 0 if the frame is not locked. Otherwise it
  restores the exe's original row-table contents (saved by setVideoMode),
  unlocks the back buffer (DDERR_NOTLOCKED counts as success), and returns 1.
  It does **not** reopen the scene; the exe calls beginScene afterwards
  (0x4450a2).
* **lockHoldBuffer** and **unlockHoldBuffer** work like lockFrame and
  unlockFrame, but on a **640×480 off-screen "hold" surface**.
  unlockHoldBuffer then calls `Blt(backbuffer, NULL, hold, NULL, 0, NULL)`,
  which stretches the full hold surface onto the back buffer with no colour
  key. That is how the 640×480 2D HUD is shown at higher resolutions (exe
  0x445020).
  * The hold surface is created with the **texture** pixel format (§5), not
    the back buffer format, while the exe draws in the back buffer format.
    This looks like a latent bug, or DirectDraw converting on Blt. A
    reimplementation should give the hold buffer the back buffer format.
    UNVERIFIED.
  * The unconditional full-screen Blt should overwrite the 3D scene; how the
    3D survives is UNVERIFIED.
* **GetDisplayContext** temporarily unlocks a locked back buffer, calls
  `GetDC`, and stores the HDC (or 0) in `*out`. **ReleaseDisplayContext**
  calls `ReleaseDC` and relocks if it had unlocked. Neither is used.

The exe's flip wrapper 0x532ba0 is relevant here. If the frame was *not*
locked this frame, it locks the frame, copies the exe's own software
framebuffer into the locked rows (row 0 of the software buffer is the saved
`*(0x1bd2fa0)`, stride `width*bpp/8`), and unlocks. Then it calls `toggle`.
That is how pure-2D screens reach the display.

### Textures

| export | signature | exe use |
|---|---|---|
| `APIDLLselectTexture` 0x10003e90 | `int (TexKey *key, int sizeUnused, const uint8_t *pixels, const uint8_t *paletteRGB, const uint8_t *alpha)`, returns 1 | yes (0x532400 ← CTextureCache::load 0x5459a4) |
| `APIDLLupdateTexture` 0x10003ed0 | same signature, returns 1 | yes (0x532440) |
| `APIDLLsetMipMapLevel` 0x10003f00 | `int (int)`, stub returning 1 | **never called** |
| `APIDLLsetColorTable16` 0x10004b30 | `int (const uint8_t paletteRGB[256][3], uint16_t out16[256])`, returns 1 | yes (0x5324a0, 0x5325c0, 0x5538da) |

The exe calls `select/updateTexture(texRecord, *(int*)0x5b762c, *(void**)0x1c02580, palette, *(void**)0x1c02584)`.
The palette is `*(void**)0x1c00020`, the current palette, or `0x1c00648`
in the unused wrapper variants.

* **`TexKey`** is the exe's texture record. The DLL only reads
  `char name[] at +0x08`, which serves as the cache key (strcmp; copied into
  a 64-byte field, max 63 characters). The exe keeps its own handle in the
  low 16 bits of `+0x04`.
* **Arg 2 is ignored.** The DLL takes the size from `*ptrTable[+0x48]`
  (32, 64, 128 or 256; any other value is fatal: "Unknown texture size").
  Textures are square, `size × size`.
* **`pixels`** holds `size*size` 8-bit palette indices, row-major with no
  padding.
* **`paletteRGB`** holds 256 × 3 bytes (R, G, B, each 0..255). This is the
  exe's `.ACT` palette, taken as is.
* **`alpha`** is either NULL or `size*size` bytes of per-texel alpha
  (0..255).

The DLL converts a texture to 32-bit ARGB (0x10003830) like this:

```
rgb = pal[i*3+0]<<16 | pal[i*3+1]<<8 | pal[i*3+2]
if (alpha)  argb = alpha[t]<<24 | rgb
else        argb = rgb ? (0xFF000000|rgb) : 0      // palette entries that are exactly black are TRANSPARENT
```

* It then builds a mip chain by 2×2 box filtering (`(a&0xfefefeff)>>1`
  averages). This happens when `*[+0x64]==0`, in which case the texture is
  created at `size/2` and the half-size level is uploaded, or when
  mipMapFlag is set.
* It uploads to the device texture format:
  * ARGB8888 is copied as is.
  * For the 16-bit format it keeps the top 4 bits of each channel, placed at
    the format's R/G/B shifts with alpha in bits 12..15, which is ARGB4444.
* The upload goes via one of four round-robin system-memory staging
  surfaces and a `Blt` to the VRAM texture. With `directTextureFlag` it
  locks the VRAM texture directly instead.

Cache semantics (0x10001020 to 0x100015d0):

* There is one LRU cache per size class. Its capacity comes from
  `maxTexturesNN`, and all of its surfaces are created up front at
  setVideoMode (at most 4096 in total).
* **selectTexture** looks the name up in the class for the current size.
  * On a **hit** it only makes the texture current.
  * On a **miss** it evicts the LRU entry, uploads, and makes the new entry
    current.
* **updateTexture** always re-uploads: in place on a hit, into a new slot on
  a miss. The uploaded texture becomes current. It is used for animated or
  changed texture contents.
* The pixel, palette and alpha pointers are also remembered as "current".
  **The alpha pointer affects later draws** (§6.2, the flags |= 0x22 rule).

**setColorTable16** does the following:

1. Stores the palette pointer at 0x10215e40. That pointer is then **used by
   the DLL for untextured polygon colours**.
2. Locks and unlocks the back buffer just to read its pixel format.
3. Writes the R, G and B shift, scale and log2(scale) values into the exe
   (§2 +0x00 to +0x20).
4. Fills `out16[i] = (R/rScale)<<rShift | (G/gScale)<<gShift | (B/bScale)<<bShift`,
   a 256-entry native-format lookup that the exe's 2D code uses.

The exe passes `(0x1c00648, 0x1bff720)`.

### Drawing

| export | signature | exe use |
|---|---|---|
| `APIDLLdrawPolygon` 0x10004380 | `int (NocVertex *verts /*stride 0x30*/, int n, uint32 flags)` | yes (0x532620 ← pod.cpp, texture.cpp, zombie.cpp, clear-rect) |
| `APIDLLdrawPolygon2` 0x100043c0 | `int (NocVertex **vptrs, int n, uint32 flags)` | yes, the main path (clipper.c 0x432xxx–0x434xxx) |
| `APIDLLdrawPolyList` 0x10004f00 | `int (NocVertex *verts, Face **faces, int nFaces, uint32 flags)` | yes (3d.c 0x408fa8, drender.cpp 0x461702) |
| `APIDLLdrawPolyList2` 0x10005130 | `int (NocVertex *verts, Face16 **faces, int nFaces, uint32 flags)` | yes (drender.cpp 0x4619dd) |
| `APIDLLaddParticle` 0x10004800 | `int (?, ?)`, 2 args from the wrapper; **stub, returns 0** | **never called** |
| `APIDLLflushParticleList` 0x10004810 | `int (void)`; **stub, returns 0** | **never called** |
| `APIDLLadd3dLine` 0x10004820 | `int (?, ?, ?)`, 3 args from the wrapper; **stub, returns 0** | **never called** |
| `APIDLLflushLineList` 0x10004830 | `int (void)`; **stub, returns 0** | **never called** |
| `APIDLLsync` 0x10004a60 | `int (void)`: flushes the batch, returns 1 | yes |

The particle and line exports are also stubs in tri3dfx and trid3d. Their
arguments are UNVERIFIED and irrelevant.

* **drawPolygon2** returns **0 and draws nothing when no scene is open**.
  Otherwise it:
  1. applies the state for `flags` (§7);
  2. computes `maxZ = max(v->z)` over the polygon;
  3. converts each vertex (§6.2) with `rhw = maxZ / z`;
  4. appends the vertices and a **triangle fan** `(0, i+1, i+2)` to the batch;
  5. returns 1.

  The batch holds up to 15990 (0x3e76) vertices and indices. It is flushed
  as `DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX (0x1c4), …,
  D3DDP_DONOTCLIP|DONOTUPDATEEXTENTS|DONOTLIGHT)` when it overflows, on any
  state change, on sync, and on endScene.
* **drawPolygon** builds a pointer array (at most 66 vertices) and calls
  drawPolygon2.
* **drawPolyList** applies the state once. Then, for each face, it emits
  triangles `(r0, r[k+1], r[k+2])` for k < n−2.

  ```c
  typedef struct { int32 vertIndex; int32 u; int32 v; } FaceRef;   /* u,v 8.24 */
  typedef struct { int32 ?; int32 n; int32 ?[4]; FaceRef ref[4]; } Face;  /* ref at +0x18; n = 3 or 4 */
  ```

  Each referenced vertex is converted **once per call** and cached by a
  per-call generation counter, with `rhw = 256 / z`, **not maxZ/z**. Before
  converting, the DLL **writes the FaceRef u,v into the exe vertex (+0x18,
  +0x1c)**. When a later face uses the same vertex with different u,v, the
  converted vertex is duplicated with the new u,v. If the exe's slot is null,
  the exe emulates the call with drawPolygon2 (0x532680); that path confirms
  the layout.
* **drawPolyList2** works the same way, but every face is a triangle:

  ```c
  typedef struct { uint16 idx[3]; uint16 u[3]; uint16 v[3]; } Face16;  /* 18 bytes; u,v in 0.16 (u/65536) */
  ```

  It expands them to 8.24 (`<<8`).

### Clears, fog and Z buffer

| export | signature | exe use |
|---|---|---|
| `APIDLLclear` 0x10004840 | `int (void)`, returns 1 | yes (CDemonCamera::beginScene 0x4404ef) |
| `APIDLLsetFogColor` 0x100049f0 | `int (int r, int g, int b)` (0..255), returns 1 | yes |
| `APIDLLclearZBuffer` 0x10004a70 | `int (void)`, returns 1 | yes (0x4404f4, straight after clear) |
| `APIDLLclearZBox` 0x10004ac0 | `int (int x0, int x1, int y0, int y1)`, returns 1 | yes (0x52efdc) |
| `APIDLLmasterZBuffer` 0x10004e10 | `int (int n)` | yes (endScene 0x440b05, beginBackgroundScene 0x440d2f, n = 0) |
| `APIDLLrestoreZBuffer` 0x10004e90 | `int (int n, int left, int top, int right, int bottom)` | yes (beginScene 0x4406fc, with `(0, 0, 0, w-1, h-1)`) |

* **clear**, inside a scene, sets `*[+0x30] = 0` and draws a full-screen quad
  through drawPolygon with flags 0x10 (untextured, flat, no Z, no blend,
  colour `palette[0]`, z = far). Outside a scene it colour-fills the back
  buffer with 0.
* **setFogColor** stores the colour and calls
  `SetRenderState(FOGCOLOR, r<<16|g<<8|b)`. If no scene was open it calls
  beginScene first. **It calls endScene whenever a scene is open at the end,
  so it always leaves the scene closed.** Keep that behaviour.
* **clearZBuffer** runs a `Blt DDBLT_DEPTHFILL` with depth 0xFFFFFFFF, which
  is the far plane.
* **clearZBox** depth-fills the rect `{x0, y0, x1+1, y1+1}` (inclusive
  bounds). Note the argument order: x0, x1, y0, y1.
* **masterZBuffer(n)** copies the whole Z buffer into master Z buffer n
  (`0 <= n < masterZBufferCount`, otherwise it returns 0).
  **restoreZBuffer(n, l, t, r, b)** copies master n back into the Z buffer
  for the rect `{l, t, r+1, b+1}`. It checks the pointer but not the range.
  Failure is fatal. The exe uses these calls to snapshot the depth of the
  pre-rendered background and restore it every frame. The only rect used is
  full-screen.

## 5. Surfaces and formats created

* **Back buffer**: display format at `bpp` (16 or 32). It is the D3D render
  target.
* **Z buffer**: at 16 bpp, the first enumerated Z format; at 32 bpp, the
  deepest. It is always 0xFFFFFFFF-cleared, and ZFUNC LESSEQUAL means a
  smaller z is nearer. The depth is stored at 0x10014170 and matters only
  for the fly.ini path.
* **Texture format**, chosen by the `EnumTextureFormats` callback at
  0x10003340:
  * At 32 bpp it takes the first 32-bit format with alpha mask 0xFF000000
    (ARGB8888).
  * At 16 bpp it prefers a 16-bit format whose alpha mask has bits in
    0xF000 (**ARGB4444**) and stops as soon as the mask is exactly 0xF000.
  * Otherwise it falls back to the first 16-bit format.

  For D3D9, use `D3DFMT_A8R8G8B8` and skip the 4444 quantisation, or use
  A4R4G4B4 for fidelity.
* **Textures**: `size×size` (half that when `*[+0x64]==0`),
  `DDSCAPS_TEXTURE|VIDEOMEMORY`. `DDSD_TEXTURESTAGE` gives them 1 mip level
  unless mipMapFlag or allowAutoMipMapping is set. Addressing is **CLAMP**.

## 6. Vertex formats

### 6.1 Input vertex `NocVertex` (0x30 bytes, all int32)

```c
typedef struct {
  int32 x0;      /* +0x00  not read by tridx7 */
  int32 x4;      /* +0x04  not read by tridx7 */
  int32 z;       /* +0x08  depth, integer >0, same units as *farZ (+0x50) */
  int32 xc;      /* +0x0c  not read by tridx7 */
  int32 sx;      /* +0x10  screen X, 16.16 fixed (sx/65536.0f) */
  int32 sy;      /* +0x14  screen Y, 16.16 fixed */
  int32 u;       /* +0x18  8.24 fixed, 1.0 = 0x01000000 (u * 2^-24).
                            Untextured with flag 4: colour index = (u>>16)&0xFF */
  int32 v;       /* +0x1c  8.24 fixed */
  int32 light;   /* +0x20  mono light: level = (light-0x100)>>4; or RED 8.8 with flag 0x200 */
  int32 g;       /* +0x24  GREEN 8.8 (flag 0x200 only) */
  int32 b;       /* +0x28  BLUE 8.8 (flag 0x200 only) */
  int32 af;      /* +0x2c  8.8: alpha (flag 0x100, textured) AND/OR fog amount (flag 8) */
} NocVertex;
```

### 6.2 Conversion to `D3DTLVERTEX` (0x100044b0)

`{sx, sy, sz, rhw, color, specular, tu, tv}`, FVF 0x1c4. The same FVF exists
in D3D9: `XYZRHW|DIFFUSE|SPECULAR|TEX1`. Pixel centres are the same in DX6
and D3D9, so no half-pixel offset is needed.

* `sx = v.sx/65536`, `sy = v.sy/65536`. If the hold buffer is enabled,
  `sx *= width/640` and `sy *= height/480`, because the exe works in
  640×480 space.
* `rhw = maxZ / v.z` for drawPolygon/drawPolygon2 (maxZ is the polygon
  maximum) and `256 / v.z` for poly lists. Only ratios matter to
  perspective-correct texturing.
* `sz`: see §6.3.
* `tu = v.u * 2^-24`, `tv = v.v * 2^-24`.
* Fog factor `F`: `255 − (v.af >> 8)` with flag 8, otherwise 255.
* Light (flags 4 and 0x200):
  * With flag 0x200: `L = 255`, `O = 0`.
  * With flag 4 and not 0x200: `L = (v.light − 0x100) >> 4`. If L > 255,
    then `O = min(L − 256, 255)` and `L = 255`; otherwise `O = 0`. O is the
    overbright amount added as white specular.
  * Without flag 4: L and O are the per-polygon cached values. With flag
    0x10 they come from `*[+0x28]` using the same formula; without it
    `L = 255`, `O = 0`.
* **Textured polygons (flag 1):**
  * `A = (flags & 0x100) ? v.af >> 8 : *[+0x2c]`
  * `specular = F<<24 | O<<16 | O<<8 | O`
  * `color` depends on the flags:
    * With 0x200: `A<<24 | (v.light>>8)<<16 | (v.g>>8)<<8 | (v.b>>8)`.
    * Else, if premultiplyColorAndAlpha is on and blend mode `*[+0x24]==1`:
      `M = A*L>>8`, and colour = `0xFF<<24 | M<<16 | M<<8 | M`.
    * Otherwise: `A<<24 | L<<16 | L<<8 | L`.
* **Untextured polygons:**
  * `specular = F<<24`
  * `color` depends on the flags:
    * With 0x200: `0xFF<<24 | RGB`, from the 8.8 fields as above.
    * Otherwise: `0xFF<<24 | pal[idx]`, where `pal` is the setColorTable16
      palette (RGB bytes) and `idx = (flags&4) ? (v.u>>16)&0xFF : *[+0x30]&0xFF`.
      No lighting is applied.

### 6.3 Depth `sz` (0x100046d4)

`far = *[+0x50]`. The reciprocal is recomputed whenever far changes.

* **When `*[+0x68] != 0`:** `sz = min(z/far, 1.0)`.
  * If `system\fly.ini` exists and the Z buffer is 16-bit, `sz` uses
    `sqrt(z)/sqrt(far)` instead, computed with a float-bit-hack
    approximation. Treat this as a dev-only path.
* **When `*[+0x68] == 0`:** `t = clamp(z*256/far, 1, 256)` and
  `sz = 1 − 1/t`.

## 7. Render state

### Initial state (0x100035b0), set once after device creation

The legacy D3DRENDERSTATE_* names are DX6.

| state | value |
|---|---|
| TEXTUREHANDLE | 0 |
| ANTIALIAS | 0 |
| TEXTUREADDRESS | CLAMP |
| TEXTUREPERSPECTIVE | 1 |
| WRAPU / WRAPV | 0 |
| ZENABLE | 0 |
| FILLMODE | SOLID |
| SHADEMODE | GOURAUD |
| MONOENABLE | 0 |
| ZWRITEENABLE | 0 |
| ALPHATESTENABLE | 0 |
| LASTPIXEL | 0 |
| TEXTUREMAG / TEXTUREMIN | `*[+0x60] ? LINEAR : NEAREST` |
| SRCBLEND | ONE |
| DESTBLEND | INVSRCALPHA |
| TEXTUREMAPBLEND | MODULATE |
| CULLMODE | NONE (no culling, ever) |
| DITHERENABLE | `*[+0x54]` |
| ALPHABLENDENABLE | 0 |
| FOGENABLE | 0 |
| FOGCOLOR | current fog colour |
| FOGTABLEMODE | NONE (vertex fog through specular alpha) |
| SPECULARENABLE | **1** |
| ZVISIBLE | 0 |
| SUBPIXEL | 1 |
| STIPPLEDALPHA / STIPPLEENABLE | 0 |
| ZFUNC | ALWAYS |
| COLORKEYENABLE | 0 |
| ALPHAREF | **0** |
| ALPHAFUNC | **GREATER** |

The device then runs the endScene reset and EndScene.

In D3D9 terms, set these as well:

* `D3DRS_FOGVERTEXMODE = NONE` and `D3DRS_FOGTABLEMODE = NONE`. With
  FOGENABLE on, pretransformed vertices use specular alpha as the fog
  factor, which is the same as DX6.
* `D3DRS_LIGHTING = FALSE`.

### Per-draw state from `flags` (0x10003f10)

This is applied at the start of each drawPolygon2/drawPolyList/drawPolyList2
call. It compares against the previous flags and does SetRenderState or
SetTextureStageState only on change, flushing the batch first.

**Preprocessing:** if the current texture has an alpha plane and
`flags & 1`, then `flags |= 0x22`.

| bit | when set | when clear |
|---|---|---|
| 0x001 **textured** | stage0 COLOROP=MODULATE; MAG/MIN filter per `*[+0x60]`; MIPFILTER `*[+0x6c] ? LINEAR : NONE`; `SetTexture(0, current)` | stage0 COLOROP=DISABLE (diffuse passes through) |
| 0x002 **translucent** | ALPHABLENDENABLE=1, ALPHATESTENABLE=1 (ref 0, GREATER, so alpha 0 is discarded), TEXTUREMAPBLEND=MODULATEALPHA | blend off, alpha test off, TEXTUREMAPBLEND=MODULATE |
| 0x004 **gouraud** | SHADEMODE=GOURAUD; per-vertex light (+0x20) or per-vertex colour index | SHADEMODE=FLAT |
| 0x008 **fog** | FOGENABLE=1 (vertex fog from +0x2c) | FOGENABLE=0 |
| 0x010 | take the flat light level from `*[+0x28]` for this call | flat light = 255 |
| 0x020 **src alpha** | SRCBLEND=SRCALPHA, **and clears 0x008** (no fog) | SRCBLEND=ONE (premultiplied-style) |
| 0x040 / 0x080 **Z** | 0x40+0x80: ZENABLE=1, ZWRITE=1, ZFUNC=LESSEQUAL. 0x40 only: ZENABLE=1, ZWRITE=0, LESSEQUAL (test-only). 0x80 only: ZENABLE=1, ZWRITE=1, ZFUNC=ALWAYS (write-only) | neither: ZENABLE=0, ZWRITE=0, ZFUNC=ALWAYS |
| 0x100 | textured: per-vertex alpha from `+0x2c>>8` | alpha = `*[+0x2c]` (global) |
| 0x200 **RGB** | per-vertex RGB from +0x20/+0x24/+0x28 (8.8); no overbright | mono light / palette colour |

**Blend mode** is not a flag. It comes from exe global `*[+0x24]`: 0 gives
DESTBLEND=INVSRCALPHA and 1 gives DESTBLEND=ONE (additive). With
premultiplyColorAndAlpha (default off), SRCBLEND is also forced to SRCALPHA
(mode 0) or ONE (mode 1). Blending only happens when flag 0x002 is set.

Texture transparency comes from the conversion rule "palette black means
alpha 0" together with alpha test. It is therefore visible **only on
polygons drawn with flag 0x002**. A typical opaque world polygon uses
**0xCD** (textured, gouraud, fog, Z test+write; `3d.c` 0x408f98).

For D3D9 fixed function:

* stage 0: `COLOROP = MODULATE(TEXTURE, DIFFUSE)` when textured, else
  `SELECTARG1(DIFFUSE)`;
* `ALPHAOP = MODULATE(TEXTURE, DIFFUSE)` when textured, else
  `SELECTARG1(DIFFUSE)`. This is equivalent to MODULATE vs. MODULATEALPHA,
  because alpha only matters when blending or testing is on.

### endScene / init state reset (0x10002d50)

1. Set `*[+0x2c] = 0` and `*[+0x24] = 1`.
2. Apply flags 0x226.
3. Set `*[+0x2c] = 0xFF` and `*[+0x24] = 0`.

The purpose is to force a known state. The pending batch is flushed first
because every state change flushes. A reimplementation should flush, then
invalidate its state cache and write those two exe globals.

## 8. Normal-flow summary (what the exe actually uses)

The exe calls these in normal flow:

* Information, init, selectCard, buildCardList, getVideoMemory
* setVideoMode2, restoreVideoMode, toggle (flip)
* beginScene, endScene, lockFrame/unlockFrame (HUD at 480 lines or fewer),
  lockHoldBuffer/unlockHoldBuffer (HUD above 480 lines)
* selectTexture/updateTexture (with palette), setColorTable16
* drawPolygon, drawPolygon2, drawPolyList, drawPolyList2, sync
* clear, clearZBuffer, clearZBox, setFogColor
* masterZBuffer/restoreZBuffer, kill

Resolved but **never called**: setVideoMode (it is used internally by
setVideoMode2), setMipMapLevel, addParticle, flushParticleList, add3dLine,
flushLineList, GetDisplayContext, ReleaseDisplayContext. A reimplementation
can make these trivial stubs that keep the same return values: 1 for
setMipMapLevel, 0 for the particle and line calls.

A typical frame runs in this order:

1. `beginScene`
2. `clear`
3. `clearZBuffer`
4. `restoreZBuffer(0, full)`
5. many `select/updateTexture` and `drawPolygon2`/`drawPolyList*` calls
6. `endScene`
7. HUD: `lockFrame`, exe software 2D, `unlockFrame`, `beginScene` (or the
   hold-buffer pair above 480 lines)
8. The exe's flip wrapper, which calls `toggle`.

Rendering the background runs `masterZBuffer(0)` after the background has
been drawn.

## 9. Notes for a D3D9 reimplementation

* Draw state is almost fixed-function-trivial. The only non-obvious parts are
  the specular-alpha vertex fog, the specular overbright, and the
  black-is-transparent conversion.
* lockFrame needs CPU-readable and writable pixels in the back buffer format,
  exposed through the exe's row table. Two options:
  * a `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER`;
  * a system-memory surface that is uploaded with `UpdateSurface` or
    `StretchRect` on unlock.
  Keep reporting 565 or X8R8G8B8 masks consistently through setColorTable16.
* master/restoreZBuffer are only ever used full-screen. A whole-surface
  `StretchRect` between depth surfaces (allowed outside Begin/EndScene) or a
  lockable depth format with memcpy both work.
* Every pointer that the pointer table, the texture arguments and the vertex
  arrays carry is a **32-bit address in the lifted image**. Resolve each one
  through the runtime's guest-to-host mapping.
