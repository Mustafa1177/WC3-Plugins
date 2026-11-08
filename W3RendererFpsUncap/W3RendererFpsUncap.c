// W3FpsUncap.mix - removes the 64 fps camera cap of Warcraft III 1.26a (Game.dll build 6401)
//
// Same fix as the hand-patched Game.dll ("Game_patched2.dll"), but applied in memory from a plugin:
//
//   Why the cap exists: Game.dll takes its millisecond clock from GetTickCount(). On current Windows that
//   counter only advances every 15.625 ms (1000 / 15.625 = 64), so every per-frame delta the engine computes -
//   including the one that moves the camera - is stepped 64 times a second, however fast frames are drawn.
//   The game's own 'maxfps' setting (200) is not involved.
//
//   The fix: Game.dll's GetTickCount import is replaced by a 1 ms clock built from QueryPerformanceCounter
//   (ms = counter * 1000 / frequency, 32-bit wrapping like GetTickCount). Every user of the import in Game.dll
//   (the clock thunk at 0x6F6C4E00 with its 119 callers, and the direct calls) gets the new clock, so all
//   values stay consistent with each other. As in the patched DLL, timeBeginPeriod(1) is requested as well.
//
// How it is loaded: the Miles sound library that the game uses loads every *.mix file from the game folder
// as a plain DLL, so DllMain runs inside the game process once Game.dll is mapped. Just drop the .mix next to
// war3.exe. Nothing on disk is modified.
//
// Build: i686-w64-mingw32-gcc -O2 -Wall -shared -static-libgcc -s -o W3FpsUncap.mix W3FpsUncap.c -lversion -lwinmm
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;

#define BUILD_126A 6401                      // Game.dll 1.26.0.6401

static u32   g_base;
static FILE* g_log;

static void logf_(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
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

// address of Game.dll's import slot for dll!name (0 if it has none)
static u32 FindImport(const char* dll, const char* name)
{
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(g_base + dd.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(g_base + d->Name), dll)) continue;
        u32* names = (u32*)(g_base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (int i = 0; names[i]; i++)
            if (!(names[i] & 0x80000000) && !strcmp((const char*)(g_base + names[i] + 2), name))
                return g_base + d->FirstThunk + i * 4;
    }
    return 0;
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

// ---- the 1 ms clock ----
static ULONGLONG g_freq;   // performance counter frequency (ticks per second), never 0 once installed

static DWORD WINAPI GetTickCount_hires(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    ULONGLONG n = (ULONGLONG)c.QuadPart;
    // n * 1000 / g_freq without overflow: whole seconds first, then the remainder (< g_freq <= 2^32, so * 1000 fits)
    ULONGLONG ms = (n / g_freq) * 1000ULL + (n % g_freq) * 1000ULL / g_freq;
    return (DWORD)ms;
}

static void Install(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* s = strrchr(path, '\\'); if (s) *(s + 1) = 0;
    strcat(path, "W3FpsUncap.log");
    g_log = fopen(path, "w");
    logf_("W3FpsUncap 1.1 (1 ms Game.dll clock)");

    g_base = (u32)GetModuleHandleA("Game.dll");
    u32 build = GetGameBuild();
    logf_("Game.dll at %08X, build %u", g_base, build);
    if (!g_base || build != BUILD_126A) { logf_("unsupported game version (need 1.26a, build %d), doing nothing", BUILD_126A); return; }

    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0 || (ULONGLONG)f.QuadPart > 0xFFFFFFFFULL) {
        logf_("performance counter unavailable or too fast (%lld Hz), doing nothing", (long long)f.QuadPart); return;
    }
    g_freq = (ULONGLONG)f.QuadPart;

    u32 slot = FindImport("kernel32.dll", "GetTickCount");
    if (!slot || !*(u32*)slot) { logf_("GetTickCount import not found, doing nothing"); return; }

    void* h = (void*)GetTickCount_hires;
    if (!WriteMem(slot, &h, 4)) { logf_("could not write the import slot, doing nothing"); return; }
    logf_("GetTickCount import at %08X now returns a 1 ms clock (counter frequency %llu Hz)", slot, (unsigned long long)g_freq);

    // the patched Game.dll also asked Windows for 1 ms timer resolution (shorter Sleep / wait granularity)
    timeBeginPeriod(1);
    logf_("timeBeginPeriod(1) requested");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        // Miles unloads / reloads .mix files when the sound provider changes; Game.dll's import slot points into
        // this module, so it must stay loaded
        HMODULE self; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)DllMain, &self);
        Install();
    }
    return TRUE;
}
