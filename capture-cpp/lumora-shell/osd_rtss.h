// RTSS-FPS-Leseweg + Afterburner-OSD-Toggle (1:1 aus main.js rtss*/toggleAbOsd).
// Laeuft RTSS/Afterburner ohnehin, lesen wir FPS/Frametime direkt aus dessen
// Shared Memory (RTSSSharedMemoryV2) - kein Admin, kein UAC, kein ETW-Konflikt.
// Alt+B schaltet ueber RTSSHooks64.dll das native Afterburner-OSD.
#pragma once
#include <windows.h>
#include <string>
#include <algorithm>

namespace lurtss {

#pragma pack(push, 1)
struct Hdr { uint32_t dwSignature, dwVersion, dwAppEntrySize, dwAppArrOffset, dwAppArrSize, dwOSDEntrySize, dwOSDArrOffset, dwOSDArrSize, dwOSDFrame; };
struct App { uint32_t dwProcessID; uint8_t szName[260]; uint32_t dwFlags, dwTime0, dwTime1, dwFrames, dwFrameTime; };
#pragma pack(pop)

// API-Name aus den Flags (nur fuer die Anzeige).
inline const char* apiName(uint32_t flags) {
    switch (flags & 0xffff) {
        case 1: return "OpenGL"; case 2: return "DirectDraw"; case 3: return "D3D8";
        case 4: case 5: return "D3D9"; case 6: return "D3D10"; case 7: return "D3D11";
        case 8: case 9: return "D3D12"; case 10: return "Vulkan"; default: return "";
    }
}

// Selbst-Ausschluss: RTSS hookt bei der Shell die WebView2-Renderer
// (msedgewebview2.exe) statt der Haupt-exe (bei Electron war es die Lumora.exe).
// Ohne Ausschluss gewinnt abwechselnd Lumoras eigene UI-Praesentation den
// "zuletzt aktiv"-Vergleich -> falsche FPS/springende Position (bei Electron gemessen).
inline std::string selfExeLower() {
    wchar_t p[MAX_PATH] = {}; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring w = p; size_t s = w.find_last_of(L'\\');
    std::wstring bn = (s == std::wstring::npos) ? w : w.substr(s + 1);
    std::string out; for (wchar_t c : bn) out += (char)tolower((unsigned char)(c < 128 ? c : '?'));
    return out;
}
inline bool isSelf(const App* e) {
    static std::string self = selfExeLower();
    char nm[261] = {}; memcpy(nm, e->szName, 260); nm[260] = 0;
    std::string s(nm);   // stoppt am ersten NUL
    size_t bs = s.find_last_of('\\'); std::string bn = (bs == std::string::npos) ? s : s.substr(bs + 1);
    for (auto& c : bn) c = (char)tolower((unsigned char)c);
    return bn == self || bn == "msedgewebview2.exe";
}

inline bool available() {
    HANDLE h = OpenFileMappingA(0x0004 /*FILE_MAP_READ*/, FALSE, "RTSSSharedMemoryV2");
    if (h) { CloseHandle(h); return true; }
    return false;
}

struct FpsOut { bool ok = false; int fps = 0; double frametime = 0; std::string api; };
// Zuletzt aktiv praesentierenden App-Eintrag lesen. dwLastForegroundApp @64
// (v2.16+) ist stabil die richtige Semantik; sonst der frischeste Eintrag.
inline FpsOut readFps() {
    FpsOut out;
    HANDLE h = OpenFileMappingA(0x0004, FALSE, "RTSSSharedMemoryV2");
    if (!h) return out;
    void* base = MapViewOfFile(h, 0x0004, 0, 0, 0);
    if (!base) { CloseHandle(h); return out; }
    const uint8_t* b = (const uint8_t*)base;
    const Hdr* hdr = (const Hdr*)base;
    uint32_t now = GetTickCount();
    auto fresh = [&](const App* e) { return e->dwProcessID && e->dwFrameTime && ((uint32_t)(now - e->dwTime1) <= 1500) && !isSelf(e); };
    const App* best = nullptr;
    if (hdr->dwVersion >= 0x20010 && hdr->dwAppEntrySize) {
        uint32_t idx = *(const uint32_t*)(b + 64);   // dwLastForegroundApp
        if (idx < hdr->dwAppArrSize) {
            const App* e = (const App*)(b + hdr->dwAppArrOffset + (size_t)idx * hdr->dwAppEntrySize);
            if (fresh(e)) best = e;
        }
    }
    if (!best && hdr->dwAppEntrySize) {
        uint32_t n = (std::min)(hdr->dwAppArrSize, 4096u);
        for (uint32_t i = 0; i < n; ++i) {
            const App* e = (const App*)(b + hdr->dwAppArrOffset + (size_t)i * hdr->dwAppEntrySize);
            if (!fresh(e)) continue;
            if (!best || e->dwTime1 > best->dwTime1) best = e;
        }
    }
    if (best) {
        uint32_t dt = best->dwTime1 - best->dwTime0;
        out.ok = true;
        out.fps = dt > 0 ? (int)((double)best->dwFrames * 1000.0 / dt + 0.5)
                         : (int)(1000000.0 / best->dwFrameTime + 0.5);
        out.frametime = (int)(best->dwFrameTime / 100.0 + 0.5) / 10.0;   // ms, 1 Dezimale
        out.api = apiName(best->dwFlags);
    }
    UnmapViewOfFile(base); CloseHandle(h);
    return out;
}

// --- Afterburner-OSD-Toggle (Alt+B) via RTSSHooks64.dll ----------------------
typedef uint32_t (*GetFlags_t)();
typedef void (*SetFlags_t)(uint32_t, uint32_t);
static HMODULE g_hooks = (HMODULE)-1;   // -1 = noch nicht versucht
static GetFlags_t g_getFlags = nullptr;
static SetFlags_t g_setFlags = nullptr;
inline bool hooksReady() {
    if (g_hooks == (HMODULE)-1) {
        g_hooks = nullptr;
        for (const wchar_t* p : { L"C:\\Program Files (x86)\\RivaTuner Statistics Server\\RTSSHooks64.dll",
                                  L"C:\\Program Files\\RivaTuner Statistics Server\\RTSSHooks64.dll" }) {
            HMODULE m = LoadLibraryW(p);
            if (m) { g_hooks = m; g_getFlags = (GetFlags_t)GetProcAddress(m, "GetFlags"); g_setFlags = (SetFlags_t)GetProcAddress(m, "SetFlags"); break; }
        }
    }
    return g_getFlags && g_setFlags;
}
// Schaltet das globale RTSS-Sichtbarkeits-Bit (Bit 0) - exakt das Bit, das auch
// Afterburners eigener OSD-Hotkey nutzt. Im Fenster-Modus ist Lumoras OSD davon
// unabhaengig (eigenes Fenster), daher getrennt schaltbar.
inline bool toggleAbOsd() {
    if (!hooksReady()) return false;
    uint32_t before = g_getFlags();
    bool on = (before & 1) == 1;
    g_setFlags(~1u, on ? 0 : 1);
    return true;
}

} // namespace lurtss
