// W3HardwareCursor.mix - "hardware cursor" for Warcraft III 1.26a (Game.dll build 6401)
//
// The game draws its mouse cursor itself: the cursor is a 3D model (CCursorFrame, a CSpriteFrame) that is rendered into the
// back buffer with the rest of the interface, once per frame, at the mouse position. It therefore moves only as fast as
// frames are produced, and lags behind the real mouse by at least a frame (more with vsync / a render-ahead queue).
// A hardware cursor is drawn by the operating system, independent of the frame rate - like the option in World of Warcraft.
//
// HOW THIS PLUGIN DOES IT (the game's own cursor art and animations are kept):
//   1) The cursor frame's methods (both vtables of CCursorFrame) are wrapped with a small shadow stack, so every Direct3D draw
//      call that is made while the cursor frame draws itself is recognised (same technique as W3TrueWidescreen.mix).
//   2) Those draw calls are NOT sent to the back buffer any more. A few times a second (CaptureHz) they are replayed twice into
//      two private render targets, one cleared to black and one to white. From the two pictures the cursor's colour and alpha
//      are recovered (on black: a*c, on white: a*c + (1-a)  ->  a = 1 - (white - black), c = black / a).
//   3) The picture around the mouse position is cut out and made into a Windows cursor (CreateIconIndirect, hotspot = mouse
//      position); Windows shows it. When the game does not draw its cursor (cinematics, loading...), the Windows cursor is hidden.
//   4) Game.dll's SetCursor / ShowCursor imports are replaced so the game can not hide or change the Windows cursor meanwhile.
// If anything is not as expected the plugin switches itself off and the game's software cursor comes back.
//
// Optional W3HardwareCursor.ini next to war3.exe (no section needed):
//   Enabled=1      0 = do nothing
//   CaptureHz=30   how often the cursor picture is refreshed (animated cursors) 1..120
//   Debug=1        write the first captured cursor pictures into the folder W3HardwareCursor_captures\ (as .tga) and statistics to the log
// Log: Logs\W3HardwareCursor.log (next to war3.exe)
//
// Build: i686-w64-mingw32-gcc -O2 -Wall -shared -static-libgcc -s -o W3HardwareCursor.mix W3HardwareCursor.c -lversion -lwinmm -lgdi32 -luser32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;
typedef uint8_t  u8;

#define BUILD_126A 6401
// ---- Game.dll 1.26a ----
#define RVA_GX             0xACBD40     // pointer to the graphics object
#define OFS_GX_DEVICE      0x584        // IDirect3DDevice8* inside it
#define RVA_VT_CURSOR      0x97063C     // CCursorFrame, frame vtable   (slot 0 = 0x6F00C060)
#define RVA_VT_CURSOR_LAY  0x970610     // CCursorFrame, layout vtable  (slot 0 = 0x6F606DC0)
#define IAT_SETCURSOR      0x86D7D0
#define IAT_SHOWCURSOR     0x86D6FC

// ---- IDirect3DDevice8 / IDirect3DSurface8 vtable slots ----
enum { V_RELEASE = 2, V_RESET = 14, V_PRESENT = 15, V_GETCREATION = 9, V_CREATERT = 25, V_CREATEIMG = 27, V_COPYRECTS = 28, V_SETRT = 31, V_GETRT = 32, V_GETZS = 33, V_CLEAR = 36,
       V_SETVP = 40, V_GETVP = 41, V_DP = 70, V_DIP = 71, V_DPUP = 72, V_DIPUP = 73 };
enum { S_RELEASE = 2, S_GETDESC = 8, S_LOCKRECT = 9, S_UNLOCKRECT = 10 };

static u32   g_base;
static FILE* g_log;
static int   g_debug, g_captureMs = 33;
static volatile int g_off;        // plugin switched itself off
static DWORD g_sfThread;          // the thread that renders (first Present)

static void logf_(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}


// ---- diagnostics: breadcrumbs for the first captures (Debug=1) ----
static int g_trace, g_drawTrace;
#define TR(...) do { if (g_trace) logf_(__VA_ARGS__); } while (0)
static void DescribeAddr(const char* what, u32 a)
{
    HMODULE m = NULL; char n[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        char full[MAX_PATH]; if (GetModuleFileNameA(m, full, MAX_PATH)) { const char* b = strrchr(full, '\\'); strcpy(n, b ? b + 1 : full); }
        logf_("  %s = %08X (%s+%X)", what, a, n, a - (u32)m);
    } else logf_("  %s = %08X (not in a module)", what, a);
}

static int WriteMem(u32 addr, const void* data, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void*)addr, data, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    return 1;
}

static u32 GetGameBuild(void)
{
    DWORD h; DWORD sz = GetFileVersionInfoSizeA("Game.dll", &h);
    if (!sz) return 0;
    char* buf = (char*)malloc(sz); u32 build = 0;
    if (buf && GetFileVersionInfoA("Game.dll", h, sz, buf)) {
        VS_FIXEDFILEINFO* vi; UINT l;
        if (VerQueryValueA(buf, "\\", (LPVOID*)&vi, &l)) build = vi->dwFileVersionLS & 0xFFFF;
    }
    free(buf);
    return build;
}

static int IniInt(const char* file, const char* key, int def)
{
    FILE* f = fopen(file, "r");
    if (!f) return def;
    char line[256]; size_t kl = strlen(key); int val = def;
    while (fgets(line, sizeof line, f)) {
        char* p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '#' || *p == '[') continue;
        if (_strnicmp(p, key, kl)) continue;
        p += kl; while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        val = atoi(p + 1); break;
    }
    fclose(f);
    return val;
}

// ------------------------------------------------------------------------------------------------ shadow stack
// Every method of the cursor frame is entered through a stub that pushes the return address on a shadow stack and replaces
// it with SfExitStub; while the stack is not empty the cursor frame is drawing.
static u32  g_sfRet[64];
static int  g_sfDepth;
static u32  g_sfTotal;
extern void SfExitStub(void);

void __stdcall SfEnter(u32* retSlot)
{
    if (GetCurrentThreadId() != g_sfThread || g_sfDepth >= 64) return;
    g_sfRet[g_sfDepth++] = *retSlot;
    *retSlot = (u32)SfExitStub;
    g_sfTotal++;
}
u32 __stdcall SfLeave(void) { return g_sfDepth > 0 ? g_sfRet[--g_sfDepth] : 0; }
__asm__(".globl _SfExitStub\n_SfExitStub:\n"
        "  sub $4, %esp\n  push %eax\n  push %edx\n  push %ecx\n"
        "  call _SfLeave@0\n  mov %eax, 12(%esp)\n"
        "  pop %ecx\n  pop %edx\n  pop %eax\n  ret\n");

static int WrapVtable(u32 vt, u32 expectSlot0, const char* what)
{
    if (IsBadReadPtr((void*)vt, 8) || *(u32*)vt != expectSlot0) { logf_("vtable %s (%08X) not as expected, not wrapped", what, vt); return 0; }
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    u32 lo = g_base + nt->OptionalHeader.BaseOfCode, hi = lo + nt->OptionalHeader.SizeOfCode;
    int n = 0;
    while (n < 96 && !IsBadReadPtr((void*)(vt + n * 4), 4) && *(u32*)(vt + n * 4) >= lo && *(u32*)(vt + n * 4) < hi) n++;
    u8* code = (u8*)VirtualAlloc(NULL, n * 32 + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code || !n) return 0;
    for (int i = 0; i < n; i++) {
        u8* t = code + i * 32; u32 orig = *(u32*)(vt + i * 4);
        int k = 0;
        t[k++] = 0x8D; t[k++] = 0x04; t[k++] = 0x24;                       // lea eax,[esp]      (address of the return address)
        t[k++] = 0x52; t[k++] = 0x51; t[k++] = 0x50;                       // push edx / ecx / eax
        t[k++] = 0xE8; *(u32*)(t + k) = (u32)SfEnter - (u32)(t + k + 4); k += 4;   // call SfEnter (stdcall, 1 argument)
        t[k++] = 0x59; t[k++] = 0x5A;                                      // pop ecx / edx
        t[k++] = 0xE9; *(u32*)(t + k) = orig - (u32)(t + k + 4); k += 4;   // jmp original method
        u32 np = (u32)t; WriteMem(vt + i * 4, &np, 4);
    }
    logf_("%d methods of %s wrapped (vtable %06X)", n, what, vt - g_base);
    return 1;
}

// ------------------------------------------------------------------------------------------------ the Windows cursor
static HWND    g_hwnd;
static HCURSOR g_hc;                 // our cursor
static int     g_active;             // 1: Game.dll's SetCursor / ShowCursor are taken over
static int     g_visShown = -1;      // what is applied now
static LONG    g_origClassCursor;
static HCURSOR (WINAPI *real_SetCursor)(HCURSOR);
static int     (WINAPI *real_ShowCursor)(BOOL);

static HCURSOR WINAPI h_SetCursor(HCURSOR h)
{
    if (g_active) return real_SetCursor(g_visShown == 1 ? g_hc : NULL);
    return real_SetCursor(h);
}
static int WINAPI h_ShowCursor(BOOL b)
{
    if (g_active) return b ? 0 : -1;           // the game believes it hides / shows the cursor; the display count is ours
    return real_ShowCursor(b);
}

static u32 FindImportSlot(u32 rva) { return g_base + rva; }

static void InstallCursorImports(void)
{
    u32 s1 = FindImportSlot(IAT_SETCURSOR), s2 = FindImportSlot(IAT_SHOWCURSOR);
    real_SetCursor  = (HCURSOR (WINAPI*)(HCURSOR))GetProcAddress(GetModuleHandleA("user32.dll"), "SetCursor");
    real_ShowCursor = (int (WINAPI*)(BOOL))GetProcAddress(GetModuleHandleA("user32.dll"), "ShowCursor");
    if (!real_SetCursor || !real_ShowCursor || *(u32*)s1 != (u32)real_SetCursor || *(u32*)s2 != (u32)real_ShowCursor) {
        // the slots may point at a forwarded / wrapped implementation: use what they hold as the original
        real_SetCursor  = (HCURSOR (WINAPI*)(HCURSOR))*(u32*)s1;
        real_ShowCursor = (int (WINAPI*)(BOOL))*(u32*)s2;
    }
    void* a = (void*)h_SetCursor; void* b = (void*)h_ShowCursor;
    WriteMem(s1, &a, 4); WriteMem(s2, &b, 4);
    logf_("SetCursor / ShowCursor imports taken over (idle until the first cursor picture exists)");
}

static void ApplyVisible(int vis)
{
    if (!g_hc || !g_active) return;
    if (vis == g_visShown) return;
    g_visShown = vis;
    if (g_hwnd) SetClassLongA(g_hwnd, GCL_HCURSOR, vis ? (LONG)g_hc : 0);
    real_SetCursor(vis ? g_hc : NULL);
}

static void Activate(void)
{
    if (g_active) return;
    if (g_hwnd) g_origClassCursor = (LONG)GetClassLongA(g_hwnd, GCL_HCURSOR);
    int n, guard = 0;
    do { n = real_ShowCursor(TRUE); } while (n < 0 && ++guard < 32);       // display count >= 0: the Windows cursor may show
    while (n > 0 && guard++ < 64) n = real_ShowCursor(FALSE);
    g_active = 1; g_visShown = -1;
    logf_("hardware cursor active");
}

static void Deactivate(const char* why)
{
    if (!g_active) return;
    g_active = 0;
    logf_("hardware cursor off: %s", why);
    if (g_hwnd) SetClassLongA(g_hwnd, GCL_HCURSOR, g_origClassCursor);
    real_SetCursor(NULL);
    int n, guard = 0; do { n = real_ShowCursor(FALSE); } while (n >= 0 && ++guard < 32);   // the game hides it again (software cursor)
    g_visShown = -1;
}

static HCURSOR MakeCursor(const u32* px, int w, int h, int hx, int hy, int empty)
{
    BITMAPV5HEADER bi; memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi; bi.bV5Width = w; bi.bV5Height = -h; bi.bV5Planes = 1; bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return NULL;
    memcpy(bits, px, (size_t)w * h * 4);
    int stride = ((w + 31) / 32) * 4;
    u8* m = (u8*)malloc((size_t)stride * h);
    memset(m, empty ? 0xFF : 0x00, (size_t)stride * h);                    // AND mask: 1 = transparent (only used when there is no alpha)
    HBITMAP mask = CreateBitmap(w, h, 1, 1, m);
    free(m);
    ICONINFO ii; ii.fIcon = FALSE; ii.xHotspot = hx; ii.yHotspot = hy; ii.hbmMask = mask; ii.hbmColor = color;
    HCURSOR hc = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return hc;
}

// ------------------------------------------------------------------------------------------------ Direct3D 8 side
typedef HRESULT (__stdcall *Draw4_t)(u32, u32, u32, u32);
typedef HRESULT (__stdcall *Draw5_t)(u32, u32, u32, u32, u32);
typedef HRESULT (__stdcall *Draw6_t)(u32, u32, u32, u32, u32, u32);
typedef HRESULT (__stdcall *Draw9_t)(u32, u32, u32, u32, u32, u32, u32, u32, u32);
typedef HRESULT (__stdcall *Present_t)(u32, u32, u32, u32, u32);
typedef HRESULT (__stdcall *Reset_t)(u32, u32);

static u32* g_hookedVt;
static void* g_o[80];                // original methods by vtable slot (only the hooked ones are filled)

#define DEV_CALL(slot, type, d, ...) (((type)(*(u32**)(d))[slot])((d), __VA_ARGS__))
typedef HRESULT (__stdcall *Fn_2)(u32, u32, u32);
typedef HRESULT (__stdcall *Fn_3)(u32, u32, u32, u32);
typedef HRESULT (__stdcall *Fn_7)(u32, u32, u32, u32, u32, u32, u32);

typedef struct { int kind; u32 a[8]; } DrawCall;
static HRESULT CallOrig(u32 d, const DrawCall* c)
{
    switch (c->kind) {
    case 0:  return ((Draw4_t)g_o[V_DP])(d, c->a[0], c->a[1], c->a[2]);
    case 1:  return ((Draw6_t)g_o[V_DIP])(d, c->a[0], c->a[1], c->a[2], c->a[3], c->a[4]);
    case 2:  return ((Draw5_t)g_o[V_DPUP])(d, c->a[0], c->a[1], c->a[2], c->a[3]);
    default: return ((Draw9_t)g_o[V_DIPUP])(d, c->a[0], c->a[1], c->a[2], c->a[3], c->a[4], c->a[5], c->a[6], c->a[7]);
    }
}

static u32 SurfCall(u32 s, int slot, u32 a, u32 b, u32 c) { return ((u32 (__stdcall*)(u32, u32, u32, u32))(*(u32**)s)[slot])(s, a, b, c); }
static void SurfRelease(u32* s) { if (*s) { ((ULONG (__stdcall*)(u32))(*(u32**)*s)[S_RELEASE])(*s); *s = 0; } }

static u32 g_rt[2];                  // black / white render targets
static int g_rtW, g_rtH; static u32 g_rtFmt;
static u32 g_oRT, g_oZ;              // the game's render target / depth surface while a capture is running
static u32 g_vp[6];                  // D3DVIEWPORT8 of the game
static struct { int active, decided; int mx, my; DWORD last; } g_cap;
static int  g_drawn, g_failStreak, g_dumped;
static struct { u32 draws, frames, caps, changes, dropped; DWORD t; } g_stat;

static u32 g_img; static int g_imgS;      // system memory staging surface for the read back
static void ReleaseTargets(void) { SurfRelease(&g_rt[0]); SurfRelease(&g_rt[1]); SurfRelease(&g_img); g_imgS = 0; g_rtW = g_rtH = 0; }

static int EnsureTargets(u32 d, int W, int H, u32 fmt)
{
    if (g_rt[0] && g_rt[1] && g_rtW == W && g_rtH == H) return 1;
    ReleaseTargets();
    u32 tryFmt[3] = { fmt, 22 /*X8R8G8B8*/, 21 /*A8R8G8B8*/ };
    for (int f = 0; f < 3; f++) {
        if (f && tryFmt[f] == fmt) continue;
        if (tryFmt[f] != 21 && tryFmt[f] != 22) continue;                   // 32 bit formats only
        int ok = 1;
        for (int i = 0; i < 2 && ok; i++) {
            u32 s = 0;
            HRESULT hr = ((HRESULT (__stdcall*)(u32, u32, u32, u32, u32, u32, u32*))(*(u32**)d)[V_CREATERT])(d, W, H, tryFmt[f], 0, 0 /*not lockable: read back with CopyRects*/, &s);
            if (hr != S_OK || !s) { ok = 0; logf_("CreateRenderTarget(%dx%d, format %u) failed: %08lX", W, H, tryFmt[f], hr); }
            else g_rt[i] = s;
        }
        if (ok) { g_rtW = W; g_rtH = H; g_rtFmt = tryFmt[f]; logf_("private render targets %dx%d, format %u", W, H, tryFmt[f]); return 1; }
        ReleaseTargets();
    }
    return 0;
}

static int GetWindowAndMouse(u32 d, int W, int H, int* mx, int* my)
{
    if (!g_hwnd || !IsWindow(g_hwnd)) {
        u32 cp[4] = { 0, 0, 0, 0 };                                           // D3DDEVICE_CREATION_PARAMETERS
        if (((HRESULT (__stdcall*)(u32, u32*))(*(u32**)d)[V_GETCREATION])(d, cp) == S_OK) g_hwnd = (HWND)cp[2];
        if (!g_hwnd || !IsWindow(g_hwnd)) g_hwnd = FindWindowA("Warcraft III", NULL);
        if (!g_hwnd) return 0;
    }
    POINT p; RECT rc;
    if (!GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p) || !GetClientRect(g_hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return 0;
    *mx = (int)((long long)p.x * W / rc.right); *my = (int)((long long)p.y * H / rc.bottom);
    return 1;
}

static int CaptureBegin(u32 d)
{
    g_trace = g_debug && g_stat.caps < 3; g_drawTrace = 0;
    TR("capture %u begin", g_stat.caps);
    g_oRT = g_oZ = 0;
    if (((HRESULT (__stdcall*)(u32, u32*))(*(u32**)d)[V_GETRT])(d, &g_oRT) != S_OK || !g_oRT) return 0;
    TR("  GetDepthStencilSurface");
    ((HRESULT (__stdcall*)(u32, u32*))(*(u32**)d)[V_GETZS])(d, &g_oZ);
    TR("  render target %08X, depth %08X", g_oRT, g_oZ);
    ((HRESULT (__stdcall*)(u32, u32*))(*(u32**)d)[V_GETVP])(d, g_vp);
    u32 desc[8];
    if (((HRESULT (__stdcall*)(u32, u32*))(*(u32**)g_oRT)[S_GETDESC])(g_oRT, desc) != S_OK) { SurfRelease(&g_oRT); SurfRelease(&g_oZ); return 0; }
    int W = (int)desc[6], H = (int)desc[7];
    TR("  backbuffer %dx%d format %u", W, H, desc[0]);
    int ok = EnsureTargets(d, W, H, desc[0]);
    TR("  targets ready: %d", ok);
    if (ok) ok = GetWindowAndMouse(d, W, H, &g_cap.mx, &g_cap.my);
    TR("  window / mouse: %d (%d,%d)", ok, g_cap.mx, g_cap.my);
    for (int i = 0; ok && i < 2; i++) {
        TR("  SetRenderTarget %d", i);
        ok = ((HRESULT (__stdcall*)(u32, u32, u32))(*(u32**)d)[V_SETRT])(d, g_rt[i], g_oZ) == S_OK;
        TR("  -> %d", ok);
        if (!ok) break;
        ((HRESULT (__stdcall*)(u32, u32*))(*(u32**)d)[V_SETVP])(d, g_vp);
        TR("  Clear %d", i);
        ok = ((HRESULT (__stdcall*)(u32, u32, u32, u32, u32, u32, u32))(*(u32**)d)[V_CLEAR])(d, 0, 0, 1 /*D3DCLEAR_TARGET*/, i ? 0xFFFFFFFF : 0xFF000000, 0, 0) == S_OK;
    }
    TR("  restore render target");
    ((HRESULT (__stdcall*)(u32, u32, u32))(*(u32**)d)[V_SETRT])(d, g_oRT, g_oZ);
    ((HRESULT (__stdcall*)(u32, u32*))(*(u32**)d)[V_SETVP])(d, g_vp);
    TR("  begin done: %d", ok);
    if (!ok) { SurfRelease(&g_oRT); SurfRelease(&g_oZ); return 0; }
    g_cap.active = 1;
    return 1;
}

static HRESULT CaptureDraw(u32 d, const DrawCall* c)
{
    int tr = g_trace && g_drawTrace++ < 6;
    if (tr) logf_("  capture draw #%d kind %d args %08X %08X %08X %08X", g_drawTrace, c->kind, c->a[0], c->a[1], c->a[2], c->a[3]);
    for (int i = 0; i < 2; i++) {
        if (tr) logf_("    SetRenderTarget %d", i);
        if (((HRESULT (__stdcall*)(u32, u32, u32))(*(u32**)d)[V_SETRT])(d, g_rt[i], g_oZ) != S_OK) goto fail;
        if (tr) logf_("    SetViewport");
        ((HRESULT (__stdcall*)(u32, u32*))(*(u32**)d)[V_SETVP])(d, g_vp);
        if (tr) logf_("    original draw");
        CallOrig(d, c);
        if (tr) logf_("    drawn");
    }
    if (tr) logf_("    restore");
    ((HRESULT (__stdcall*)(u32, u32, u32))(*(u32**)d)[V_SETRT])(d, g_oRT, g_oZ);
    ((HRESULT (__stdcall*)(u32, u32*))(*(u32**)d)[V_SETVP])(d, g_vp);
    if (tr) logf_("    done");
    return S_OK;
fail:
    ((HRESULT (__stdcall*)(u32, u32, u32))(*(u32**)d)[V_SETRT])(d, g_oRT, g_oZ);
    ((HRESULT (__stdcall*)(u32, u32*))(*(u32**)d)[V_SETVP])(d, g_vp);
    g_cap.active = 0; g_failStreak++;
    SurfRelease(&g_oRT); SurfRelease(&g_oZ);
    return CallOrig(d, c);
}

static HRESULT OnDraw(u32 d, DrawCall* c)
{
    if (g_off || g_sfDepth <= 0 || GetCurrentThreadId() != g_sfThread) return CallOrig(d, c);
    g_stat.draws++; g_drawn = 1;
    if (!g_cap.decided) {
        g_cap.decided = 1;
        if ((!g_hc || (DWORD)(GetTickCount() - g_cap.last) >= (DWORD)g_captureMs) && CaptureBegin(d)) { /* running */ }
        else if (!g_hc && !g_cap.active) { g_failStreak++; }
    }
    if (g_cap.active) return CaptureDraw(d, c);
    if (g_hc && g_active) { g_stat.dropped++; return S_OK; }                // the Windows cursor stands in for it
    return CallOrig(d, c);
}

static HRESULT __stdcall h_DP(u32 d, u32 a, u32 b, u32 c)                   { DrawCall k = { 0, { a, b, c } };  return OnDraw(d, &k); }
static HRESULT __stdcall h_DIP(u32 d, u32 a, u32 b, u32 c, u32 e, u32 f)    { DrawCall k = { 1, { a, b, c, e, f } }; return OnDraw(d, &k); }
static HRESULT __stdcall h_DPUP(u32 d, u32 a, u32 b, u32 c, u32 e)          { DrawCall k = { 2, { a, b, c, e } }; return OnDraw(d, &k); }
static HRESULT __stdcall h_DIPUP(u32 d, u32 a, u32 b, u32 c, u32 e, u32 f, u32 g, u32 h, u32 i)
                                                                             { DrawCall k = { 3, { a, b, c, e, f, g, h, i } }; return OnDraw(d, &k); }

// ---- reading the two pictures back and turning them into a cursor ----
static int ReadRegion(u32 d, u32 surf, int W, int H, int ox, int oy, int S, u32* out, u32 fill)
{
    // outside the screen nothing was drawn: black picture stays black, white picture stays white (= fully transparent)
    for (size_t i = 0; i < (size_t)S * S; i++) out[i] = fill;
    RECT r = { ox < 0 ? 0 : ox, oy < 0 ? 0 : oy, ox + S > W ? W : ox + S, oy + S > H ? H : oy + S };
    if (r.right <= r.left || r.bottom <= r.top) return 1;                  // entirely outside
    if (!g_img || g_imgS != S) {
        SurfRelease(&g_img);
        TR("  CreateImageSurface %dx%d format %u", S, S, g_rtFmt);
        HRESULT hr = ((HRESULT (__stdcall*)(u32, u32, u32, u32, u32*))(*(u32**)d)[V_CREATEIMG])(d, S, S, g_rtFmt, &g_img);
        if (hr != S_OK || !g_img) { logf_("CreateImageSurface failed: %08lX", hr); g_img = 0; return 0; }
        g_imgS = S;
    }
    POINT dp = { 0, 0 };
    TR("  CopyRects %ld,%ld-%ld,%ld", r.left, r.top, r.right, r.bottom);
    HRESULT hr = ((HRESULT (__stdcall*)(u32, u32, RECT*, u32, u32, POINT*))(*(u32**)d)[V_COPYRECTS])(d, surf, &r, 1, g_img, &dp);
    if (hr != S_OK) { logf_("CopyRects failed: %08lX", hr); return 0; }
    struct { int pitch; void* bits; } lr = { 0, NULL };
    TR("  LockRect");
    if (SurfCall(g_img, S_LOCKRECT, (u32)&lr, 0, 0x10 /*D3DLOCK_READONLY*/) != S_OK || !lr.bits) { logf_("LockRect failed"); return 0; }
    for (int y = 0; y < r.bottom - r.top; y++)
        memcpy(out + (size_t)(r.top - oy + y) * S + (r.left - ox), (u8*)lr.bits + (size_t)y * lr.pitch, (size_t)(r.right - r.left) * 4);
    ((HRESULT (__stdcall*)(u32))(*(u32**)g_img)[S_UNLOCKRECT])(g_img);          // UnlockRect takes no arguments (stdcall: a wrong count unbalances the stack)
    TR("  unlocked");
    return 1;
}

static void DumpTga(const u32* px, int w, int h)
{
    char path[MAX_PATH]; GetModuleFileNameA(NULL, path, MAX_PATH);
    char* s = strrchr(path, '\\'); if (s) *(s + 1) = 0;
    strcat(path, "W3HardwareCursor_captures");
    CreateDirectoryA(path, NULL);                                          // own folder for the pictures (exists already: fine)
    char name[64]; snprintf(name, sizeof name, "\\W3HardwareCursor_%02d.tga", g_dumped);
    strcat(path, name);
    FILE* f = fopen(path, "wb"); if (!f) return;
    u8 hd[18] = { 0, 0, 2, 0,0,0,0,0, 0,0,0,0, (u8)w, (u8)(w >> 8), (u8)h, (u8)(h >> 8), 32, 0x28 };   // 32 bit, top-left origin, 8 alpha bits
    fwrite(hd, 1, 18, f); fwrite(px, 4, (size_t)w * h, f); fclose(f);
}

static void CaptureFinish(u32 d)
{
    int W = g_rtW, H = g_rtH;
    int R = H / 9; if (R < 64) R = 64; if (R > 128) R = 128;
    int S = 2 * R, ox = g_cap.mx - R, oy = g_cap.my - R;
    u32* b = (u32*)malloc((size_t)S * S * 4 * 3); u32* w = b + (size_t)S * S; u32* o = w + (size_t)S * S;
    if (!b) return;
    TR("finish: reading back %dx%d around %d,%d", S, S, g_cap.mx, g_cap.my);
    int ok = ReadRegion(d, g_rt[0], W, H, ox, oy, S, b, 0xFF000000u) && ReadRegion(d, g_rt[1], W, H, ox, oy, S, w, 0xFFFFFFFFu);
    TR("finish: read back %d", ok);
    if (!ok) { logf_("could not read the render targets back"); g_failStreak++; free(b); return; }
    int x0 = S, y0 = S, x1 = -1, y1 = -1;
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        u32 pb = b[y * S + x], pw = w[y * S + x];
        int r1 = (pb >> 16) & 255, g1 = (pb >> 8) & 255, b1 = pb & 255, r2 = (pw >> 16) & 255, g2 = (pw >> 8) & 255, b2 = pw & 255;
        int a = 255 - ((r2 - r1) + (g2 - g1) + (b2 - b1)) / 3;
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        u32 px = 0;
        if (a > 1) {
            int r = r1 * 255 / a, g = g1 * 255 / a, bl = b1 * 255 / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (bl > 255) bl = 255;
            px = ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        o[y * S + x] = px;
    }
    int empty = (x1 < 0);
    if (empty) { x0 = y0 = R; x1 = y1 = R; }
    // the hot spot is inside the picture
    if (R < x0) x0 = R;
    if (R > x1) x1 = R;
    if (R < y0) y0 = R;
    if (R > y1) y1 = R;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    u32* c = (u32*)malloc((size_t)cw * ch * 4);
    if (c) {
        for (int y = 0; y < ch; y++) memcpy(c + (size_t)y * cw, o + (size_t)(y0 + y) * S + x0, (size_t)cw * 4);
        static u32* prev; static int pw_, ph_, phx, phy;
        int same = prev && pw_ == cw && ph_ == ch && phx == R - x0 && phy == R - y0 && !memcmp(prev, c, (size_t)cw * ch * 4);
        if (!same) {
            HCURSOR hc = MakeCursor(c, cw, ch, R - x0, R - y0, empty);
            if (hc) {
                HCURSOR old = g_hc; int first = !g_hc;
                g_hc = hc;
                if (first) Activate();
                g_visShown = -1;                                   // re-apply with the new handle
                ApplyVisible(1);
                if (old) DestroyIcon(old);
                free(prev); prev = c; pw_ = cw; ph_ = ch; phx = R - x0; phy = R - y0; c = NULL;
                g_stat.changes++;
                if (g_debug && g_dumped < 30) { DumpTga(prev, cw, ch); g_dumped++; }
                if (first) logf_("first cursor picture: %dx%d, hot spot %d,%d, mouse %d,%d in a %dx%d frame", cw, ch, R - x0, R - y0, g_cap.mx, g_cap.my, W, H);
            } else { logf_("CreateIconIndirect failed (%lu)", GetLastError()); g_failStreak++; }
        }
        free(c);
    }
    free(b);
    g_stat.caps++; g_failStreak = 0;
}

static void FrameEnd(u32 d)
{
    if (g_cap.active) {
        TR("frame end: finishing capture");
        g_cap.active = 0;
        CaptureFinish(d);
        SurfRelease(&g_oRT); SurfRelease(&g_oZ);
        g_cap.last = GetTickCount();
    }
    g_cap.decided = 0;
    if (g_hc && g_active) ApplyVisible(g_drawn ? 1 : 0);
    g_drawn = 0;
    g_stat.frames++;
    if (g_failStreak >= 5 && !g_off) {
        g_off = 1; logf_("too many failures, plugin switched off");
        Deactivate("failures");
    }
    DWORD now = GetTickCount();
    if (g_debug && now - g_stat.t > 5000) {
        g_stat.t = now;
        logf_("stats: %u frames, %u cursor draw calls (%u dropped), %u captures, %u cursor changes, shadow-stack entries %u, depth %d",
              g_stat.frames, g_stat.draws, g_stat.dropped, g_stat.caps, g_stat.changes, g_sfTotal, g_sfDepth);
    }
}

static HRESULT __stdcall h_Present(u32 d, u32 a, u32 b, u32 c, u32 e)
{
    if (!g_sfThread) { g_sfThread = GetCurrentThreadId(); logf_("render thread %lu, device %08X", g_sfThread, d); }
    if (GetCurrentThreadId() == g_sfThread) {
        if (g_sfDepth != 0) { static int once; if (!once++) logf_("note: Present called from inside the cursor frame (shadow stack depth %d)", g_sfDepth); }
        if (!g_off) FrameEnd(d);
    }
    return ((Present_t)g_o[V_PRESENT])(d, a, b, c, e);
}

// Our render targets and the staging surface are children of the game's device. If the game destroys the device (shutdown)
// while they are alive, d3d8.dll is unloaded with objects still pointing into it and the process crashes on exit. So whenever the
// game calls Release on the device (the last call of its shutdown included) our surfaces are let go first; they are simply
// created again by the next capture if the game keeps running.
static ULONG __stdcall h_Release(u32 d)
{
    static int busy;
    if (!busy && !g_cap.active && (g_rt[0] || g_rt[1] || g_img)) {
        busy = 1;
        ReleaseTargets();
        static int logged; if (!logged++) logf_("device Release called: private surfaces released first");
        busy = 0;
    }
    return ((ULONG (__stdcall*)(u32))g_o[V_RELEASE])(d);
}

static HRESULT __stdcall h_Reset(u32 d, u32 pp)
{
    ReleaseTargets();                                                      // default-pool surfaces must be gone before Reset
    if (g_cap.active) { g_cap.active = 0; SurfRelease(&g_oRT); SurfRelease(&g_oZ); }
    return ((Reset_t)g_o[V_RESET])(d, pp);
}

// the object must really be a Direct3D 8 device (the game can also run on OpenGL, where the same place holds something else,
// and wrappers such as d3d8to9 / dgVoodoo implement the same interface): methods in the module loaded as d3d8.dll, or in a
// module that exports Direct3DCreate8, or an object that answers QueryInterface for IDirect3DDevice8
static const GUID kIID_D3DDevice8 = {0x7385e5df,0x8fe8,0x41d5,{0x86,0xb6,0xd7,0xb4,0x85,0x47,0xb6,0xcf}};
static int IsD3D8Device(u32 dev)
{
    static int state = -1;
    if (state >= 0) return state;
    u32* vt = *(u32**)dev;
    if (IsBadReadPtr(vt, 80 * 4)) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    u32 gameLo = g_base, gameHi = g_base + nt->OptionalHeader.SizeOfImage;
    state = 0;
    HMODULE m = NULL; char mn[MAX_PATH] = "?";
    if ((u32)vt >= gameLo && (u32)vt < gameHi) { logf_("device %08X: its vtable %08X is inside Game.dll - not Direct3D 8 (OpenGL renderer?), plugin idle", dev, (u32)vt); return state; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[36], &m) && m)
        GetModuleFileNameA(m, mn, sizeof mn);
    const char* how = NULL;
    HMODULE d8 = GetModuleHandleA("d3d8.dll");
    if (m && m == d8) how = "d3d8.dll";
    else if (m && m != (HMODULE)g_base && GetProcAddress(m, "Direct3DCreate8")) how = "exports Direct3DCreate8";
    else {
        int ind8 = 0;
        for (int i = 0; d8 && i < 42; i++) {
            HMODULE mi = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[i], &mi) && mi == d8) ind8++;
        }
        if (ind8 >= 21) how = "mostly d3d8.dll";
        else {
            typedef HRESULT (__stdcall *QI_t)(u32, const GUID*, void**);
            typedef ULONG (__stdcall *Rel_t)(u32);
            HMODULE m0 = NULL; u32 q = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[0], &m0) && m0 &&
                m0 != (HMODULE)g_base && ((QI_t)vt[0])(dev, &kIID_D3DDevice8, (void**)&q) == S_OK && q) {
                ((Rel_t)(*(u32**)q)[2])(q);
                how = "IDirect3DDevice8";
            }
        }
    }
    if (!how) { logf_("device %08X (methods in %s) is not Direct3D 8, plugin idle", dev, mn); return state; }
    // every slot we replace must point at code outside Game.dll
    static const int need[] = { V_RESET, V_PRESENT, V_SETRT, V_GETRT, V_GETZS, V_CLEAR, V_SETVP, V_GETVP, V_CREATERT, V_DP, V_DIP, V_DPUP, V_DIPUP };
    for (unsigned i = 0; i < sizeof need / sizeof need[0]; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)vt[need[i]], &mbi, sizeof mbi) || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            logf_("device vtable slot %d (%08X) is not code - plugin idle", need[i], vt[need[i]]); return state; }
    }
    state = 1;
    logf_("Direct3D 8 device %08X (%s: %s)", dev, mn, how);
    return state;
}

static void HookDevice(u32 dev)
{
    if (!IsD3D8Device(dev)) return;
    u32* vt = *(u32**)dev;
    if (vt == g_hookedVt) return;
    if (g_hookedVt) { logf_("device with a different vtable %08X (hooked: %08X) - not hooked", (u32)vt, (u32)g_hookedVt); return; }
    if (IsBadReadPtr(vt, 80 * 4)) return;
    const int slots[7] = { V_RELEASE, V_RESET, V_PRESENT, V_DP, V_DIP, V_DPUP, V_DIPUP };
    void* fn[7] = { (void*)h_Release, (void*)h_Reset, (void*)h_Present, (void*)h_DP, (void*)h_DIP, (void*)h_DPUP, (void*)h_DIPUP };
    for (int i = 0; i < 7; i++) { if (vt[slots[i]] == (u32)fn[i]) return; g_o[slots[i]] = (void*)vt[slots[i]]; }
    for (int i = 0; i < 7; i++) WriteMem((u32)&vt[slots[i]], &fn[i], 4);
    g_hookedVt = vt;
    logf_("Direct3D 8 device %08X, vtable %08X hooked (Present %08X)", dev, (u32)vt, (u32)g_o[V_PRESENT]);
    if (g_debug) {
        static const int sl[] = { V_GETCREATION, V_RESET, V_PRESENT, V_CREATERT, V_SETRT, V_GETRT, V_GETZS, V_CLEAR, V_SETVP, V_GETVP, V_DP, V_DIP, V_DPUP, V_DIPUP };
        for (unsigned i = 0; i < sizeof sl / sizeof sl[0]; i++) { char t[32]; snprintf(t, sizeof t, "vtable[%d]", sl[i]); DescribeAddr(t, g_o[sl[i]] ? (u32)g_o[sl[i]] : vt[sl[i]]); }
    }
}

static DWORD WINAPI Poller(LPVOID p)
{
    (void)p;
    for (;;) {
        u32 gx = *(u32*)(g_base + RVA_GX);
        if (gx && !IsBadReadPtr((void*)gx, OFS_GX_DEVICE + 4)) {
            u32 dev = *(u32*)(gx + OFS_GX_DEVICE);
            if (dev && !IsBadReadPtr((void*)dev, 4) && !IsBadReadPtr(*(void**)dev, 80 * 4)) HookDevice(dev);
        }
        Sleep(g_hookedVt ? 2000 : 200);
    }
    return 0;
}

static void Install(void)
{
    char path[MAX_PATH], ini[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* s = strrchr(path, '\\'); if (s) *(s + 1) = 0;
    strcpy(ini, path); strcat(ini, "W3HardwareCursor.ini");
    strcat(path, "Logs");
    CreateDirectoryA(path, NULL);                                          // the game's own Logs folder (created if missing)
    strcat(path, "\\W3HardwareCursor.log");
    g_log = fopen(path, "w");
    logf_("W3HardwareCursor 0.1");

    g_base = (u32)GetModuleHandleA("Game.dll");
    u32 build = GetGameBuild();
    logf_("Game.dll at %08X, build %u", g_base, build);
    if (!g_base || build != BUILD_126A) { logf_("unsupported game version (need 1.26a, build %d), doing nothing", BUILD_126A); return; }
    if (!IniInt(ini, "Enabled", 1)) { logf_("Enabled=0, doing nothing"); return; }
    g_debug = IniInt(ini, "Debug", 0);
    int hz = IniInt(ini, "CaptureHz", 30); if (hz < 1) hz = 1; if (hz > 120) hz = 120;
    g_captureMs = 1000 / hz;
    logf_("settings: CaptureHz=%d Debug=%d", hz, g_debug);

    if (!WrapVtable(g_base + RVA_VT_CURSOR, g_base + 0x00C060, "the cursor frame") ) { logf_("cursor frame not found, doing nothing"); return; }
    WrapVtable(g_base + RVA_VT_CURSOR_LAY, g_base + 0x606DC0, "the cursor frame layout");
    InstallCursorImports();
    CreateThread(NULL, 0, Poller, NULL, 0, NULL);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        HMODULE self; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)DllMain, &self);
        Install();
    }
    return TRUE;
}
