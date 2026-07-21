// lumora-shell (Phase-0-PoC des Electron-Ausstiegs): kleine native C++/Win32-Shell,
// die das ECHTE Lumora-UI (index.html/styles.css, unveraendert) im System-WebView2
// rendert - ohne gebuendeltes Chromium, ohne Node. Ein injizierter Shim faengt
// require('electron') ab und leitet ipcRenderer ueber window.chrome.webview an die
// Shell; als Roundtrip-Beweis beantwortet sie 'get-app-settings' mit den ECHTEN
// App-Einstellungen (%APPDATA%\lumora\app-settings.json) und 'shell-open-external'
// per ShellExecute. Alle anderen Kanaele antworten mit null (Promises haengen
// nicht); die Modul-Implementierungen folgen in Phase 2.
// Aufruf: lumora-shell [--appdir <Ordner mit index.html>]   (Default: aktuelles Verzeichnis)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dxgi1_4.h>
#include <dcomp.h>
#pragma comment(lib, "dcomp.lib")
#include <bcrypt.h>
#include <pdh.h>
#pragma comment(lib, "pdh.lib")
#include <Xinput.h>
#include <timeapi.h>
#pragma comment(lib, "xinput9_1_0.lib")
#pragma comment(lib, "winmm.lib")
#include <ctime>
#include <cmath>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "bcrypt.lib")
#include <wrl.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "WebView2.h"
#include "json.hpp"
#include "scan_games.h"
#include "artwork.h"
#include "launch_game.h"
#include "http_server.h"
#include "upnp.h"
#include "osd_broker.h"
#include "osd_rtss.h"
#include "osd_adl.h"
#include "group_lan.h"
#include "router_hints.h"
#include <thread>
#include <mutex>
#include <map>
#include <set>
#include <atomic>
#include <random>
#pragma comment(lib, "gdiplus.lib")

// Antworten aus Worker-Threads muessen vom UI-Thread gepostet werden (WebView2-Regel):
// Worker verpackt das fertige Antwort-JSON und stellt es per PostMessage zu.
#define WM_SHELL_REPLY (WM_APP + 1)

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using nlohmann::json;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static std::wstring g_appDir;
static HWND g_hwnd = nullptr;

// require('electron')-Shim: ipcRenderer.invoke/send/on ueber window.chrome.webview.
// invoke korreliert Antworten ueber eine laufende id; on-Listener werden von der Shell
// per {"channel":...,"payload":...} bedient (Push-Richtung, ab Phase 2 genutzt).
static const wchar_t* SHIM_JS = LR"JS(
(() => {
  const pending = new Map(); let seq = 0; const listeners = {};
  window.chrome.webview.addEventListener('message', (e) => {
    const d = e.data;
    if (d && d.id && pending.has(d.id)) { const p = pending.get(d.id); pending.delete(d.id); p.resolve(d.result); }
    else if (d && d.channel) { const as = Array.isArray(d.payloads) ? d.payloads : [d.payload]; (listeners[d.channel] || []).forEach((f) => { try { f({}, ...as) } catch {} }); }
  });
  const syncGet = (u) => { try { const x = new XMLHttpRequest(); x.open('GET', u + '?t=' + Date.now(), false); x.send(); return x.status === 200 ? x.responseText : null; } catch (e) { return null; } };
  const ipcRenderer = {
    invoke: (channel, ...args) => new Promise((resolve) => { const id = ++seq; pending.set(id, { resolve }); window.chrome.webview.postMessage({ id, channel, args }); }),
    send: (channel, ...args) => window.chrome.webview.postMessage({ id: 0, channel, args }),
    // sendSync-Emulation: Datendateien synchron ueber das data.lumora-Mapping (lokal).
    sendSync: (channel) => {
      if (channel === 'load-games-sync') return syncGet('https://data.lumora/games.json');
      if (channel === 'load-prefs-sync') return syncGet('https://data.lumora/prefs.json');
      if (channel === 'get-version-sync') return '%SHELL_VERSION%';
      return null;
    },
    on: (channel, fn) => { (listeners[channel] = listeners[channel] || []).push(fn); },
    removeAllListeners: (channel) => { delete listeners[channel]; },
  };
  window.require = (m) => (m === 'electron')
    ? { ipcRenderer, shell: { openExternal: (u) => ipcRenderer.invoke('shell-open-external', u) } }
    : {};
})();
)JS";

static std::string readFile(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary); if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
static bool writeFile(const std::wstring& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc); if (!f) return false;   // binaer = UTF-8 OHNE BOM (BOM = JSON-Datenverlust-Falle!)
    f.write(data.data(), (std::streamsize)data.size()); return (bool)f;
}
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}

// --- Modul: Einstellungen/Daten (gleiche Dateien wie die Electron-App -> Parallelbetrieb) ---
static std::wstring dataDir() {
    wchar_t appdata[MAX_PATH] = {}; GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    return std::wstring(appdata) + L"\\lumora";
}
static std::wstring settingsPath() { return dataDir() + L"\\app-settings.json"; }
static json loadSettings() {
    json j = json::parse(readFile(settingsPath()), nullptr, false);
    return j.is_object() ? j : json::object();
}
// Produkt-Version aus der eigenen VersionInfo (fuer get-version-sync im Shim).
static std::string shellVersion() {
    wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD h = 0, sz = GetFileVersionInfoSizeW(exe, &h); if (!sz) return "0.0.0";
    std::string buf(sz, 0);
    if (!GetFileVersionInfoW(exe, 0, sz, &buf[0])) return "0.0.0";
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&ffi, &len) || !ffi) return "0.0.0";
    return std::to_string(HIWORD(ffi->dwProductVersionMS)) + "." + std::to_string(LOWORD(ffi->dwProductVersionMS)) + "." + std::to_string(HIWORD(ffi->dwProductVersionLS));
}
// Push an das UI (der Shim verteilt {channel,payload} an ipcRenderer.on-Listener).
// THREADSICHER: immer ueber PostMessage in den UI-Thread (WebView2 verlangt das) -
// damit koennen auch Worker-Threads (launch-Monitor, Netz) direkt pushen.
static void sendToUi(const std::string& channel, const json& payload) {
    json m = { {"channel", channel}, {"payload", payload} };
    PostMessageW(g_hwnd, WM_SHELL_REPLY, 0, (LPARAM)new std::wstring(widen(m.dump())));
}
// Push mit MEHREREN Argumenten: Shim ruft dann f(event, a, b, ...). Fuer
// Electron-kompatible Renderer-Handler wie osd-setup-status (e, msg, done).
static void sendToUiMulti(const std::string& channel, const json& payloadsArr) {
    json m = { {"channel", channel}, {"payloads", payloadsArr} };
    PostMessageW(g_hwnd, WM_SHELL_REPLY, 0, (LPARAM)new std::wstring(widen(m.dump())));
}

// --- Modul: Datei-/Ordner-Dialoge (IFileOpenDialog, modal wie in Electron) ---
static json pickPathDialog(const wchar_t* title, bool folder, const wchar_t* filterName, const wchar_t* filterSpec) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return nullptr;
    dlg->SetTitle(title);
    DWORD opts = 0; dlg->GetOptions(&opts);
    if (folder) dlg->SetOptions(opts | FOS_PICKFOLDERS);
    else if (filterSpec) { COMDLG_FILTERSPEC fs[2] = { { filterName, filterSpec }, { L"Alle Dateien", L"*.*" } }; dlg->SetFileTypes(2, fs); }
    if (FAILED(dlg->Show(g_hwnd))) return nullptr;             // abgebrochen
    ComPtr<IShellItem> item; if (FAILED(dlg->GetResult(&item))) return nullptr;
    LPWSTR p = nullptr; if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return nullptr;
    std::string out = narrow(p); CoTaskMemFree(p);
    return out;
}

// --- Modul: Datei-Icon als data-URL (SHGetFileInfo -> GDI+ -> PNG-Base64) ---
static std::string b64encode(const uint8_t* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t x = (uint32_t)d[i] << 16; if (i + 1 < n) x |= (uint32_t)d[i + 1] << 8; if (i + 2 < n) x |= d[i + 2];
        o.push_back(t[(x >> 18) & 63]); o.push_back(t[(x >> 12) & 63]);
        o.push_back(i + 1 < n ? t[(x >> 6) & 63] : '='); o.push_back(i + 2 < n ? t[x & 63] : '=');
    }
    return o;
}
static int pngClsid(CLSID* clsid) {
    UINT num = 0, size = 0; Gdiplus::GetImageEncodersSize(&num, &size); if (!size) return -1;
    std::vector<uint8_t> buf(size); auto* c = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, c);
    for (UINT i = 0; i < num; ++i) if (wcscmp(c[i].MimeType, L"image/png") == 0) { *clsid = c[i].Clsid; return 0; }
    return -1;
}
static json fileIconDataUrl(const std::wstring& path) {
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) || !sfi.hIcon) return nullptr;
    json out = nullptr;
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHICON(sfi.hIcon);
    if (bmp) {
        CLSID clsid; IStream* stm = nullptr;
        if (pngClsid(&clsid) == 0 && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stm))) {
            if (bmp->Save(stm, &clsid, nullptr) == Gdiplus::Ok) {
                HGLOBAL hg = nullptr; GetHGlobalFromStream(stm, &hg);
                if (hg) { SIZE_T sz = GlobalSize(hg); void* p = GlobalLock(hg);
                    if (p) out = "data:image/png;base64," + b64encode((uint8_t*)p, sz); GlobalUnlock(hg); }
            }
            stm->Release();
        }
        delete bmp;
    }
    DestroyIcon(sfi.hIcon);
    return out;
}

// --- Modul: Spielstart (Vollversion, Logik 1:1 aus main.js - s. launch_game.h) ---
#define TIMER_LAUNCH 100
#define TIMER_EXTWATCH 101
static std::mutex g_launchMx;
static std::vector<lulaunch::LaunchSession> g_launches;
static bool g_hdrByLauncher = false;                       // wer HDR gerade verwaltet (Eigen- ODER Fremdstart)
static std::vector<std::string> g_activeLaunchExes;        // exe(lower) der Eigenstart-Monitore
struct ExtSession { std::string gamePath, name; ULONGLONG startTs; int absent; bool hdrOn; };
static std::map<std::string, ExtSession> g_extSessions;    // exe(lower) -> Fremdstart-Session
static std::map<std::wstring, std::string> g_lnkExeCache;  // .lnk-Pfad -> Ziel-exe(lower)

// Alle laufenden Exe-Namen (lowercase) in einem Toolhelp-Schnappschuss (<1 ms).
static std::set<std::string> listRunningExes() {
    std::set<std::string> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) do {
        std::wstring n = pe.szExeFile; for (auto& c : n) c = towlower(c);
        out.insert(narrow(n));
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return out;
}
static json readLibraryGames() {
    json j = json::parse(readFile(dataDir() + L"\\games.json"), nullptr, false);
    return j.is_array() ? j : json::array();
}
static void sendExternalRunning() {   // Start-Knopf zeigt "laeuft" fuer Fremd-Sessions
    json arr = json::array();
    for (auto& [k, s] : g_extSessions) arr.push_back(s.gamePath);
    sendToUi("external-running", arr);
}
// Fremdstart-Watcher (2s-Takt wie Electron-Nachruestung: HDR geht an, BEVOR das Spiel
// seine Display-Faehigkeiten prueft; Ende nach 3 leeren Scans ~6s gegen DRM-Handoff).
static void extWatchTick() {
    std::lock_guard<std::mutex> lk(g_launchMx);
    auto running = listRunningExes();
    if (running.empty()) return;                      // Scan-Aussetzer -> nichts beenden
    // 1) Neue fremd gestartete Bibliotheksspiele erkennen
    for (auto& g : readLibraryGames()) {
        std::string p = g.value("path", ""); if (p.empty()) continue;
        std::string exe; std::string plow = p; for (auto& c : plow) c = (char)tolower((unsigned char)c);
        if (plow.size() > 4 && plow.rfind(".exe") == plow.size() - 4) { size_t sl = p.find_last_of("\\/"); exe = plow.substr(sl == std::string::npos ? 0 : sl + 1); }
        else if (plow.size() > 4 && plow.rfind(".lnk") == plow.size() - 4) {
            std::wstring wp = widen(p);
            auto it = g_lnkExeCache.find(wp);
            if (it == g_lnkExeCache.end()) {          // einmalig aufloesen (IShellLink, UI-Thread-STA)
                std::wstring n = lulaunch::resolveProcessName(wp); for (auto& c : n) c = towlower(c);
                g_lnkExeCache[wp] = narrow(n); continue;   // ab dem naechsten Tick beruecksichtigt
            }
            exe = it->second;
        }
        if (exe.empty() || exe.rfind(".exe") != exe.size() - 4) continue;
        bool activeLaunch = false; for (auto& a : g_activeLaunchExes) if (a == exe) { activeLaunch = true; break; }
        if (activeLaunch || g_extSessions.count(exe) || !running.count(exe)) continue;
        ExtSession s{ p, g.value("name", ""), GetTickCount64(), 0, false };
        lulaunch::playLog(dataDir(), "EXTERN erkannt: " + exe + " (" + s.name + ") hdr=" + (g.value("hdr", false) ? "true" : "false"));
        // HDR wie beim Eigenstart - nur wenn nicht schon eine andere Session HDR verwaltet
        if (g.value("hdr", false) && !g_hdrByLauncher) {
            lulaunch::setHDR(true); g_hdrByLauncher = true; s.hdrOn = true;
            sendToUi("hdr-status", true);
        }
        g_extSessions[exe] = s;
        sendExternalRunning();
    }
    // 2) Laufende Fremd-Sessions pruefen / beenden
    for (auto it = g_extSessions.begin(); it != g_extSessions.end();) {
        if (running.count(it->first)) { it->second.absent = 0; ++it; continue; }
        if (++it->second.absent < 3) { ++it; continue; }
        ExtSession s = it->second; it = g_extSessions.erase(it);
        sendExternalRunning();
        lulaunch::playLog(dataDir(), "EXTERN beendet: " + s.gamePath + " Dauer " + std::to_string((GetTickCount64() - s.startTs) / 1000) + "s");
        if (s.hdrOn) { lulaunch::setHDR(false); g_hdrByLauncher = false; sendToUi("hdr-status", false); }
        sendToUi("play-session", { {"gamePath", s.gamePath}, {"durationMs", (long long)(GetTickCount64() - s.startTs)} });
    }
}

// Ende einer Session: HDR ggf. aus, Spielzeit verbuchen, Button freigeben (wie endSession in main.js).
static void launchEndSession(lulaunch::LaunchSession& s, bool credit) {
    std::string exeLow = narrow(s.exeName); for (auto& c : exeLow) c = (char)tolower((unsigned char)c);
    g_activeLaunchExes.erase(std::remove(g_activeLaunchExes.begin(), g_activeLaunchExes.end(), exeLow), g_activeLaunchExes.end());   // Watcher darf wieder uebernehmen
    if (s.useHdr) { lulaunch::setHDR(false); g_hdrByLauncher = false; sendToUi("hdr-status", false); }
    if (credit && s.startTs) sendToUi("play-session", { {"gamePath", narrow(s.gamePath)}, {"durationMs", (long long)(GetTickCount64() - s.startTs)} });
    sendToUi("launch-status", "idle");
    s.done = true;
}
// 4s-Tick (UI-Timer): Start-Erkennung, 30s-Freigabe, 120s-Aufgabe, Ende nach 2 leeren Checks.
static void launchTick() {
    std::lock_guard<std::mutex> lk(g_launchMx);
    for (auto& s : g_launches) {
        if (s.done) continue;
        bool running = lulaunch::probeRunning(s);
        if (!s.started) {
            if (running) {
                s.started = true; s.startTs = GetTickCount64(); s.absent = 0;
                sendToUi("launch-status", "running");
                // HDR wurde bei der 30s-Freigabe geparkt? Spaetstarter doch noch da -> wieder an.
                if (s.useHdr && !g_hdrByLauncher) { lulaunch::setHDR(true); g_hdrByLauncher = true; sendToUi("hdr-status", true); }
                lulaunch::playLog(dataDir(), "STARTED nach +" + std::to_string((GetTickCount64() - s.launchTs) / 1000) + "s");
            } else {
                ULONGLONG waited = GetTickCount64() - s.launchTs;
                if (!s.reenabled && waited > 30000) {
                    s.reenabled = true; sendToUi("launch-status", "idle");
                    // Missglueckter Start (BF6-Fall): UI zeigt ab hier "idle" - dann darf HDR
                    // nicht bis zum 120s-Timeout anbleiben. Jetzt aus; kommt das Spiel doch
                    // noch (Spaetstart bis 120s), schaltet der STARTED-Zweig es wieder ein.
                    if (s.useHdr && g_hdrByLauncher) { lulaunch::setHDR(false); g_hdrByLauncher = false; sendToUi("hdr-status", false); lulaunch::playLog(dataDir(), "HDR aus nach 30s ohne Start-Erkennung"); }
                }
                if (waited > 120000) { lulaunch::playLog(dataDir(), "TIMEOUT - nie erkannt nach " + std::to_string(waited / 1000) + "s."); launchEndSession(s, false); }
            }
        } else if (running) s.absent = 0;
        else if (++s.absent >= 2) {
            lulaunch::playLog(dataDir(), "ENDED - Dauer " + std::to_string((GetTickCount64() - s.startTs) / 1000) + "s");
            launchEndSession(s, true);
        }
    }
    g_launches.erase(std::remove_if(g_launches.begin(), g_launches.end(), [](const lulaunch::LaunchSession& s) { return s.done; }), g_launches.end());
    if (g_launches.empty()) KillTimer(g_hwnd, TIMER_LAUNCH);
}

// launch-game-Handler (laeuft im Worker-Thread: HDR-Wartezeit + AUMID-Suche blockieren das UI nicht).
static json launchGame(const json& args) {
    std::wstring gamePath = widen(args[0].get<std::string>());
    json opts = args.size() >= 2 && args[1].is_object() ? args[1] : json::object();
    bool useHdr = opts.value("useHdr", false);
    bool admin = opts.value("admin", false);
    std::vector<std::wstring> largs = lulaunch::tokenizeArgs(widen(opts.value("args", "")));
    try {
        if (useHdr) {
            lulaunch::setHDR(true);
            sendToUi("hdr-status", true);
            sendToUi("launch-status", "hdr-wait");
            Sleep(3000);   // HDR-Umschaltzeit wie im Original
        }
        sendToUi("launch-status", "launching");
        std::wstring low = gamePath; for (auto& c : low) c = towlower(c);
        bool isLnk = low.size() > 4 && low.rfind(L".lnk") == low.size() - 4;
        bool isXbox = std::regex_search(gamePath, std::wregex(LR"(\\XboxGames\\)", std::regex::icase));
        std::string appId = isXbox ? "" : lulaunch::steamAppIdForExe(gamePath);
        std::wstring gameDir = gamePath.substr(0, gamePath.find_last_of(L'\\'));
        std::wstring argStr; for (auto& a : largs) { if (!argStr.empty()) argStr += L' '; argStr += a; }

        if (isXbox) {
            // UWP: Exe ist gesperrt -> Start ueber die AUMID (shell:appsFolder)
            std::wstring aumid = lulaunch::xboxAumidForGame(gamePath);
            if (!aumid.empty()) ShellExecuteW(nullptr, L"open", (L"shell:appsFolder\\" + aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else ShellExecuteW(nullptr, L"open", gamePath.c_str(), argStr.empty() ? nullptr : argStr.c_str(), gameDir.c_str(), SW_SHOWNORMAL);
        } else if (!appId.empty() && !admin) {
            // Steam-Spiel UEBER Steam (DRM + Steam-Spielzeit); Argumente via steam://run
            std::wstring url = largs.empty() ? (L"steam://rungameid/" + luart::toW(appId))
                : (L"steam://run/" + luart::toW(appId) + L"//" + luart::toW(luart::urlEnc(narrow(argStr))));
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (isLnk) {
            ShellExecuteW(nullptr, L"open", gamePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (admin) {
            ShellExecuteW(nullptr, L"runas", gamePath.c_str(), argStr.empty() ? nullptr : argStr.c_str(), gameDir.c_str(), SW_SHOWNORMAL);   // UAC
        } else {
            ShellExecuteW(nullptr, L"open", gamePath.c_str(), argStr.empty() ? nullptr : argStr.c_str(), gameDir.c_str(), SW_SHOWNORMAL);
        }

        lulaunch::LaunchSession s;
        s.gamePath = gamePath; s.gameDir = gameDir; s.isLnk = isLnk; s.useHdr = useHdr;
        s.appId = appId; s.launchTs = GetTickCount64();
        s.exeName = lulaunch::resolveProcessName(gamePath);
        if (useHdr) g_hdrByLauncher = true;
        lulaunch::playLog(dataDir(), "LAUNCH " + narrow(s.exeName) + " kind=" + (isXbox ? "xbox" : (!appId.empty() ? "steam" : (isLnk ? "lnk" : "direct"))) + " appid=" + (appId.empty() ? "-" : appId));
        {
            std::lock_guard<std::mutex> lk(g_launchMx);
            std::string exeLow = narrow(s.exeName); for (auto& c : exeLow) c = (char)tolower((unsigned char)c);
            g_activeLaunchExes.push_back(exeLow);         // Fremdstart-Watcher: dieses Spiel gehoert jetzt dem Eigenstart-Monitor
            auto ext = g_extSessions.find(exeLow);        // lief es schon fremd? Dort sauber abschliessen (keine Doppelzaehlung)
            if (ext != g_extSessions.end()) {
                sendToUi("play-session", { {"gamePath", ext->second.gamePath}, {"durationMs", (long long)(GetTickCount64() - ext->second.startTs)} });
                g_extSessions.erase(ext);
                sendExternalRunning();
            }
            g_launches.push_back(std::move(s));
        }
        SetTimer(g_hwnd, TIMER_LAUNCH, 4000, nullptr);   // Tick im UI-Thread
        return { {"success", true} };
    } catch (...) {
        if (useHdr) { lulaunch::setHDR(false); sendToUi("hdr-status", false); }
        sendToUi("launch-status", "idle");
        return { {"success", false}, {"error", "Startfehler"} };
    }
}

// --- Modul: Streaming-HTTP-Server (Port 8787, Routen 1:1 aus bcStartServer/main.js) ---
static const int BROADCAST_PORT = 8787;   // TCP: player.html + WHEP-Signalisierung (Proxy vor mediamtx)
static const int MTX_WHEP_PORT = 8889;    // localhost: mediamtx WHEP-HTTP (hinter dem Proxy)
static const char* MTX_PATH = "live";     // mediamtx-Pfadname
static const int MTX_RTSP_PORT = 8554, MTX_API_PORT = 9997, MTX_ICE_UDP = 8189;
static const int MTX_RTP_UDP = 8556, MTX_RTCP_UDP = 8557, MTX_TS_UDP = 8558;
static lusrv::HttpServer g_streamSrv;
static bool g_streamSrvUp = false;
static std::string g_playerHtmlCache;
static std::mutex g_qosMx;
struct QosEntry { ULONGLONG t; bool bad; double lossRate; int badStreak; };
static std::map<std::string, QosEntry> g_qosMap;   // Zuschauer-QoS fuer die adaptive Bitrate

static void bcLogStream(const std::string& msg) {   // gleiches Log wie die Electron-App
    wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
    FILE* f = nullptr; _wfopen_s(&f, (std::wstring(tmp) + L"\\lumora-stream.log").c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d  %s\n", st.wHour, st.wMinute, st.wSecond, msg.c_str());
    fclose(f);
}
// Zuschauer transparent informieren (Freeze-Frame/Bitrate-Toast) - wie bcBroadcastSwitch.
static void bcBroadcastSwitch(const std::string& kind, int kbit) {
    json d = { {"kind", kind}, {"kbit", kbit} };
    g_streamSrv.sse.broadcast("data: " + d.dump() + "\n\n");
}
// QoS-Bericht eines Players (Logik 1:1 inkl. der Render-Metrik-Lehren: frz/drop
// zaehlen nur bei echtem Paketverlust; badStreak gegen Einzel-Ruckler).
static void bcQosReport(const std::string& ip, const json& q) {
    if (!q.is_object()) return;
    if (ip == "::1" || ip.rfind("127.", 0) == 0) return;   // eigene Vorschau
    long long recv = (std::max)(0ll, q.value("recv", 0ll)), lost = (std::max)(0ll, q.value("lost", 0ll));
    long long drop = (std::max)(0ll, q.value("drop", 0ll)), frz = (std::max)(0ll, q.value("frz", 0ll));
    double lossRate = (lost + recv) > 0 ? (double)lost / (double)(lost + recv) : 0.0;
    bool bad = lossRate > 0.02 || ((frz > 0 || drop > 15) && lossRate > 0.005);
    std::string key = q.contains("id") && q["id"].is_string() ? q["id"].get<std::string>().substr(0, 40) : ip;
    std::lock_guard<std::mutex> lk(g_qosMx);
    auto prev = g_qosMap.find(key);
    int streak = bad ? ((prev != g_qosMap.end() ? prev->second.badStreak : 0) + 1) : 0;
    g_qosMap[key] = { GetTickCount64(), bad, lossRate, streak };
    if (bad) bcLogStream("qos: " + key + " lost=" + std::to_string(lost) + "/" + std::to_string(lost + recv) + " !");
}
// --- Verbindungstest (STUN-NAT-Erkennung + UPnP-/IPv6-Proben, 1:1 aus main.js) ---
struct StunResult { std::string ip; int port = 0; };
static bool stunQuery(SOCKET s, const char* host, int port, StunResult& out) {
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* ai = nullptr;
    if (getaddrinfo(host, std::to_string(port).c_str(), &hints, &ai) != 0 || !ai) return false;
    unsigned char req[20] = {}; req[0] = 0x00; req[1] = 0x01;   // Binding Request
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xa4; req[7] = 0x42; // Magic Cookie
    static std::random_device rd; for (int i = 8; i < 20; ++i) req[i] = (unsigned char)(rd() & 0xFF);
    sendto(s, (const char*)req, 20, 0, ai->ai_addr, (int)ai->ai_addrlen);
    freeaddrinfo(ai);
    DWORD to = 3000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    unsigned char buf[512];
    int n = recv(s, (char*)buf, sizeof(buf), 0);
    if (n < 24) return false;
    int off = 20;
    while (off + 4 <= n) {   // XOR-MAPPED-ADDRESS (0x0020) bzw. MAPPED-ADDRESS (0x0001)
        int type = (buf[off] << 8) | buf[off + 1], len = (buf[off + 2] << 8) | buf[off + 3];
        const unsigned char* v = buf + off + 4;
        if ((type == 0x0020 || type == 0x0001) && len >= 8) {
            bool x = type == 0x0020;
            out.port = ((v[2] << 8) | v[3]) ^ (x ? 0x2112 : 0);
            unsigned char b[4] = { v[4], v[5], v[6], v[7] };
            if (x) { b[0] ^= 0x21; b[1] ^= 0x12; b[2] ^= 0xa4; b[3] ^= 0x42; }
            char ip[20]; sprintf_s(ip, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
            out.ip = ip;
            return true;
        }
        off += 4 + len + ((4 - (len % 4)) % 4);
    }
    return false;
}
// Mehrere STUN-Server am SELBEN Socket: gleiche IP, verschiedene Ports = symmetrisches NAT.
static json detectNat() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return { {"ok", false} };
    sockaddr_in b{}; b.sin_family = AF_INET; bind(s, (sockaddr*)&b, sizeof(b));
    std::vector<StunResult> res;
    for (auto& [h, p] : std::vector<std::pair<const char*, int>>{ {"stun.l.google.com", 19302}, {"stun1.l.google.com", 19302}, {"stun.cloudflare.com", 3478} }) {
        StunResult r; if (stunQuery(s, h, p, r)) res.push_back(r);
    }
    closesocket(s);
    if (res.empty()) return { {"ok", false} };
    std::string ip = res[0].ip;
    int o0 = atoi(ip.c_str()); int o1 = atoi(ip.c_str() + ip.find('.') + 1);
    bool cgn = o0 == 100 && o1 >= 64 && o1 <= 127;
    bool priv = o0 == 10 || (o0 == 172 && o1 >= 16 && o1 <= 31) || (o0 == 192 && o1 == 168);
    std::set<int> ports; for (auto& r : res) ports.insert(r.port);
    return { {"ok", true}, {"ip", ip}, {"cgn", cgn}, {"priv", priv}, {"symmetric", ports.size() > 1} };
}
// Kompletter Verbindungstest (Texte 1:1 aus main.js - die UI zeigt sie unveraendert).
static json runConnectivityTest() {
    json steps = json::array();
    json nat = detectNat();
    if (!nat.value("ok", false)) {
        steps.push_back({ {"key","ip"},{"state","error"},{"label","\xC3\x96""ffentliche Adresse"},{"detail","Keine Antwort vom STUN-Server \xE2\x80\x93 ausgehendes UDP scheint blockiert (Firewall/Netzwerk). Streaming ist so nicht m\xC3\xB6glich."} });
        return { {"verdict","error"}, {"steps", steps} };
    }
    std::string ip = nat.value("ip", "");
    std::string v6 = luupnp::publicIPv6();
    bool v6Ok = false;
    if (!v6.empty()) { std::string id = luupnp::addPinhole(v6, MTX_ICE_UDP, "UDP"); if (!id.empty()) { v6Ok = true; luupnp::deletePinhole(id); } }
    if (nat.value("cgn", false) || nat.value("priv", false)) {
        if (v6Ok) steps.push_back({ {"key","ip"},{"state","warn"},{"label","\xC3\x96""ffentliche IPv4"},{"detail","Kein eigenes \xC3\xB6""ffentliches IPv4 (DS-Lite / Carrier-NAT, " + ip + "). Das ist aber ok \xE2\x80\x93 \xC3\xBC""ber IPv6 (siehe unten) bist du trotzdem direkt erreichbar."} });
        else steps.push_back({ {"key","ip"},{"state","error"},{"label","\xC3\x96""ffentliche IPv4"},{"detail","Dein Anschluss hat keine eigene \xC3\xB6""ffentliche IPv4 (DS-Lite / Carrier-NAT, " + ip + ") und auch kein nutzbares IPv6. Direktes Streaming ist so nicht m\xC3\xB6glich \xE2\x80\x93 ein Relay-Server w\xC3\xA4re n\xC3\xB6tig, oder beim Provider echtes IPv4 anfragen (oft kostenlos)."} });
    } else steps.push_back({ {"key","ip"},{"state","ok"},{"label","\xC3\x96""ffentliche IPv4"},{"detail", ip} });
    if (nat.value("symmetric", false))
        steps.push_back({ {"key","nat"},{"state","warn"},{"label","NAT-Typ"},{"detail","Symmetrisches NAT \xE2\x80\x93 manche Zuschauer erreichen dich evtl. nur \xC3\xBC""ber eine feste Portfreigabe oder einen Relay."} });
    else steps.push_back({ {"key","nat"},{"state","ok"},{"label","NAT-Typ"},{"detail","Cone-NAT \xE2\x80\x93 direkte Verbindung m\xC3\xB6glich."} });
    bool tcpOk = luupnp::mapPort(BROADCAST_PORT, "TCP", "Lumora Verbindungstest"); if (tcpOk) luupnp::unmapPort(BROADCAST_PORT, "TCP");
    bool udpOk = luupnp::mapPort(MTX_ICE_UDP, "UDP", "Lumora Verbindungstest"); if (udpOk) luupnp::unmapPort(MTX_ICE_UDP, "UDP");
    if (tcpOk && udpOk)
        steps.push_back({ {"key","upnp"},{"state","ok"},{"label","Router-Portfreigabe (UPnP)"},{"detail","Lumora kann die n\xC3\xB6tigen IPv4-Ports beim Streamstart automatisch \xC3\xB6""ffnen."} });
    else {
        std::string found = luupnp::routerName().empty() ? "" : ("Erkannter Router: " + luupnp::routerName() + ". ");
        std::string alt = v6Ok ? " (Zur Not l\xC3\xA4uft es aber \xC3\xBC""ber den IPv6-Direktweg \xE2\x80\x93 siehe unten.)" : "";
        steps.push_back({ {"key","upnp"},{"state","warn"},{"label","Router-Portfreigabe (UPnP)"},{"detail","Der Router \xC3\xB6""ffnet die IPv4-Ports nicht selbst. " + found + "Die genaue Schritt-f\xC3\xBCr-Schritt-Anleitung f\xC3\xBCr deinen Router bekommst du beim Streamstart im Stream-Tab." + alt} });
    }
    if (!v6.empty() && v6Ok)
        steps.push_back({ {"key","ipv6"},{"state","ok"},{"label","IPv6-Direktweg"},{"detail","Globales IPv6 vorhanden und die Firewall l\xC3\xA4sst sich automatisch \xC3\xB6""ffnen. Zuschauer mit IPv6 (Mobilfunk, moderne Anschl\xC3\xBCsse) erreichen dich direkt \xE2\x80\x93 auch ohne IPv4-Portfreigabe."} });
    else if (!v6.empty())
        steps.push_back({ {"key","ipv6"},{"state","warn"},{"label","IPv6-Direktweg"},{"detail","Globales IPv6 ist da, aber die Firewall l\xC3\xA4sst sich nicht automatisch \xC3\xB6""ffnen. " + (luupnp::routerName().empty() ? "" : ("Router: " + luupnp::routerName() + ". ")) + "Erlaube im Router die selbstst\xC3\xA4ndigen (IPv6-)Freigaben f\xC3\xBCr diesen PC."} });
    else steps.push_back({ {"key","ipv6"},{"state","warn"},{"label","IPv6-Direktweg"},{"detail","Kein globales IPv6 an diesem Anschluss \xE2\x80\x93 dieser Weg steht nicht zur Verf\xC3\xBC""gung."} });
    bool hasError = false, hasWarn = false;
    for (auto& st : steps) { if (st.value("state", "") == "error") hasError = true; if (st.value("state", "") == "warn") hasWarn = true; }
    return { {"verdict", hasError ? "error" : hasWarn ? "warn" : "ok"}, {"publicIp", ip}, {"steps", steps} };
}

// --- Tuersteher (Knock->Approve->Token, Logik 1:1; Freigabe-UI: doorman-list-Push) ---
extern std::string doormanAccessKey();   // = g_accessKey (Gruppen-Modul weiter unten)
struct Knock { std::string name; ULONGLONG at = 0, deniedAt = 0; std::string status; };
static std::mutex g_doorMx;
static std::map<std::string, Knock> g_knocks;              // vid -> Anfrage
static std::map<std::string, ULONGLONG> g_prevGrants;      // 15-Min-Karenz nach Stream-Neustart
static std::set<std::string> g_blockedIps;                 // kick mit Sperre (bis App-Ende)
// HMAC-SHA256 (BCrypt) als Hex - fuer den Vermittlungs-Nachweis ueber (SDP + '|' + ts).
static std::string hmacSha256Hex(const std::string& key, const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
        UCHAR digest[32];
        if (BCryptCreateHash(alg, &h, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) == 0 &&
            BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
            BCryptFinishHash(h, digest, 32, 0) == 0) {
            char hex[3];
            for (int i = 0; i < 32; ++i) { sprintf_s(hex, "%02x", digest[i]); out += hex; }
        }
        if (h) BCryptDestroyHash(h);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out;
}
static bool bcVerifyDoormanSig(const std::string& body, const std::string& sig, const std::string& ts) {
    std::string key = doormanAccessKey();
    if (key.empty() || sig.empty() || ts.empty()) return false;
    long long t = atoll(ts.c_str());
    if (!t || llabs((long long)time(nullptr) - t) > 60) return false;   // Replay-Schutz
    std::string expect = hmacSha256Hex(key, body + "|" + ts);
    if (expect.size() != sig.size()) return false;
    unsigned char diff = 0;                                             // zeitkonstanter Vergleich
    for (size_t i = 0; i < expect.size(); ++i) diff |= (unsigned char)(expect[i] ^ sig[i]);
    return diff == 0;
}
// --- Tuersteher-Freigabefenster (kleines Always-on-top mit doorman.html, wie Electron) ---
#define WM_SHELL_DOORMSG (WM_APP + 4)
#define WM_SHELL_DOORSYNC (WM_APP + 5)
static HWND g_doorHwnd = nullptr;
static ComPtr<ICoreWebView2Controller> g_doorCtrl;
static ComPtr<ICoreWebView2> g_doorWv;
static void onWebMessage(const std::wstring& raw, HWND replyWnd);   // (weiter unten)
static LRESULT CALLBACK doorWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SHELL_DOORMSG: { auto* s = (std::wstring*)l; if (s) { if (g_doorWv) g_doorWv->PostWebMessageAsJson(s->c_str()); delete s; } return 0; }
    case WM_SIZE: if (g_doorCtrl) { RECT rc; GetClientRect(h, &rc); g_doorCtrl->put_Bounds(rc); } return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void createDoormanWindow() {
    if (g_doorHwnd) return;
    static bool reg = false;
    if (!reg) { WNDCLASSW wc{}; wc.lpfnWndProc = doorWndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"LumoraDoor"; wc.hbrBackground = CreateSolidBrush(RGB(15, 15, 15)); RegisterClassW(&wc); reg = true; }
    HMONITOR hm = MonitorFromPoint({ 0,0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
    UINT dpi = GetDpiForSystem(); if (!dpi) dpi = 96;
    int w = MulDiv(360, dpi, 96), hgt = MulDiv(220, dpi, 96);
    g_doorHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"LumoraDoor", L"Lumora", WS_POPUP, mi.rcWork.right - w - 16, mi.rcWork.top + 16, w, hgt, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_doorHwnd) return;
    wchar_t lad[MAX_PATH] = {}; GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    std::wstring userData = std::wstring(lad) + L"\\lumora-shell";
    CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(res) || !env || !g_doorHwnd) return res;
                env->CreateCoreWebView2Controller(g_doorHwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT r2, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(r2) || !ctrl || !g_doorHwnd) return r2;
                            g_doorCtrl = ctrl; g_doorCtrl->get_CoreWebView2(&g_doorWv);
                            RECT rc; GetClientRect(g_doorHwnd, &rc); g_doorCtrl->put_Bounds(rc);
                            ComPtr<ICoreWebView2_3> wv3; g_doorWv.As(&wv3);
                            if (wv3) wv3->SetVirtualHostNameToFolderMapping(L"app.lumora", g_appDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            std::wstring shim = SHIM_JS; size_t vp = shim.find(L"%SHELL_VERSION%");
                            if (vp != std::wstring::npos) shim.replace(vp, 15, widen(shellVersion()));
                            g_doorWv->AddScriptToExecuteOnDocumentCreated(shim.c_str(), nullptr);
                            g_doorWv->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                    LPWSTR j = nullptr;
                                    if (SUCCEEDED(a->get_WebMessageAsJson(&j)) && j) { onWebMessage(j, g_doorHwnd); CoTaskMemFree(j); }
                                    return S_OK;
                                }).Get(), nullptr);
                            g_doorWv->Navigate(L"https://app.lumora/doorman.html");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}
static void bcSyncDoorman() {   // pending-Anfragen ans Freigabefenster; zeigen OHNE Fokus, leer -> verstecken
    json pend = json::array();
    { std::lock_guard<std::mutex> lk(g_doorMx);
      for (auto& [vid, k] : g_knocks) if (k.status == "pending") pend.push_back({ {"vid", vid}, {"name", k.name}, {"at", (long long)k.at} }); }
    bool de = true;   // Sprachwahl wie mainLang (V1: de; language-Setting folgt)
    if (g_doorHwnd) {
        json m = { {"channel", "doorman-list"}, {"payloads", json::array({ pend, de })} };
        PostMessageW(g_doorHwnd, WM_SHELL_DOORMSG, 0, (LPARAM)new std::wstring(widen(m.dump())));
        if (pend.empty()) ShowWindow(g_doorHwnd, SW_HIDE);
        else ShowWindow(g_doorHwnd, SW_SHOWNOACTIVATE);
    }
    sendToUi("doorman-list", pend);   // Haupt-UI weiter mitinformieren
}
static std::string bcApproveGate(const std::string& vid, const std::string& nameRaw) {
    if (vid.empty()) return "denied";
    json s = loadSettings();
    if (s.value("streamBanned", json::object()).contains(vid)) return "banned";     // dauerhaft gesperrt
    if (s.value("streamRegulars", json::object()).contains(vid)) return "granted";  // Stammgast
    std::lock_guard<std::mutex> lk(g_doorMx);
    auto it = g_knocks.find(vid);
    if (it != g_knocks.end() && it->second.status == "granted") return "granted";
    auto pg = g_prevGrants.find(vid);                                   // 15-Min-Karenz
    if (pg != g_prevGrants.end() && GetTickCount64() - pg->second < 15 * 60 * 1000ull) {
        g_knocks[vid] = { nameRaw.empty() ? "Gast" : nameRaw.substr(0, 24), GetTickCount64(), 0, "granted" };
        return "granted";
    }
    if (it != g_knocks.end() && it->second.status == "denied") {        // 60s-Ablehnungs-Cooldown
        if (GetTickCount64() - it->second.deniedAt < 60000) return "denied";
        g_knocks.erase(it); it = g_knocks.end();
    }
    std::string nm = nameRaw.substr(0, 24);
    size_t a = nm.find_first_not_of(' '); if (a == std::string::npos) nm.clear(); else { size_t b = nm.find_last_not_of(' '); nm = nm.substr(a, b - a + 1); }
    if (nm.empty()) return "pending";                                   // ohne Namen KEIN Popup (nur Link geoeffnet)
    if (it == g_knocks.end()) { g_knocks[vid] = { nm, GetTickCount64(), 0, "pending" }; }
    else if (it->second.status == "pending" && it->second.name != nm) it->second.name = nm;
    PostMessageW(g_hwnd, WM_SHELL_DOORSYNC, 0, 0);   // Fenster-Erstellung+Sync im UI-Thread
    return "pending";
}

// Die 8 Routen des Streaming-Servers (CORS/OPTIONS erledigt der Transport).
static lusrv::Response handleStreamHttp(const lusrv::Request& rq) {
    lusrv::Response rs;
    if (rq.method == "GET" && (rq.path == "/" || rq.path == "/index.html")) {
        if (g_playerHtmlCache.empty()) g_playerHtmlCache = readFile(g_appDir + L"\\player.html");
        rs.status = 200; rs.body = g_playerHtmlCache.empty() ? "<!doctype html><meta charset=utf-8>Player nicht gefunden." : g_playerHtmlCache;
        rs.headers["Content-Type"] = "text/html; charset=utf-8"; rs.headers["Cache-Control"] = "no-store";
        return rs;
    }
    if (rq.method == "GET" && rq.path == "/cfg") {
        json s = loadSettings();
        json ice = json::array({ { {"urls","stun:stun.l.google.com:19302"} }, { {"urls","stun:stun.cloudflare.com:3478"} } });
        std::string turl = s.value("streamTurnUrl", ""); bool turnOn = s.value("streamTurnEnabled", false) && !turl.empty();
        if (turnOn) {   // WebRTC verlangt bei turn: IMMER username+credential (Platzhalter bei auth-los)
            if (turl.rfind("turn:", 0) != 0 && turl.rfind("turns:", 0) != 0) turl = "turn:" + turl;
            std::string tu = s.value("streamTurnUser", ""), tp = s.value("streamTurnPass", "");
            ice.push_back({ {"urls", turl}, {"username", tu.empty() ? "lumora" : tu}, {"credential", tp.empty() ? "lumora" : tp} });
        }
        json cfg = { {"buffer", (std::max)(0, s.value("streamBufferMs", 120))}, {"iceServers", ice},
                     {"forceRelay", turnOn && s.value("streamTurnForce", false)} };
        rs.status = 200; rs.body = cfg.dump();
        rs.headers["Content-Type"] = "application/json"; rs.headers["Cache-Control"] = "no-store";
        return rs;
    }
    if (rq.method == "GET" && rq.path == "/instanz") {
        extern std::string instanzId(); extern std::string instanzGroup();
        json r = { {"lumora", true}, {"id", instanzId()}, {"group", instanzGroup().empty() ? json(nullptr) : json(instanzGroup())} };
        rs.status = 200; rs.body = r.dump();
        rs.headers["Content-Type"] = "application/json"; rs.headers["Cache-Control"] = "no-store";
        return rs;
    }
    if (rq.path == "/whep" || rq.path.rfind("/whep/", 0) == 0) {
        bool local = rq.clientIp == "::1" || rq.clientIp.rfind("127.", 0) == 0 || rq.clientIp.empty();
        if (rq.method == "POST" && g_blockedIps.count(rq.clientIp)) { rs.status = 403; rs.body = "blocked"; return rs; }
        // Tuersteher (1:1): POST braucht HMAC-Nachweis der Vermittlung (?sig=&ts=) + individuelle Freigabe (?vid=).
        if (rq.method == "POST" && !local && loadSettings().value("streamDoorman", false)) {
            auto qget = [&](const char* k) { std::string pat = std::string(k) + "="; size_t p = rq.query.find(pat);
                if (p != std::string::npos && (p == 0 || rq.query[p - 1] == '&')) { std::string v = rq.query.substr(p + pat.size()); size_t amp = v.find('&'); return amp == std::string::npos ? v : v.substr(0, amp); } return std::string(); };
            if (!bcVerifyDoormanSig(rq.body, qget("sig"), qget("ts"))) { rs.status = 401; rs.body = "unauthorized"; return rs; }
            std::string gate = bcApproveGate(qget("vid"), qget("name"));
            if (gate != "granted") { rs.status = 403; rs.body = gate; return rs; }
        }
        // Zuschauer-Name aus der Query (fuer "Wer schaut zu"); Auth-Query NICHT durchreichen.
        std::string reqName;
        size_t np = rq.query.find("name=");
        if (np != std::string::npos) { reqName = rq.query.substr(np + 5); size_t amp = reqName.find('&'); if (amp != std::string::npos) reqName = reqName.substr(0, amp); if (reqName.size() > 72) reqName = reqName.substr(0, 72); }
        std::string rest = rq.path.substr(5);   // '' oder '/<session>'
        std::string target = std::string("/") + MTX_PATH + "/whep" + rest;
        if (rq.method == "POST" && rest.empty() && !reqName.empty()) target += "?name=" + reqName;
        auto pr = lusrv::proxyLocal(MTX_WHEP_PORT, rq.method, target,
            rq.headers.count("content-type") ? rq.headers.at("content-type") : "",
            rq.headers.count("user-agent") ? rq.headers.at("user-agent") : "", rq.body);
        if (pr.status == 0) { rs.status = 502; rs.body = "mediamtx nicht erreichbar"; return rs; }
        rs.status = pr.status; rs.body = pr.body;
        for (auto& [k, v] : pr.headers) {
            if (k == "Location") {   // http://127.0.0.1:8889/live/whep/<id> -> /whep/<id>
                std::string loc = v; size_t h = loc.find("//");
                if (h != std::string::npos) { size_t sl = loc.find('/', h + 2); loc = sl == std::string::npos ? "/" : loc.substr(sl); }
                std::string pre = std::string("/") + MTX_PATH + "/whep"; size_t pp = loc.find(pre);
                if (pp != std::string::npos) loc = loc.substr(0, pp) + "/whep" + loc.substr(pp + pre.size());
                rs.headers["Location"] = loc;
            } else rs.headers[k] = v;
        }
        return rs;
    }
    if (rq.method == "GET" && rq.path == "/switch-events") { rs.status = 200; rs.takeoverSse = true; return rs; }
    if (rq.method == "POST" && rq.path == "/freeze-log") {
        json d = json::parse(rq.body, nullptr, false);
        if (d.is_object()) bcLogStream("freeze-client: kind=" + d.value("kind", "") + " reason=" + d.value("reason", "") + " ms=" + std::to_string(d.value("ms", 0)) + " von=" + rq.clientIp);
        rs.status = 204; return rs;
    }
    if (rq.method == "POST" && rq.path == "/qos") {
        bcQosReport(rq.clientIp, json::parse(rq.body, nullptr, false));
        rs.status = 204; return rs;
    }
    rs.status = 404; rs.body = "not found";
    return rs;
}

// --- Modul: Broadcast-Orchestrierung (nativer Weg; Logik 1:1 aus main.js) ------------
// Die Shell streamt AUSSCHLIESSLICH ueber den nativen C++-Helfer (der FFmpeg-Weg
// stirbt mit Electron). Prozesse: Lumora Media-Relay (mediamtx) + lumora-capture-native.
#define TIMER_VIEWER 102
#define TIMER_ADAPT  103
#define TIMER_BCAPPLY 104   // debounced: Stream-Settings-Aenderung live anwenden
// (MTX_*-Portkonstanten stehen beim Streaming-HTTP-Server weiter oben)
static const int BC_ADAPT_F[] = { 100, 70, 50, 35, 24, 16 };   // Stufen-Faktoren (25 Mbit -> 17,5/12,5/8,8/6,0/4,0)
static json g_bcState = { {"active", false} };
struct ChildProc { HANDLE proc = nullptr; HANDLE outRd = nullptr; DWORD pid = 0; bool intentional = false; };
static ChildProc g_mtx, g_cap;
static bool g_bcStopping = false;
static std::string g_bcCapKey; static int g_bcNatKbit = 0; static int g_capFastFails = 0;
static int g_adaptLevel = 0; static ULONGLONG g_adaptLastChange = 0, g_adaptBadSince = 0, g_adaptGoodSince = 0, g_adaptUpAt = 0, g_adaptUpHold = 0;
static ULONGLONG g_bcSince = 0; static int g_bcPeakViewers = 0, g_bcSwitches = 0; static std::set<std::string> g_bcSessionIds;
static std::vector<std::string> g_bcPinholeIds; static bool g_bcV4Mapped = false;   // Router-Phase (Teardown beim Stopp)
static ULONGLONG g_bcPinholeSetAt = 0;   // fuer die 12h-IPv6-Pinhole-Erneuerung (24h-Lease)
#define TIMER_IPWATCH 109   // IP-Wechsel-Watcher (5 min): Zwangstrennung/IP-Wechsel nachziehen
static void bcRegisterWatchLink(); static void bcUnregisterWatchLink();   // (Gruppen-Modul weiter unten)
static void notifyForwardIssue();   // Tray-Balloon "Portfreigabe noetig" (Definition nach dem Tray)
static bool g_bcForwardNotified = false;   // Balloon nur einmal je Stream

static void bcPushState() { sendToUi("broadcast-status", g_bcState); }
static std::wstring binDir() { return g_appDir + L"\\bin"; }
static std::wstring tempPath(const wchar_t* name) { wchar_t t[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", t, MAX_PATH); return std::wstring(t) + L"\\" + name; }
static void bcWriteBitrateControl(int kbit) { writeFile(tempPath(L"lumora-bitrate.txt"), std::to_string((std::max)(500, kbit))); }
static void bcWriteSourceControl(const json& cfg) {
    writeFile(tempPath(L"lumora-source.txt"), (cfg.value("mode", "") == "window" && cfg.value("hwnd", 0) != 0) ? ("hwnd " + std::to_string(cfg.value("hwnd", 0))) : ("monitor " + std::to_string(cfg.value("outputIdx", 0))));
}
static void bcWriteHdrControl() {
    json s = loadSettings();
    int mode = (std::max)(0, (std::min)(3, s.value("streamHdrMode", 0)));
    double exp = (std::max)(0.05, (std::min)(3.0, s.value("streamHdrExposure", 0.3937)));
    char buf[32]; sprintf_s(buf, "%d %.4f", mode, exp);
    writeFile(tempPath(L"lumora-hdr.txt"), buf);
}
static std::string bcLanIp() {
    // bevorzugte lokale IPv4 ueber einen (nie gesendeten) UDP-connect ermitteln
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "127.0.0.1";
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons(53); inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    std::string ip = "127.0.0.1";
    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in loc{}; int ll = sizeof(loc);
        if (getsockname(s, (sockaddr*)&loc, &ll) == 0) { char b[64] = {}; inet_ntop(AF_INET, &loc.sin_addr, b, sizeof(b)); ip = b; }
    }
    closesocket(s);
    return ip;
}
// Prozess ausfuehren und stdout ROH zurueckgeben (fuer --list/--hdr-check des Helfers).
static std::string runCaptureOutput(const std::wstring& cmdLine, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa) }; sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring mcmd = cmdLine; std::string out;
    if (CreateProcessW(nullptr, &mcmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr); CloseHandle(pi.hThread);
        ULONGLONG t0 = GetTickCount64();
        char buf[8192]; DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) {
            out.append(buf, got);
            if (GetTickCount64() - t0 > timeoutMs || out.size() > 16 * 1024 * 1024) break;
        }
        WaitForSingleObject(pi.hProcess, 2000);
        TerminateProcess(pi.hProcess, 0);   // haengt er noch (altes --frames-Verhalten), hart beenden
        CloseHandle(pi.hProcess);
    } else CloseHandle(wr);
    CloseHandle(rd);
    return out;
}
// Kindprozess mit stdout/stderr-Capture starten; Reader-Thread schreibt ins Stream-Log.
static bool spawnChild(ChildProc& cp, const std::wstring& cmdLine, const char* logPrefix) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa) }; sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring mcmd = cmdLine;
    if (!CreateProcessW(nullptr, &mcmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); return false;
    }
    CloseHandle(wr); CloseHandle(pi.hThread);
    cp.proc = pi.hProcess; cp.outRd = rd; cp.pid = pi.dwProcessId; cp.intentional = false;
    std::string pfx = logPrefix;
    std::thread([rd, pfx]() {   // Ausgaben zeilenweise ins Log (wie die on-data-Handler)
        std::string acc; char buf[2048]; DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) {
            acc.append(buf, got);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl); acc = acc.substr(nl + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (!line.empty()) bcLogStream(pfx + line);
            }
        }
        CloseHandle(rd);
    }).detach();
    return true;
}
static void killChild(ChildProc& cp) {
    if (!cp.proc) return;
    cp.intentional = true;
    TerminateProcess(cp.proc, 0); CloseHandle(cp.proc); cp.proc = nullptr; cp.pid = 0;
}
// mediamtx-Config (nativer Zweig 1:1: UDP/MPEG-TS-Ingest + alwaysAvailable-Ueberbrueckung).
static std::wstring bcWriteMtxConfig(const std::vector<std::string>& hosts) {
    json s = loadSettings();
    std::string y;
    y += "logLevel: error\n";
    y += "rtspAddress: 127.0.0.1:" + std::to_string(MTX_RTSP_PORT) + "\n";
    y += "rtspTransports: [udp, tcp]\n";
    y += "rtpAddress: 127.0.0.1:" + std::to_string(MTX_RTP_UDP) + "\nrtcpAddress: 127.0.0.1:" + std::to_string(MTX_RTCP_UDP) + "\n";
    y += "udpReadBufferSize: 26214400\n";
    y += "rtmp: no\nhls: no\nsrt: no\nplayback: no\nmetrics: no\npprof: no\n";
    y += "api: yes\napiAddress: 127.0.0.1:" + std::to_string(MTX_API_PORT) + "\n";
    y += "webrtcAddress: 127.0.0.1:" + std::to_string(MTX_WHEP_PORT) + "\n";
    y += "webrtcLocalUDPAddress: :" + std::to_string(MTX_ICE_UDP) + "\nwebrtcLocalTCPAddress: ''\nwebrtcIPsFromInterfaces: yes\n";
    y += "writeQueueSize: 2048\n";
    if (!hosts.empty()) {   // IPv6 MUSS gequotet werden
        y += "webrtcAdditionalHosts: [";
        for (size_t i = 0; i < hosts.size(); ++i) y += std::string(i ? ", " : "") + "'" + hosts[i] + "'";
        y += "]\n";
    }
    y += "webrtcHandshakeTimeout: 10s\n";
    std::string turl = s.value("streamTurnUrl", "");
    if (s.value("streamTurnEnabled", false) && !turl.empty()) {
        if (turl.rfind("turn:", 0) != 0 && turl.rfind("turns:", 0) != 0) turl = "turn:" + turl;
        y += "webrtcICEServers2:\n  - url: " + turl + "\n";
        if (!s.value("streamTurnUser", "").empty()) y += "    username: " + s.value("streamTurnUser", "") + "\n";
        if (!s.value("streamTurnPass", "").empty()) y += "    password: " + s.value("streamTurnPass", "") + "\n";
    }
    y += "paths:\n  "; y += MTX_PATH; y += ":\n";
    y += "    source: udp+mpegts://127.0.0.1:" + std::to_string(MTX_TS_UDP) + "\n";   // nativer Helfer publisht per UDP/MPEG-TS
    y += "    alwaysAvailable: yes\n    alwaysAvailableTracks:\n      - codec: H264\n      - codec: Opus\n";
    std::wstring p = tempPath(L"lumora-mediamtx.yml");
    writeFile(p, y);
    return p;
}
// Eigener nativer Relay (lumora-relay) statt mediamtx? Standard: ja, sobald das Exe da ist.
// Einstellung useLegacyRelay=true erzwingt mediamtx (Uebergangs-Fallback, ein Release lang).
static bool bcNativeRelay() {
    if (loadSettings().value("useLegacyRelay", false)) return false;
    return GetFileAttributesW((binDir() + L"\\lumora-media-relay.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
}
// ICE-Konfiguration (oeffentliche Hosts + TURN) an den nativen Relay pushen - ersetzt den
// mediamtx-YAML-Hot-Reload. Wirkt nur auf NEUE Zuschauer-Sessions, bestehende bleiben verbunden.
static void bcPushIceConfig(const std::vector<std::string>& hosts) {
    json s = loadSettings();
    json body; json ah = json::array();
    for (auto& h : hosts) ah.push_back(h);
    body["additionalHosts"] = ah;
    json servers = json::array();
    std::string turl = s.value("streamTurnUrl", "");
    if (s.value("streamTurnEnabled", false) && !turl.empty()) {
        if (turl.rfind("turn:", 0) != 0 && turl.rfind("turns:", 0) != 0) turl = "turn:" + turl;
        servers.push_back({ {"url", turl}, {"user", s.value("streamTurnUser", "")}, {"pass", s.value("streamTurnPass", "")} });
    }
    body["iceServers"] = servers;
    luart::httpPost("http://127.0.0.1:" + std::to_string(MTX_API_PORT) + "/v3/config/ice", body.dump(), 2000);
}
// Relay starten + aktiver Ready-Check (TCP-Probe auf den RTSP-Port, wie das Original).
static std::wstring g_mtxCfgPath;   // fuer den Crash-Neustart gemerkt
// Relay-Absturz mitten im Stream (nicht gewollt beendet): neu starten - der native
// Helfer publisht per UDP/MPEG-TS weiter, mediamtx zieht nach dem Neustart wieder an
// (wie main.js: mtxProc-exit -> bcStartMtx). Ohne das bleibt der Stream tot.
static void bcMtxExitWatch(HANDLE proc) {
    std::thread([proc]() {
        WaitForSingleObject(proc, INFINITE);
        bool intentional; { std::lock_guard<std::mutex> lk(g_launchMx); intentional = g_mtx.intentional || g_mtx.proc != proc; }
        if (intentional || g_bcStopping || !g_bcState.value("active", false)) return;
        { std::lock_guard<std::mutex> lk(g_launchMx); if (g_mtx.proc == proc) g_mtx.proc = nullptr; }
        bcLogStream("mtx: unerwartet beendet -> Neustart in 800ms");
        Sleep(800);
        if (!g_bcState.value("active", false) || g_bcStopping) return;
        extern bool bcStartMtx(const std::wstring&);
        bcStartMtx(g_mtxCfgPath);
    }).detach();
}
bool bcStartMtx(const std::wstring& cfgPath) {
    g_mtxCfgPath = cfgPath;
    bool native = bcNativeRelay();
    std::wstring relay = binDir() + (native ? L"\\lumora-media-relay.exe" : L"\\mediamtx.exe");
    if (GetFileAttributesW(relay.c_str()) == INVALID_FILE_ATTRIBUTES) { native = false; relay = binDir() + L"\\mediamtx.exe"; }
    // nativer Relay: keine Config-Datei (ICE kommt per Push nach dem Ready-Check)
    std::wstring cmd = native ? (L"\"" + relay + L"\"") : (L"\"" + relay + L"\" \"" + cfgPath + L"\"");
    if (!spawnChild(g_mtx, cmd, "mtx: ")) return false;
    bcMtxExitWatch(g_mtx.proc);
    // Readiness: nativ = Control-API-Port (lauscht zuletzt), mediamtx = RTSP-Port (wie das Original)
    int readyPort = native ? MTX_API_PORT : MTX_RTSP_PORT;
    for (int i = 0; i < 14; ++i) {   // ~840 ms Fallback, typisch nach ~150 ms bereit
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(readyPort); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        bool ok = connect(s, (sockaddr*)&a, sizeof(a)) == 0;
        closesocket(s);
        if (ok) {
            if (native) {   // initiale ICE-Hosts/TURN pushen (Cache; Router-Phase pusht spaeter frisch)
                std::vector<std::string> hosts;
                for (auto& h : loadSettings().value("streamLastHosts", json::array())) if (h.is_string()) hosts.push_back(h.get<std::string>());
                bcPushIceConfig(hosts);
            }
            return true;
        }
        Sleep(60);
    }
    DWORD ec = 0;   // nie bereit geworden: lebt der Prozess ueberhaupt noch? (Port belegt etc.)
    return g_mtx.proc && GetExitCodeProcess(g_mtx.proc, &ec) && ec == STILL_ACTIVE;
}
// Stream-Konfiguration aus den Einstellungen (bcStreamCfg 1:1; native Monitorhoehe via Win32).
static json bcStreamCfg(const std::string& encoder) {
    json s = loadSettings();
    std::string q = s.value("streamQuality", "1080");
    int scaleH = q == "720" ? 720 : q == "1080" ? 1080 : q == "1440" ? 1440 : q == "2160" ? 0 : 0;   // 4K = nativ
    int base = (std::max)(1000, s.value("streamUploadKbit", 8000));
    int kbit = base;
    if (g_adaptLevel > 0 && s.value("streamAdaptive", true)) kbit = (std::max)(3000, (int)(base * BC_ADAPT_F[g_adaptLevel] / 100.0 / 100.0 + 0.5) * 100);
    std::string src = s.value("streamSource", "");
    std::string mode = "monitor"; int outputIdx = 0; long long hwnd = 0;
    if (src.rfind("window:", 0) == 0) { hwnd = atoll(src.c_str() + 7); if (hwnd) mode = "window"; }
    else if (src.rfind("screen:", 0) == 0) outputIdx = atoi(src.c_str() + 7);
    if (mode == "monitor" && scaleH) {   // Preset >= native Hoehe des GEWAEHLTEN Bildschirms -> nicht skalieren
        struct MP { int want, cur, h; } mp{ outputIdx, 0, 0 };
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* p = (MP*)lp; if (p->cur++ == p->want) { MONITORINFO mi{ sizeof(mi) }; if (GetMonitorInfoW(hm, &mi)) p->h = mi.rcMonitor.bottom - mi.rcMonitor.top; return FALSE; } return TRUE;
        }, (LPARAM)&mp);
        if (!mp.h) { HMONITOR hm = MonitorFromPoint({ 0,0 }, MONITOR_DEFAULTTOPRIMARY); MONITORINFO mi{ sizeof(mi) }; if (GetMonitorInfoW(hm, &mi)) mp.h = mi.rcMonitor.bottom - mi.rcMonitor.top; }
        if (mp.h && scaleH >= mp.h) scaleH = 0;
    }
    return { {"encoder", encoder}, {"fps", s.value("streamFps", 60)}, {"kbit", kbit}, {"scaleH", scaleH},
             {"mode", mode}, {"outputIdx", outputIdx}, {"hwnd", hwnd} };
}
static std::string bcQualityLabel(const json& cfg) {
    std::string h = cfg.value("scaleH", 0) ? std::to_string(cfg.value("scaleH", 0)) + "p" : "nativ";
    std::string enc = cfg.value("encoder", ""); size_t us = enc.find('_'); if (us != std::string::npos) enc = enc.substr(us + 1);
    for (auto& c : enc) c = (char)toupper((unsigned char)c);
    return h + " \xC2\xB7 " + std::to_string(cfg.value("fps", 60)) + " fps \xC2\xB7 " + std::to_string((int)(cfg.value("kbit", 8000) / 1000)) + " Mbit \xC2\xB7 " + enc;
}
// Nativen Capture-Helfer starten (Args + Steuerdateien + Exit-Watch mit Auto-Neustart).
static void bcStartNative(const json& cfg);
static void bcNativeExitWatch(HANDLE proc, ULONGLONG startedAt) {
    std::thread([proc, startedAt]() {
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 0; GetExitCodeProcess(proc, &code);
        bool intentional; { std::lock_guard<std::mutex> lk(g_launchMx); intentional = g_cap.intentional || g_cap.proc != proc; }
        CloseHandle(proc);
        if (intentional || g_bcStopping || !g_bcState.value("active", false)) return;
        { std::lock_guard<std::mutex> lk(g_launchMx); if (g_cap.proc == proc) g_cap.proc = nullptr; }
        if (GetTickCount64() - startedAt < 4000) g_capFastFails++; else g_capFastFails = 0;
        int delay = g_capFastFails > 3 ? 4000 : 500;
        bcLogStream("nat beendet (" + std::to_string(code) + ") -> Neustart in " + std::to_string(delay) + "ms");
        Sleep(delay);
        if (g_bcState.value("active", false) && !g_bcStopping)
            bcStartNative(bcStreamCfg(g_bcState.value("encoder", "")));
    }).detach();
}
static void bcStartNative(const json& cfg) {
    static const std::map<std::string, std::wstring> encMap = { {"h264_nvenc", L"nvenc"}, {"h264_amf", L"amf"}, {"h264_qsv", L"qsv"} };
    auto em = encMap.find(cfg.value("encoder", ""));
    std::wstring args = L"--encoder " + (em != encMap.end() ? em->second : L"auto") +
        L" --fps " + std::to_wstring(cfg.value("fps", 60)) +
        L" --bitrate " + std::to_wstring((std::max)(1, (int)(cfg.value("kbit", 8000) / 1000.0 + 0.5))) +
        L" --mtx-host 127.0.0.1 --mtx-port " + std::to_wstring(MTX_TS_UDP) + L" --audio";
    if (cfg.value("scaleH", 0)) args += L" --scale " + std::to_wstring(cfg.value("scaleH", 0));
    if (cfg.value("mode", "") == "window" && cfg.value("hwnd", 0ll)) args += L" --window --hwnd " + std::to_wstring(cfg.value("hwnd", 0ll));
    else args += L" --monitor " + std::to_wstring(cfg.value("outputIdx", 0));   // gewaehlter Bildschirm
    bcWriteHdrControl();
    g_bcCapKey = std::to_string(cfg.value("mode", "") == "window" ? cfg.value("hwnd", 0ll) : 0) + "|" + std::to_string(cfg.value("fps", 60)) + "|" + std::to_string(cfg.value("scaleH", 0));
    bcWriteBitrateControl(cfg.value("kbit", 8000));
    g_bcNatKbit = cfg.value("kbit", 8000);
    bcWriteSourceControl(cfg);
    ChildProc cp;
    if (!spawnChild(cp, L"\"" + binDir() + L"\\lumora-capture-native.exe\" " + args, "nat: ")) { bcLogStream("nat: Start fehlgeschlagen"); return; }
    { std::lock_guard<std::mutex> lk(g_launchMx); g_cap = cp; }
    bcNativeExitWatch(cp.proc, GetTickCount64());
}
// mediamtx-Sessions: echte Zuschauer (state=read, keine 127.x-Vorschau) aus der API.
static json bcMtxReaders() {
    auto r = luart::httpGet("http://127.0.0.1:" + std::to_string(MTX_API_PORT) + "/v3/webrtcsessions/list");
    json out = json::array();
    if (r.status != 200) return out;
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return out;
    for (auto& s : j.value("items", json::array())) {
        if (s.value("path", "") != MTX_PATH || s.value("state", "") != "read") continue;
        std::string rc = s.value("remoteCandidate", "");
        if (rc.find("127.0.0.1") != std::string::npos || rc.find("::1") != std::string::npos) continue;   // eigene Vorschau
        if (s.value("userAgent", "").find("LumoraPreview") != std::string::npos) continue;                // markierte Vorschau-Session
        out.push_back(s);
    }
    return out;
}
// mediamtx-ICE-Kandidat ist SLASH-getrennt "<typ>/<proto>/<ip>/<port>" -> IP = vorletztes
// Segment (1:1 aus main.js bcExtractIp). Fallback: nackte IPv4 per Regex. Der fruehere
// ip:port-Parser lieferte die falsche Zuschauer-IP + machte die IP-Sperre unwirksam.
static std::string bcExtractIp(const std::string& cand) {
    if (cand.empty()) return "";
    std::vector<std::string> parts; size_t s = 0, c;
    while ((c = cand.find('/', s)) != std::string::npos) { parts.push_back(cand.substr(s, c - s)); s = c + 1; }
    parts.push_back(cand.substr(s));
    if (parts.size() >= 4 && !parts[parts.size() - 2].empty()) return parts[parts.size() - 2];
    std::smatch m;
    if (std::regex_search(cand, m, std::regex("(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3})"))) return m[1];
    return "";
}
// User-Agent auf "Browser · OS" kuerzen (1:1 aus main.js bcUaShort) - statt des
// rohen langen UA-Strings in der Zuschauerliste.
static std::string bcUaShort(const std::string& ua) {
    if (ua.empty()) return "";
    auto has = [&](const char* n) { return ua.find(n) != std::string::npos; };
    std::string br = has("Edg/") ? "Edge" : (has("OPR/") || has("Opera")) ? "Opera"
        : has("Firefox/") ? "Firefox" : has("Chrome/") ? "Chrome" : has("Safari/") ? "Safari" : "Browser";
    std::string os = has("Android") ? "Android" : (has("iPhone") || has("iPad") || has("iPod")) ? "iOS"
        : has("Windows") ? "Windows" : has("Mac OS X") ? "macOS" : has("Linux") ? "Linux" : "";
    return os.empty() ? br : br + " · " + os;
}
static void bcViewerTick() {
    if (!g_bcState.value("active", false)) return;
    json readers = bcMtxReaders();
    int n = (int)readers.size();
    if (n > g_bcPeakViewers) g_bcPeakViewers = n;
    for (auto& s : readers) if (s.contains("id")) g_bcSessionIds.insert(s.value("id", ""));
    int prev = g_bcState.value("viewers", 0);
    if (n != prev) {
        g_bcState["viewers"] = n;
        bcLogStream("viewer: " + std::to_string(prev) + " -> " + std::to_string(n));
        bcPushState();
        if (n > prev) sendToUi("viewer-joined", n);
    }
}
// Live-Umstellung (bcDoRestartFfmpeg-Kern): Bitrate + Quelle ueber die Steuerdateien,
// Vollneustart des Helfers nur bei fps/scaleH-Aenderung.
static void bcApplyCfg(const json& cfg) {
    std::string hardNew = std::to_string(cfg.value("fps", 60)) + "|" + std::to_string(cfg.value("scaleH", 0));
    size_t p1 = g_bcCapKey.find('|');
    std::string hardOld = p1 == std::string::npos ? "" : g_bcCapKey.substr(p1 + 1);
    bool capAlive; { std::lock_guard<std::mutex> lk(g_launchMx); capAlive = g_cap.proc != nullptr; }
    if (capAlive && hardOld == hardNew) {
        std::string newHwnd = std::to_string(cfg.value("mode", "") == "window" ? cfg.value("hwnd", 0ll) : 0);
        std::string oldHwnd = g_bcCapKey.substr(0, p1);
        if (newHwnd != oldHwnd) {
            bcWriteSourceControl(cfg);
            g_bcCapKey = newHwnd + "|" + hardNew;
            bcLogStream("nat: Quelle live -> " + (newHwnd == "0" ? "Monitor" : "Fenster " + newHwnd) + " (kein Neustart)");
        }
        bcWriteBitrateControl(cfg.value("kbit", 8000));
        if (cfg.value("kbit", 8000) != g_bcNatKbit) {
            g_bcNatKbit = cfg.value("kbit", 8000);
            bcBroadcastSwitch("bitrate", g_bcNatKbit);
            bcLogStream("nat: Bitrate live -> " + std::to_string(g_bcNatKbit) + " kbit (kein Neustart)");
        }
        g_bcState["quality"] = bcQualityLabel(cfg);
        bcPushState();
        return;
    }
    // Vollneustart (fps/scaleH geaendert): Zuschauer einfrieren, Helfer neu.
    g_bcSwitches++;
    bcBroadcastSwitch("full", cfg.value("kbit", 8000));
    { std::lock_guard<std::mutex> lk(g_launchMx); killChild(g_cap); }
    bcStartNative(cfg);
    g_bcState["quality"] = bcQualityLabel(cfg);
    bcPushState();
}
// Adaptive Bitrate (bcAdaptTick 1:1: Notabstieg >=12%, runter nach 8s schlecht,
// rauf nach 90s sauber, 20s-Ruhe, gescheiterte Rauf-Probe = 10 min Sperre).
static void bcAdaptTick() {
    if (!g_bcState.value("active", false)) return;
    json s = loadSettings();
    if (!s.value("streamAdaptive", true)) return;
    ULONGLONG now = GetTickCount64();
    bool bad = false; double worstLoss = 0;
    {
        std::lock_guard<std::mutex> lk(g_qosMx);
        for (auto it = g_qosMap.begin(); it != g_qosMap.end();) {
            if (now - it->second.t > 15000) { it = g_qosMap.erase(it); continue; }
            if (it->second.badStreak >= 2) bad = true;
            if (now - it->second.t <= 8000 && it->second.lossRate > worstLoss) worstLoss = it->second.lossRate;
            ++it;
        }
    }
    const int maxLevel = (int)(sizeof(BC_ADAPT_F) / sizeof(int)) - 1;
    int base = (std::max)(1000, s.value("streamUploadKbit", 8000));
    auto lvKbit = [&](int lv) { return lv <= 0 ? base : (std::max)(3000, (int)(base * BC_ADAPT_F[lv] / 100.0 / 100.0 + 0.5) * 100); };
    if (worstLoss >= 0.12 && g_adaptLevel < maxLevel && now - g_adaptLastChange >= 6000) {   // NOTABSTIEG
        int target = (std::min)(maxLevel, g_adaptLevel + (worstLoss >= 0.30 ? 3 : 2));
        if (lvKbit(target) < lvKbit(g_adaptLevel)) {
            g_adaptLevel = target; g_adaptLastChange = now; g_adaptBadSince = 0; g_adaptGoodSince = 0;
            { std::lock_guard<std::mutex> lk(g_qosMx); g_qosMap.clear(); }
            bcLogStream("adapt: NOTABSTIEG bei " + std::to_string((int)(worstLoss * 100)) + "% Verlust -> Stufe " + std::to_string(target) + " -> " + std::to_string(lvKbit(target)) + " kbit");
            bcApplyCfg(bcStreamCfg(g_bcState.value("encoder", "")));
        }
        return;
    }
    if (bad) { if (!g_adaptBadSince) g_adaptBadSince = now; g_adaptGoodSince = 0; }
    else { g_adaptBadSince = 0; if (!g_adaptGoodSince) g_adaptGoodSince = now; }
    int next = g_adaptLevel;
    if (bad && g_adaptBadSince && now - g_adaptBadSince >= 8000 && g_adaptLevel < maxLevel) next = g_adaptLevel + 1;
    else if (!bad && g_adaptGoodSince && now - g_adaptGoodSince >= 90000 && g_adaptLevel > 0 && now >= g_adaptUpHold) next = g_adaptLevel - 1;
    if (next == g_adaptLevel || now - g_adaptLastChange < 20000) return;
    bool goingDown = next > g_adaptLevel;
    if (goingDown && g_adaptUpAt && now - g_adaptUpAt < 60000) {   // Rauf-Probe gescheitert
        g_adaptUpHold = now + 600000;
        bcLogStream("adapt: Rauf-Probe gescheitert -> naechster Versuch in 10 min");
    }
    if (!goingDown) g_adaptUpAt = now;
    g_adaptLevel = next; g_adaptLastChange = now;
    bcLogStream("adapt: Stufe " + std::to_string(next) + " -> " + std::to_string(lvKbit(next)) + " kbit");
    bcApplyCfg(bcStreamCfg(g_bcState.value("encoder", "")));
    g_bcState["adaptKbit"] = next > 0 ? lvKbit(next) : 0;
    bcPushState();
}
// Encoder-Vendor fuer die Shell (immer nativer Helfer; ohne ffmpeg-Abfrage).
static std::string bcShellEncoder() {
    json s = loadSettings();
    std::string ov = s.value("streamEncoder", "auto");
    if (ov == "nvenc" || ov == "amf" || ov == "qsv") return "h264_" + ov;
    // Vendor via DXGI (wie der Helfer selbst: NVIDIA > AMD > Intel)
    ComPtr<IDXGIFactory1> fac; CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    bool nv = false, amd = false, intel = false;
    if (fac) for (UINT i = 0;; ++i) { ComPtr<IDXGIAdapter1> a; if (fac->EnumAdapters1(i, &a) != S_OK) break; DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d); if (d.VendorId == 0x10DE) nv = true; else if (d.VendorId == 0x1002) amd = true; else if (d.VendorId == 0x8086) intel = true; }
    return nv ? "h264_nvenc" : amd ? "h264_amf" : intel ? "h264_qsv" : "h264_nvenc";
}
static json startBroadcast() {
    if (g_bcState.value("active", false)) return g_bcState;
    g_bcStopping = false; g_adaptLevel = 0; g_adaptLastChange = 0; g_adaptBadSince = 0; g_adaptGoodSince = 0; g_adaptUpAt = 0; g_adaptUpHold = 0;
    { std::lock_guard<std::mutex> lk(g_qosMx); g_qosMap.clear(); }
    g_bcSince = GetTickCount64(); g_bcPeakViewers = 0; g_bcSwitches = 0; g_bcSessionIds.clear(); g_bcForwardNotified = false;
    std::string enc = bcShellEncoder();
    std::string lanLink = "http://" + bcLanIp() + ":" + std::to_string(BROADCAST_PORT) + "/";
    g_bcState = { {"active", true}, {"port", BROADCAST_PORT}, {"link", lanLink}, {"linkV4", ""}, {"linkV6", ""},
                  {"lanLink", lanLink}, {"viewers", 0}, {"quality", ""}, {"internet", false}, {"opening", true},
                  {"encoder", enc}, {"since", (long long)time(nullptr) * 1000} };
    bcPushState();
    // PHASE 1 - LOKAL, SOFORT (gecachte oeffentliche Hosts der letzten Session als ICE-Hosts)
    json s = loadSettings();
    std::vector<std::string> hosts;
    for (auto& h : s.value("streamLastHosts", json::array())) if (h.is_string()) hosts.push_back(h.get<std::string>());
    std::wstring cfgPath = bcWriteMtxConfig(hosts);
    if (!bcStartMtx(cfgPath)) {
        bcLogStream("start: FEHLGESCHLAGEN (Relay startet nicht - Port belegt? Electron-Lumora aktiv?)");
        sendToUi("stream-error", "Stream-Start fehlgeschlagen: Relay startet nicht. Laeuft Lumora doppelt (Electron-Version streamt)?");
        g_bcState = { {"active", false} }; bcPushState();
        { std::lock_guard<std::mutex> lk(g_launchMx); killChild(g_mtx); }
        return g_bcState;
    }
    if (!g_streamSrvUp) g_streamSrvUp = g_streamSrv.start(BROADCAST_PORT, handleStreamHttp);
    if (!g_streamSrvUp) {
        sendToUi("stream-error", "Stream-Start fehlgeschlagen: Port 8787 wird von einem anderen Programm belegt.");
        { std::lock_guard<std::mutex> lk(g_launchMx); killChild(g_mtx); }
        g_bcState = { {"active", false} }; bcPushState();
        return g_bcState;
    }
    json cfg = bcStreamCfg(enc);
    bcStartNative(cfg);
    g_bcState["quality"] = bcQualityLabel(cfg);
    bcPushState();   // Medien laufen, Vorschau kann verbinden (opening bleibt true bis Router-Phase durch)
    SetTimer(g_hwnd, TIMER_VIEWER, 2000, nullptr);
    SetTimer(g_hwnd, TIMER_ADAPT, 5000, nullptr);
    SetTimer(g_hwnd, TIMER_IPWATCH, 300000, nullptr);   // 5min: IP-Wechsel/Pinhole-Erneuerung
    bcLogStream("start: nativ " + enc + " " + bcQualityLabel(cfg) + " " + lanLink);
    // PHASE 2 - ROUTER, PARALLEL (1:1 aus main.js): oeffentliche IPs + Portfreigaben +
    // IPv6-Pinholes; erst danach steht der oeffentliche Link fest -> opening=false.
    std::thread([hosts]() {
        ULONGLONG t0 = GetTickCount64();
        json s = loadSettings();
        bool forceV6 = s.value("streamForceIPv6", false);
        std::string pubIp = luupnp::getExternalIp();
        std::string pubIp6 = luupnp::publicIPv6();
        bool tcpOk = false, udpOk = false, v6Ok = false, v4Occupied = false;
        if (!pubIp6.empty()) {   // Pinholes (TCP-Signalisierung + UDP-Medien)
            std::string t6 = luupnp::addPinhole(pubIp6, BROADCAST_PORT, "TCP");
            std::string u6 = luupnp::addPinhole(pubIp6, MTX_ICE_UDP, "UDP");
            if (!t6.empty()) g_bcPinholeIds.push_back(t6);
            if (!u6.empty()) g_bcPinholeIds.push_back(u6);
            v6Ok = !t6.empty() && !u6.empty();
            if (v6Ok) g_bcPinholeSetAt = GetTickCount64();   // Startzeit fuer die 12h-Erneuerung
        }
        if (!forceV6 && !pubIp.empty()) {
            // Zweiter PC im selben Netz: antwortet unter der oeffentlichen IPv4 schon eine
            // ANDERE Lumora-Instanz, deren Mapping NICHT stehlen (Hairpin-Check, 1.5s wie Electron).
            auto r = luart::httpGet("http://" + pubIp + ":" + std::to_string(BROADCAST_PORT) + "/instanz", L"", 1500);
            json j = json::parse(r.body, nullptr, false);
            if (r.status == 200 && j.is_object() && j.value("lumora", false) && !j.value("id", json()).is_null()) v4Occupied = true;   // fremde (Electron-)Instanz mit Identitaet
        }
        if (!v4Occupied && !forceV6) {
            tcpOk = luupnp::mapPort(BROADCAST_PORT, "TCP", "Lumora Stream");
            udpOk = luupnp::mapPort(MTX_ICE_UDP, "UDP", "Lumora Stream Medien");
            if (tcpOk || udpOk) g_bcV4Mapped = true;
        }
        if (!g_bcState.value("active", false)) {   // waehrend der Router-Phase gestoppt
            for (auto& id : g_bcPinholeIds) luupnp::deletePinhole(id);
            g_bcPinholeIds.clear();
            return;
        }
        // Frische Hosts vom Cache abweichend? Config aktualisieren (mediamtx laedt selbst neu) + Cache speichern.
        std::vector<std::string> fresh;
        if (!forceV6 && !pubIp.empty()) fresh.push_back(pubIp);
        if (!pubIp6.empty()) fresh.push_back(pubIp6);
        if (fresh != hosts) {
            bcWriteMtxConfig(fresh);       // mediamtx: Hot-Reload der YAML
            bcPushIceConfig(fresh);        // nativer Relay: Push (bei mediamtx wirkungslos, API kennt den Pfad nicht)
            json s2 = loadSettings(); json arr = json::array(); for (auto& h : fresh) arr.push_back(h);
            s2["streamLastHosts"] = arr; writeFile(settingsPath(), s2.dump(2));
        }
        bcLogStream("router-phase: " + std::to_string(GetTickCount64() - t0) + " ms (v4=" + ((tcpOk && udpOk) ? "ok" : "nein") + " v6=" + (v6Ok ? "ok" : "nein") + (v4Occupied ? ", IPv4 belegt von anderer Instanz" : "") + ")");
        bool v4Reachable = tcpOk && udpOk && !pubIp.empty();
        std::string l4 = v4Reachable ? ("http://" + pubIp + ":" + std::to_string(BROADCAST_PORT) + "/") : "";
        std::string l6 = (v6Ok && !pubIp6.empty()) ? ("http://[" + pubIp6 + "]:" + std::to_string(BROADCAST_PORT) + "/") : "";
        if (v4Reachable) g_bcState["link"] = l4;                       // Link-Prioritaet wie das Original
        else if (!l6.empty()) g_bcState["link"] = l6;
        else if (!pubIp.empty()) g_bcState["link"] = "http://" + pubIp + ":" + std::to_string(BROADCAST_PORT) + "/";
        g_bcState["linkV4"] = l4; g_bcState["linkV6"] = l6;
        g_bcState["internet"] = v4Reachable || !l6.empty();
        g_bcState["ipv6Only"] = !v4Reachable && !l6.empty();
        g_bcState["needsForward"] = !pubIp.empty() && !v4Reachable && l6.empty();
        if (g_bcState["needsForward"].get<bool>()) {   // herstellerspezifische Portfreigabe-Anleitung + Toast
            lurouter::RouterHint hint = lurouter::routerUpnpHint(luupnp::routerName(), true /*de*/, BROADCAST_PORT, MTX_ICE_UDP);
            g_bcState["forwardHint"] = { {"router", hint.router}, {"steps", hint.steps} };
            notifyForwardIssue();
        } else g_bcState["forwardHint"] = nullptr;
        g_bcState["opening"] = false;
        bcPushState();
        if (g_bcState.value("internet", false)) bcRegisterWatchLink();   // schoene stream.php-Teilen-URL statt IP
    }).detach();
    return g_bcState;
}
static json stopBroadcast() {
    bool wasActive = g_bcState.value("active", false);
    if (wasActive && GetTickCount64() - g_bcSince >= 10000) {   // Abschluss-Statistik
        json s = loadSettings();
        // Ingest-Bytes vom Relay (bevor er unten beendet wird): mediamtx UND nativer Relay
        // liefern "bytesReceived" unter /v3/paths/get/<pfad>. Encoder-seitige Datenmenge,
        // nicht Zuschauer-Summe - ehrlicher Wert unabhaengig von der Zuschauerzahl.
        long long bytes = 0;
        {
            auto r = luart::httpGet("http://127.0.0.1:" + std::to_string(MTX_API_PORT) + "/v3/paths/get/" + MTX_PATH, L"", 1500);
            json j = json::parse(r.body, nullptr, false);
            if (r.status == 200 && j.is_object()) bytes = j.value("bytesReceived", 0ll);
        }
        long long durMs = (long long)(GetTickCount64() - g_bcSince);
        double avgMbit = durMs > 0 ? (double)bytes * 8.0 / ((double)durMs / 1000.0) / 1e6 : 0.0;
        avgMbit = std::round(avgMbit * 10.0) / 10.0;   // UI zeigt den Wert unformatiert an
        sendToUi("stream-summary", { {"durMs", durMs}, {"peakViewers", g_bcPeakViewers},
            {"totalViewers", (int)g_bcSessionIds.size()}, {"bytes", bytes}, {"avgMbit", avgMbit}, {"switches", g_bcSwitches},
            {"quality", s.value("streamQuality", "auto")}, {"fps", s.value("streamFps", 60)}, {"kbit", s.value("streamUploadKbit", 8000)} });
    }
    g_bcStopping = true;
    bcUnregisterWatchLink();   // Teilen-URL abmelden (5-Min-Karenz laeuft)
    {   // Freigegebene Zuschauer in die 15-Min-Karenz uebernehmen (Neustart fragt nicht erneut)
        std::lock_guard<std::mutex> lk(g_doorMx);
        for (auto& [vid, k] : g_knocks) if (k.status == "granted") g_prevGrants[vid] = GetTickCount64();
        g_knocks.clear();
    }
    KillTimer(g_hwnd, TIMER_VIEWER); KillTimer(g_hwnd, TIMER_ADAPT); KillTimer(g_hwnd, TIMER_IPWATCH);
    { std::lock_guard<std::mutex> lk(g_launchMx); killChild(g_cap); killChild(g_mtx); }
    // Router-Teardown im Hintergrund (SOAP darf den Stopp nicht verzoegern)
    if (g_bcV4Mapped || !g_bcPinholeIds.empty()) {
        std::vector<std::string> pins = g_bcPinholeIds; g_bcPinholeIds.clear();
        bool unmap = g_bcV4Mapped; g_bcV4Mapped = false;
        std::thread([pins, unmap]() {
            if (unmap) { luupnp::unmapPort(BROADCAST_PORT, "TCP"); luupnp::unmapPort(MTX_ICE_UDP, "UDP"); }
            for (auto& id : pins) luupnp::deletePinhole(id);
        }).detach();
    }
    if (g_streamSrvUp) { g_streamSrv.stop(); g_streamSrvUp = false; }
    g_bcState = { {"active", false}, {"port", BROADCAST_PORT}, {"link", ""}, {"linkV4", ""}, {"linkV6", ""},
                  {"lanLink", ""}, {"viewers", 0}, {"quality", ""}, {"internet", false}, {"opening", false} };
    bcPushState();
    bcLogStream("stop: Stream beendet");
    return g_bcState;
}

// --- Modul: Gruppe + Vermittlung + Teilen-URL (1:1 aus main.js) ------------------
#define TIMER_GROUP 105   // 8s-Anwesenheits-Heartbeat
#define TIMER_WATCH 106   // 20s-Refresh der Einzelstream-Registrierung
static const char* GROUP_RELAY_DEFAULT = "https://lumora-streaming.de/gruppe.php";
static json g_group = nullptr;                       // {code, members, relayFails} oder null
static std::string g_accessKey, g_watchCode, g_prevWatchCode;
static ULONGLONG g_prevWatchAt = 0;
static std::atomic<bool> g_groupTickBusy{ false };

static std::string randHex(int bytes) {
    static std::random_device rd; std::string o; char b[3];
    for (int i = 0; i < bytes; ++i) { sprintf_s(b, "%02x", (unsigned)(rd() & 0xFF)); o += b; }
    return o;
}
static std::string groupRelayUrl() {
    std::string u = loadSettings().value("groupRelayUrl", "");
    while (!u.empty() && (u.back() == ' ')) u.pop_back();
    return u.empty() ? GROUP_RELAY_DEFAULT : u;
}
static std::string streamShareUrl() {
    std::string u = groupRelayUrl();
    size_t p = u.rfind("gruppe.php");
    return p != std::string::npos ? u.substr(0, p) + "stream.php" : u;
}
static std::string groupMemberId() {
    json s = loadSettings();
    std::string id = s.value("groupMemberId", "");
    if (id.empty()) { id = randHex(6); s["groupMemberId"] = id; writeFile(settingsPath(), s.dump(2)); }
    return id;
}
static std::string groupDisplayName() {
    wchar_t name[64] = {}; DWORD n = 64;
    if (GetUserNameW(name, &n) && name[0]) { std::string s = narrow(name); return s.substr(0, 24); }
    return "Spieler";
}
// Anfrage an die Vermittlung (GET ohne / POST mit Body); niemals werfen, null bei Fehler.
static json groupRelay(const std::string& action, const json& params, const json* bodyObj) {
    std::string url = groupRelayUrl() + "?a=" + action;
    if (params.is_object()) for (auto& [k, v] : params.items()) url += "&" + k + "=" + luart::urlEnc(v.is_string() ? v.get<std::string>() : v.dump());
    luart::HttpResp r = bodyObj ? luart::httpPost(url, bodyObj->dump(), 8000) : luart::httpGet(url, L"", 8000);
    json j = json::parse(r.body, nullptr, false);
    return j.is_object() ? j : json(nullptr);
}
static json groupSelfEntry() {
    if (g_accessKey.empty()) g_accessKey = randHex(16);   // Tuersteher-Schluessel je Session
    return { {"id", groupMemberId()}, {"name", groupDisplayName()},
             {"linkV4", g_bcState.value("linkV4", "").empty() ? json(nullptr) : json(g_bcState.value("linkV4", ""))},
             {"linkV6", g_bcState.value("linkV6", "").empty() ? json(nullptr) : json(g_bcState.value("linkV6", ""))},
             {"streaming", g_bcState.value("active", false)}, {"vk", g_accessKey} };
}
static json groupPublicState() {
    if (g_group.is_null()) return { {"active", false}, {"lastCode", loadSettings().value("groupLastCode", "")} };
    json members = json::array();
    for (auto& m : g_group.value("members", json::array()))
        members.push_back({ {"id", m.value("id", "")}, {"name", m.value("name", "")},
                            {"isSelf", m.value("id", "") == groupMemberId()}, {"streaming", m.value("streaming", true)} });
    return { {"active", true}, {"code", g_group.value("code", "")},
             {"link", groupRelayUrl() + "?code=" + g_group.value("code", "")},
             {"members", members}, {"relayUnreachable", g_group.value("relayFails", 0) > 0} };
}
static void groupPushState() { sendToUi("group-status", groupPublicState()); }
static std::string bcShareUrl() {
    if (!g_group.is_null()) return groupRelayUrl() + "?code=" + g_group.value("code", "");
    if (!g_watchCode.empty()) return streamShareUrl() + "?s=" + g_watchCode;
    return "";
}
static void groupTickOnce() {
    if (g_group.is_null()) return;
    json self = groupSelfEntry();
    json r = groupRelay("update", { {"code", g_group.value("code", "")} }, &self);
    if (r.is_object() && r.value("ok", false)) {
        g_group["relayFails"] = 0;
        g_group["members"] = r.value("members", json::array());
        groupPushState();
        return;
    }
    if (r.is_object() && r.value("error", "") == "no-room") {   // TTL abgelaufen -> lokal beenden
        KillTimer(g_hwnd, TIMER_GROUP); lulan::beaconStop();   // LAN-Beacon einstellen (weiter lauschen)
        g_group = nullptr;
        groupPushState();
        return;
    }
    g_group["relayFails"] = g_group.value("relayFails", 0) + 1;
    groupPushState();
}
// Teilen-URL (stream.php?s=CODE) registrieren - Logik 1:1 inkl. 5-Min-Code-Karenz.
static void bcRegisterWatchLink() {
    if (!g_watchCode.empty()) { std::string su = bcShareUrl(); if (!su.empty()) { g_bcState["link"] = su; bcPushState(); } return; }
    if (g_bcState.value("linkV4", "").empty() && g_bcState.value("linkV6", "").empty()) return;   // nur LAN
    json params = json::object();
    if (!g_prevWatchCode.empty() && GetTickCount64() - g_prevWatchAt < 300000) params["want"] = g_prevWatchCode;
    json empty = json::object();
    json c = groupRelay("create", params, &empty);
    if (!c.is_object() || !c.value("ok", false) || c.value("code", "").empty()) { bcLogStream("watch-link: Vermittlung nicht erreichbar -> IP-Link bleibt"); return; }
    json self = groupSelfEntry();
    json r = groupRelay("update", { {"code", c.value("code", "")} }, &self);
    if (!r.is_object() || !r.value("ok", false)) { bcLogStream("watch-link: Registrierung fehlgeschlagen -> IP-Link bleibt"); return; }
    if (!g_bcState.value("active", false)) { json leave = { {"id", groupMemberId()} }; groupRelay("leave", { {"code", c.value("code", "")} }, &leave); return; }
    g_watchCode = c.value("code", "");
    g_bcState["link"] = streamShareUrl() + "?s=" + g_watchCode;
    bcLogStream("watch-link: " + g_bcState.value("link", ""));
    SetTimer(g_hwnd, TIMER_WATCH, 20000, nullptr);
    bcPushState();
}
// /instanz-Identitaet + Tuersteher-Schluessel (Vorwaertsdeklarationen; nicht-static wegen extern)
std::string instanzId() { return groupMemberId(); }
std::string instanzGroup() { return g_group.is_null() ? "" : g_group.value("code", ""); }
std::string doormanAccessKey() { if (g_accessKey.empty() && g_bcState.value("active", false)) g_accessKey = randHex(16); return g_accessKey; }
static void bcUnregisterWatchLink() {
    g_accessKey.clear();   // naechster Stream bekommt einen frischen Tuersteher-Schluessel
    KillTimer(g_hwnd, TIMER_WATCH);
    if (!g_watchCode.empty()) {
        g_prevWatchCode = g_watchCode; g_prevWatchAt = GetTickCount64();   // 5-Min-Karenz
        std::string code = g_watchCode; g_watchCode.clear();
        std::thread([code]() { json leave = { {"id", groupMemberId()} }; groupRelay("leave", { {"code", code} }, &leave); }).detach();
    }
}

// --- Modul: Tray + Hotkeys + Autostart + Deep-Link (1:1-Verhalten aus main.js) ---
#define WM_SHELL_TRAY (WM_APP + 2)
#define HK_TOGGLE 1
#define HK_STREAM 2
#define HK_OSD 3
#define HK_OSDEDIT 5
#define HK_OSDAB 6
static NOTIFYICONDATAW g_nid{};
static bool g_trayOn = false, g_quitting = false;

// Fenster, dem wir beim Hochholen den Fokus genommen haben (i.d.R. das Spiel) -
// beim Verstecken geben wir ihn exakt dorthin zurueck (1:1 aus main.js).
static HWND g_prevGameHwnd = nullptr;
static HWND g_osdHwnd = nullptr;   // OSD-Overlay-Fenster (nicht als "Vorgaenger" merken)
static void restoreGameFocus() {
    if (!g_prevGameHwnd) return;
    if (!IsWindow(g_prevGameHwnd)) { g_prevGameHwnd = nullptr; return; }   // Spiel inzwischen beendet
    if (IsIconic(g_prevGameHwnd)) ShowWindow(g_prevGameHwnd, SW_RESTORE);   // Vollbild kickte sich beim Fokusverlust
    SetForegroundWindow(g_prevGameHwnd);
    g_prevGameHwnd = nullptr;
}
static void showMainWindow() {
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_hwnd && fg != g_osdHwnd) g_prevGameHwnd = fg;   // merken, wem wir den Fokus nehmen
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE); else ShowWindow(g_hwnd, SW_SHOW);
    // Kurzes TOPMOST-Anheben erzwingt das echte Nach-vorn (Hintergrund-Aufruf per
    // Hotkey/Tray wuerde sonst nur in der Taskleiste blinken); danach zuruecknehmen.
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(g_hwnd);
    SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // Fallback: Vollbild-Spiel mit Input-Besitz blockt die Windows-Foreground-Sperre
    // (Lumora wirkt "abgestuerzt"). Dann per synthetischem Alt-Tipp entsperren + erzwingen.
    if (GetForegroundWindow() != g_hwnd) {
        keybd_event(VK_MENU, 0, 0, 0); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetForegroundWindow(g_hwnd); ShowWindow(g_hwnd, SW_SHOW);
    }
    // Fokus INS WebView2 setzen: Chromium liefert Gamepad-Input (navigator.getGamepads)
    // nur bei Dokument-Fokus. Ohne das musste nach dem Gamepad-Hotkey erst einmal mit
    // der Maus in die UI geklickt werden, bevor der Controller reagierte.
    if (g_controller) g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}
static void toggleMainWindow() {
    if (IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd) && GetForegroundWindow() == g_hwnd) {
        restoreGameFocus();   // ERST dem Spiel den Vordergrund sauber zurueckgeben, DANN verstecken
        ShowWindow(g_hwnd, SW_HIDE);
    } else showMainWindow();
}
// --- Gamepad-Hotkeys (nativer XInput-Poll wie Electrons pollNativeHotkeys: laeuft
// fokusunabhaengig, auch waehrend ein Spiel im Vordergrund ist) ---
#define TIMER_XINPUT 108
static void toggleOsdSetting();   // (unten definiert)
static void setOsdEditMode(bool on);
// Standard-Gamepad-API-Index -> XINPUT_GAMEPAD-Pruefung (Masken 1:1 aus main.js XI_MASK)
static bool xiButtonDown(const XINPUT_GAMEPAD& g, int idx) {
    switch (idx) {
    case 0: return (g.wButtons & 0x1000) != 0; case 1: return (g.wButtons & 0x2000) != 0;
    case 2: return (g.wButtons & 0x4000) != 0; case 3: return (g.wButtons & 0x8000) != 0;
    case 4: return (g.wButtons & 0x0100) != 0; case 5: return (g.wButtons & 0x0200) != 0;
    case 6: return g.bLeftTrigger > 40;        case 7: return g.bRightTrigger > 40;
    case 8: return (g.wButtons & 0x0020) != 0; case 9: return (g.wButtons & 0x0010) != 0;
    case 10: return (g.wButtons & 0x0040) != 0; case 11: return (g.wButtons & 0x0080) != 0;
    case 12: return (g.wButtons & 0x0001) != 0; case 13: return (g.wButtons & 0x0002) != 0;
    case 14: return (g.wButtons & 0x0004) != 0; case 15: return (g.wButtons & 0x0008) != 0;
    }
    return false;
}
// Kombi auf irgendeinem der 4 XInput-Slots vollstaendig gedrueckt?
static bool gamepadComboDown(const json& combo) {
    if (!combo.is_array() || combo.empty()) return false;
    for (DWORD slot = 0; slot < 4; ++slot) {
        XINPUT_STATE st{};
        if (XInputGetState(slot, &st) != ERROR_SUCCESS) continue;
        bool all = true;
        for (auto& b : combo) { if (!b.is_number_integer() || !xiButtonDown(st.Gamepad, b.get<int>())) { all = false; break; } }
        if (all) return true;
    }
    return false;
}
// Tastatur-Hotkeys: fokusunabhaengiger GetAsyncKeyState-Poll (wie main.js
// pollNativeHotkeys) - greift auch im Vollbild-Spiel, wo RegisterHotKey/globaler
// Shortcut vom Spiel/UIPI abgefangen wird. Die Liste baut rebuildKbHotkeys().
struct KbHotkey { UINT vk; bool alt, ctrl, shift; std::function<void()> action; bool down; };
static std::vector<KbHotkey> g_kbHotkeys;
static std::mutex g_kbMx;
static const ULONGLONG GP_RELEASE_MS = 150;   // Read-Aussetzer einer gehaltenen Gamepad-Kombi ueberbruecken
static bool g_gpMainWas = false, g_gpOsdWas = false;
static ULONGLONG g_gpMainSeen = 0, g_gpOsdSeen = 0;
static void xinputTick() {   // 25 Hz; Flanke = einmal ausloesen pro Druck
    json s = loadSettings();
    ULONGLONG now = GetTickCount64();
    // 1) Gamepad-Kombis (XInput) mit Release-Debounce gegen Read-Aussetzer.
    bool rawM = gamepadComboDown(s.value("gamepadHotkey", json::array()));
    if (rawM) g_gpMainSeen = now;
    bool m = rawM || (g_gpMainSeen && now - g_gpMainSeen < GP_RELEASE_MS);
    bool rawO = gamepadComboDown(s.value("gamepadOsdHotkey", json::array()));
    if (rawO) g_gpOsdSeen = now;
    bool o = rawO || (g_gpOsdSeen && now - g_gpOsdSeen < GP_RELEASE_MS);
    if (m && !g_gpMainWas) toggleMainWindow();
    if (o && !g_gpOsdWas) toggleOsdSetting();
    g_gpMainWas = m; g_gpOsdWas = o;
    // 2) Tastatur-Hotkeys (steigende Flanke, EXAKTE Modifier wie main.js).
    std::lock_guard<std::mutex> lk(g_kbMx);
    if (!g_kbHotkeys.empty()) {
        auto dn = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        bool alt = dn(VK_MENU), ctrl = dn(VK_CONTROL), shift = dn(VK_SHIFT);
        for (auto& h : g_kbHotkeys) {
            bool on = dn((int)h.vk) && h.alt == alt && h.ctrl == ctrl && h.shift == shift;
            if (on && !h.down) h.action();
            h.down = on;
        }
    }
}
// IP-Wechsel-Watcher (5-min-Tick, 1:1 aus main.js bcIpWatchTick): DSL-Zwangstrennung/
// IP-Wechsel mitten im Stream nachziehen (mtx-Config, Portfreigaben, Links, Roster) +
// IPv6-Pinhole-12h-Erneuerung (24h-Lease). Laeuft im Worker (SOAP blockiert nicht).
// Steht hinter dem Gruppen-Modul, weil es bcShareUrl/groupSelfEntry/g_watchCode nutzt.
static void bcIpWatchTick() {
    if (!g_bcState.value("active", false) || g_bcState.value("opening", true)) return;
    std::thread([]() {
        std::string ip = luupnp::getExternalIp();
        std::string ip6 = luupnp::publicIPv6();
        json s = loadSettings();
        bool forceV6 = s.value("streamForceIPv6", false);
        std::vector<std::string> hosts;
        if (!forceV6 && !ip.empty()) hosts.push_back(ip);
        if (!ip6.empty()) hosts.push_back(ip6);
        if (hosts.empty()) return;   // gerade kein Netz -> nichts anfassen
        std::vector<std::string> cur;
        for (auto& h : s.value("streamLastHosts", json::array())) if (h.is_string()) cur.push_back(h.get<std::string>());
        bool changed = hosts != cur;
        bool renewPinholes = !ip6.empty() && g_bcPinholeSetAt > 0 && GetTickCount64() - g_bcPinholeSetAt > 12ull * 3600 * 1000;
        if (!changed && !renewPinholes) return;
        if (changed) {
            bcLogStream("ipwatch: oeffentliche Adresse geaendert");
            bcWriteMtxConfig(hosts);   // mediamtx-Hot-Reload
            bcPushIceConfig(hosts);    // nativer Relay: Push (Sessions bleiben verbunden)
            json s2 = loadSettings(); json arr = json::array(); for (auto& h : hosts) arr.push_back(h);
            s2["streamLastHosts"] = arr; writeFile(settingsPath(), s2.dump(2));
            if (!forceV6 && !ip.empty()) { luupnp::mapPort(BROADCAST_PORT, "TCP", "Lumora Stream"); luupnp::mapPort(MTX_ICE_UDP, "UDP", "Lumora Stream Medien"); }
            std::string l4 = ip.empty() ? g_bcState.value("linkV4", "") : ("http://" + ip + ":" + std::to_string(BROADCAST_PORT) + "/");
            std::string l6 = ip6.empty() ? g_bcState.value("linkV6", "") : ("http://[" + ip6 + "]:" + std::to_string(BROADCAST_PORT) + "/");
            g_bcState["linkV4"] = l4; g_bcState["linkV6"] = l6;
            std::string su = bcShareUrl(); g_bcState["link"] = !su.empty() ? su : (!l4.empty() ? l4 : l6);   // Watch-URL bewahren (haengt am Code)
            if (!g_watchCode.empty()) { json self = groupSelfEntry(); groupRelay("update", { {"code", g_watchCode} }, &self); }
            else if (g_bcState.value("internet", false)) bcRegisterWatchLink();
            bcPushState();
            if (!g_group.is_null()) { groupPushState(); groupTickOnce(); }
        }
        if (!ip6.empty() && (changed || renewPinholes)) {   // Pinholes neu/erneuern
            for (auto& id : g_bcPinholeIds) luupnp::deletePinhole(id);
            g_bcPinholeIds.clear();
            std::string t6 = luupnp::addPinhole(ip6, BROADCAST_PORT, "TCP");
            std::string u6 = luupnp::addPinhole(ip6, MTX_ICE_UDP, "UDP");
            if (!t6.empty()) g_bcPinholeIds.push_back(t6);
            if (!u6.empty()) g_bcPinholeIds.push_back(u6);
            if (!t6.empty() && !u6.empty()) g_bcPinholeSetAt = GetTickCount64();
            bcLogStream("ipwatch: IPv6-Pinholes " + std::string(changed ? "neu gesetzt" : "erneuert"));
        }
    }).detach();
}
static void createTray() {
    if (g_trayOn) return;
    g_nid = {}; g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_SHELL_TRAY;
    g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), L"IDI_ICON1");
    wcscpy_s(g_nid.szTip, L"Lumora");
    g_trayOn = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}
static void destroyTray() {
    if (!g_trayOn) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayOn = false;
}
// Tray-Balloon "Portfreigabe noetig" (Pendant zu Electrons Notification). Nur mit
// aktivem Tray-Icon; sonst zeigt die UI die Anleitung ohnehin (broadcastState.forwardHint).
static void notifyForwardIssue() {
    if (g_bcForwardNotified || !g_trayOn) return;
    g_bcForwardNotified = true;
    NOTIFYICONDATAW n{}; n.cbSize = sizeof(n); n.hWnd = g_hwnd; n.uID = 1; n.uFlags = NIF_INFO;
    wcscpy_s(n.szInfoTitle, L"Stream nur im lokalen Netz erreichbar");
    wcscpy_s(n.szInfo, L"Dein Router öffnet die Ports nicht automatisch. Klick hier für die Anleitung.");
    n.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}
// Accelerator "Alt+L" / "Ctrl+Shift+F1" -> RegisterHotKey-Parameter.
static bool parseAccelerator(const std::string& acc, UINT& mods, UINT& vk) {
    mods = 0; vk = 0;
    std::string rest = acc; size_t p;
    while ((p = rest.find('+')) != std::string::npos) {
        std::string part = rest.substr(0, p); rest = rest.substr(p + 1);
        for (auto& c : part) c = (char)tolower((unsigned char)c);
        if (part == "alt") mods |= MOD_ALT; else if (part == "ctrl" || part == "control" || part == "commandorcontrol" || part == "cmdorctrl") mods |= MOD_CONTROL;
        else if (part == "shift") mods |= MOD_SHIFT; else if (part == "super" || part == "win") mods |= MOD_WIN;
    }
    if (rest.empty()) return false;
    if (rest.size() == 1) { char c = (char)toupper((unsigned char)rest[0]); vk = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ? c : VkKeyScanA(rest[0]) & 0xFF; }
    else if ((rest[0] == 'F' || rest[0] == 'f') && rest.size() <= 3) { int f = atoi(rest.c_str() + 1); if (f >= 1 && f <= 24) vk = VK_F1 + f - 1; }
    else { std::string r = rest; for (auto& c : r) c = (char)tolower((unsigned char)c);
        if (r == "space") vk = VK_SPACE; else if (r == "tab") vk = VK_TAB; else if (r == "escape" || r == "esc") vk = VK_ESCAPE;
        else if (r == "home") vk = VK_HOME; else if (r == "end") vk = VK_END; else if (r == "pageup") vk = VK_PRIOR; else if (r == "pagedown") vk = VK_NEXT; }
    return vk != 0;
}
static void rebuildKbHotkeys();   // Poll-Liste (unten definiert; nutzt startBroadcast etc.)
static bool registerHotkeys() {
    // Die Shell pollt die Tastatur-Hotkeys fokusunabhaengig (xinputTick/GetAsyncKeyState),
    // damit sie AUCH im Vollbild-Spiel greifen - RegisterHotKey/globaler Shortcut wird
    // dort vom Spiel/UIPI abgefangen. Alte RegisterHotKey-Bindungen sicherheitshalber loesen.
    UnregisterHotKey(g_hwnd, HK_TOGGLE); UnregisterHotKey(g_hwnd, HK_STREAM); UnregisterHotKey(g_hwnd, HK_OSD); UnregisterHotKey(g_hwnd, HK_OSDEDIT); UnregisterHotKey(g_hwnd, HK_OSDAB);
    rebuildKbHotkeys();
    return true;
}
// Autostart: HKCU-Run-Key "Lumora" + Legacy-/Doppel-Keys raeumen (der bekannte
// "Autostart startet im Vordergrund"-Bug entstand durch ZWEI Run-Keys derselben App).
// PARALLELBETRIEB-SCHUTZ: Im Beta-Betrieb NEBEN der Electron-App bleibt das ein No-op -
// sonst wuerde die Shell den Electron-Key com.lumora.app loeschen und sich selbst
// eintragen (beim User real autostart=true!). SCHARF erst mit dem Installer (Phase 4):
// dann Legacy-Keys ('electron.app.HDR Launcher','electron.app.Lumora','com.lumora.app')
// loeschen + eigenen Key "Lumora" -> "<exe>" [--minimized] setzen/entfernen.
static void applyAutostart() {
    // 1:1 aus main.js cleanupLegacyAutostart+applyAutostart. Umstiegs-scharf: entfernt die
    // Electron-/Rebrand-Autostart-Keys (Doppelstart-Bug) und setzt EINEN eigenen Key.
    const wchar_t* run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    for (const wchar_t* k : { L"electron.app.HDR Launcher", L"electron.app.Lumora", L"com.lumora.app" })
        RegDeleteKeyValueW(HKEY_CURRENT_USER, run, k);   // verwaiste/doppelte Eintraege raeumen
    json s = loadSettings();
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, run, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        if (s.value("autostart", false)) {
            wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring cmd = std::wstring(L"\"") + exe + L"\"" + (s.value("startMinimized", true) ? L" --minimized" : L"");
            RegSetValueExW(h, L"Lumora", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        } else RegDeleteValueW(h, L"Lumora");
        RegCloseKey(h);
    }
}
// lumora://-Protokoll auf die native exe registrieren (HKCU, kein Admin). Uebernimmt
// den Handler beim Umstieg von der Electron-App. Idempotent, bei jedem Start aufgerufen.
static void registerProtocol() {
    wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\lumora", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        const wchar_t* desc = L"URL:Lumora Protocol";
        RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)desc, (DWORD)((wcslen(desc) + 1) * sizeof(wchar_t)));
        RegSetValueExW(k, L"URL Protocol", 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
        RegCloseKey(k);
    }
    HKEY c;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\lumora\\shell\\open\\command", 0, nullptr, 0, KEY_WRITE, nullptr, &c, nullptr) == ERROR_SUCCESS) {
        std::wstring cmd = std::wstring(L"\"") + exe + L"\" \"%1\"";
        RegSetValueExW(c, nullptr, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(c);
    }
}
// Deep-Link (lumora://...) verarbeiten. Die PROTOKOLL-Registrierung uebernimmt erst der
// Installer (Phase 4) - sonst wuerde die Beta-Shell der produktiven Electron-App den
// lumora://-Handler wegnehmen. Verarbeiten kann die Shell die Links aber schon (Argument
// beim Start bzw. WM_COPYDATA einer Zweitinstanz).
static void handleDeepLink(const std::string& url) {
    std::smatch m;
    if (std::regex_search(url, m, std::regex("^lumora://join/([A-Za-z0-9]+)", std::regex::icase))) {
        std::string code = m[1]; for (auto& c : code) c = (char)toupper((unsigned char)c);
        showMainWindow();
        sendToUi("deep-join", { {"code", code} });
    } else if (url.rfind("lumora://forward-help", 0) == 0) {
        showMainWindow();
        sendToUi("show-forward-help", nullptr);
    }
}

// --- Modul: OSD-Overlay (zweites WebView2-Fenster mit osd.html; wie Electrons
// frame:false/transparent/focusable:false-Overlay: click-through, hoechste Ebene,
// stiehlt dem Spiel nie den Fokus). Sensorik (osd-data) folgt als eigener Schritt -
// Fenster, Konfiguration (Ecke/Deckkraft/Theme/Felder/Zoom) und Kanaele sind komplett.
#define WM_SHELL_OSDMSG (WM_APP + 3)
static ComPtr<ICoreWebView2Controller> g_osdCtrl;
static ComPtr<ICoreWebView2> g_osdWv;
static bool g_osdLoaded = false;

static void sendToOsd(const std::string& channel, const json& payload);
#define TIMER_OSDDATA 107
#define TIMER_OSDFPS 110   // schneller Frametime-Graph-Tick (~30 Hz), getrennt von den 5-Hz-Sensoren
#define TIMER_LANPUSH 111  // LAN-Gruppen (Beacon-Empfang) alle 5 s an die UI melden
// --- OSD-Sensorik Teil 1 (treiberfrei): PDH-CPU/GPU-Last, RAM, DXGI-VRAM.
// Temp/Takt/Power (NVML/ADL/PawnIO) + FPS (PresentMon) folgen als Teil 2.
static PDH_HQUERY g_pdhQ = nullptr;
static PDH_HCOUNTER g_pdhCpu = nullptr, g_pdhGpu = nullptr, g_pdhCpuPerf = nullptr;
static bool g_pdhInit = false;
static void sensorsInit() {
    if (g_pdhInit) return;
    g_pdhInit = true;
    if (PdhOpenQueryW(nullptr, 0, &g_pdhQ) != ERROR_SUCCESS) return;
    // % Processor Utility = die Taskmanager-Metrik (turbo-normiert); GPU-3D-Summe aller Engines
    PdhAddEnglishCounterW(g_pdhQ, L"\\Processor Information(_Total)\\% Processor Utility", 0, &g_pdhCpu);
    PdhAddEnglishCounterW(g_pdhQ, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &g_pdhGpu);
    // % Processor Performance = Ist-Takt-Faktor (kann >100 = Boost) * Basis-MHz -> CPU-Takt
    // OHNE Afterburner (genau der vom User gemeldete fehlende clock-Wert). Registrierung
    // ging beim git-Reset verloren; readCpuNative nutzt g_pdhCpuPerf, es war aber nullptr.
    PdhAddEnglishCounterW(g_pdhQ, L"\\Processor Information(_Total)\\% Processor Performance", 0, &g_pdhCpuPerf);
    PdhCollectQueryData(g_pdhQ);   // Basissample (Delta-Zaehler)
    luadl::setup();   // AMD-GPU-Sensoren (atiadlxx.dll) einmalig - inert ohne AMD-Treiber
}
static json readMahm();       // MSI-Afterburner-Sensoren (Definition weiter unten)
static json readSenseCpu();   // PawnIO-Broker (Definition weiter unten)
static bool g_osdEdit = false; // Live-Edit (Alt+Shift+O): Overlay faengt die Maus
static json readCpuNative() {
    // Marke/Modell aus der Registry, Bereinigung wie readCpu in main.js
    wchar_t nm[128] = {}; DWORD sz = sizeof(nm);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, nm, &sz);
    std::string raw = narrow(nm), low = raw; for (auto& c : low) c = (char)tolower((unsigned char)c);
    std::string brand = low.find("intel") != std::string::npos ? "INTEL" : (low.find("amd") != std::string::npos || low.find("ryzen") != std::string::npos) ? "AMD" : "CPU";
    std::string model = std::regex_replace(raw, std::regex("\\((R|TM)\\)", std::regex::icase), "");
    model = std::regex_replace(model, std::regex("^AMD\\s+|^Intel\\s+", std::regex::icase), "");
    model = std::regex_replace(model, std::regex("\\s+w/.*$|\\s+with\\s+.*$|\\s+\\d+-Core Processor.*$|\\s+CPU.*$", std::regex::icase), "");
    size_t a = model.find_first_not_of(' '), b2 = model.find_last_not_of(' ');
    if (a != std::string::npos) model = model.substr(a, b2 - a + 1);
    json load = nullptr;
    PDH_FMT_COUNTERVALUE v;
    if (g_pdhCpu && PdhGetFormattedCounterValue(g_pdhCpu, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS)
        load = (int)(std::min)(100.0, v.doubleValue + 0.5);
    MEMORYSTATUSEX ms{ sizeof(ms) }; GlobalMemoryStatusEx(&ms);
    json clock = nullptr;   // Ist-Takt = Basis-MHz (Registry ~MHz) * % Processor Performance
    DWORD baseMhz = 0, msz = sizeof(baseMhz);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, nullptr, &baseMhz, &msz);
    if (g_pdhCpuPerf && baseMhz && PdhGetFormattedCounterValue(g_pdhCpuPerf, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS && v.doubleValue > 0)
        clock = (int)(baseMhz * v.doubleValue / 100.0 + 0.5);
    // CPU-Temp/Takt/Power-Kette 1:1 wie Electrons readCpu: Afterburner (MAHM) hat
    // Vorrang und liefert alle drei; sonst PawnIO-Broker (temp/power) + PDH-Takt.
    json temp = nullptr, power = nullptr;
    json m = readMahm();
    if (m.is_object() && m.contains("cpuTemp")) {
        temp = m["cpuTemp"];
        if (m.contains("cpuClock")) clock = m["cpuClock"];
        if (m.contains("cpuPower")) power = m["cpuPower"];
    } else {
        json sense = readSenseCpu();
        if (sense.is_object()) { temp = sense["temp"]; power = sense["power"]; }
    }
    return { {"brand", brand}, {"name", model}, {"load", load},
             {"ram", (long long)((ms.ullTotalPhys - ms.ullAvailPhys) / 1048576)},
             {"temp", temp}, {"clock", clock}, {"power", power} };
}
// --- FPS-/Sensor-Broker-Anbindung (Shared Memory, Layout 1:1 aus main.js) ---
// Die elevated Broker (geplante Aufgaben LumoraOSD-FPS / LumoraOSD-Sensors)
// existieren bereits (Electron-Infrastruktur); die Shell liest dieselben Sections
// und sendet denselben Heartbeat (appTick@24, wanted@28). Eigene Broker-Modi der
// Shell folgen mit Phase 4 - bis dahin teilen sich beide Apps die Datenquelle.
#pragma pack(push, 1)
struct FpsShmFull { uint32_t magic, brokerTick, fps, frametimeX100, apiCode, pid, appTick, wanted; };
struct SenseShmFull { uint32_t magic, brokerTick; int32_t tempX10, powerX10; uint32_t pid, _r, appTick, wanted; };
#pragma pack(pop)
static const uint32_t FPS_MAGIC = 0x4C4F5344, SENSE_MAGIC = 0x4C4F5345;   // 'LOSD'/'LOSE'
struct ShmMap { HANDLE h = nullptr; };
static ShmMap g_fpsShm, g_senseShm;
static ULONGLONG g_brokerSpawnAt = 0;
static bool shmOpen(ShmMap& m, const char* name) {
    if (m.h) return true;
    m.h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, name);
    return m.h != nullptr;
}
template<typename T> static bool shmRead(ShmMap& m, T& out) {   // frisches Mapping je Zugriff (Kohaerenz-Lehre)
    if (!m.h) return false;
    void* p = MapViewOfFile(m.h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p) return false;
    memcpy(&out, p, sizeof(T)); UnmapViewOfFile(p);
    return true;
}
static void shmWriteApp(ShmMap& m, uint32_t wanted) {   // Heartbeat @24 (appTick, wanted)
    if (!m.h) return;
    void* p = MapViewOfFile(m.h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p) return;
    ((uint32_t*)p)[6] = GetTickCount(); ((uint32_t*)p)[7] = wanted;
    UnmapViewOfFile(p);
}
static bool runTask(const wchar_t* task) {   // geplante Broker-Aufgabe starten (elevated ohne UAC)
    std::string out = runCaptureOutput(std::wstring(L"schtasks /run /tn \"") + task + L"\"", 8000);
    return out.find("ERROR") == std::string::npos && out.find("FEHLER") == std::string::npos;
}
// FPS lesen (Frische: magic + brokerTick-Alter <= 1500ms; 1,5s-Ueberbrueckung wie main.js)
static json g_lastFps = nullptr; static ULONGLONG g_lastFpsAt = 0;
static json readBrokerFps() {
    if (!shmOpen(g_fpsShm, "Local\\LumoraOSDFps")) return nullptr;
    uint32_t now = GetTickCount();
    shmWriteApp(g_fpsShm, 1);
    FpsShmFull s{};
    if (shmRead(g_fpsShm, s) && s.magic == FPS_MAGIC && (uint32_t)(now - s.brokerTick) <= 1500 && s.fps) {
        g_lastFps = { {"fps", s.fps}, {"frametime", s.frametimeX100 / 100.0} };
        g_lastFpsAt = GetTickCount64();
        return g_lastFps;
    }
    if (!g_lastFps.is_null() && GetTickCount64() - g_lastFpsAt < 1500) return g_lastFps;
    return nullptr;
}
// FPS-Quelle waehlen (osdFpsSource, 1:1 main.js): 'rtss' -> RTSS, 'presentmon' ->
// PresentMon-Broker, 'auto' -> RTSS wenn verfuegbar, sonst Broker. g_fpsUseRtss
// merkt sich die Wahl fuer osdDataTick (nur dann brokersEnsure).
static bool g_fpsUseRtss = false;
static ULONGLONG g_fpsSrcAt = 0;
static json readFps() {   // ~30 Hz aufrufbar: Quelle nur 2x/s aus den Settings lesen (kein Datei-I/O je Frame)
    ULONGLONG now = GetTickCount64();
    if (!g_fpsSrcAt || now - g_fpsSrcAt > 500) {
        std::string src = loadSettings().value("osdFpsSource", std::string("auto"));
        g_fpsUseRtss = (src == "rtss") ? true : (src == "presentmon") ? false : lurtss::available();
        g_fpsSrcAt = now;
    }
    if (g_fpsUseRtss) {
        lurtss::FpsOut r = lurtss::readFps();
        return r.ok ? json{ {"fps", r.fps}, {"frametime", r.frametime} } : json(nullptr);
    }
    return readBrokerFps();
}
// MSI Afterburner "MAHMSharedMemory" (1:1 aus main.js readMahmRaw): laeuft Afterburner,
// liefert es CPU-Temp/-Takt/-Watt. Header {sig 'MAHM', ver, headerSize, numEntries,
// entrySize}; Eintrag = 5x char[260] (Name zuerst), float bei +5*260. 150ms-Cache.
static json g_mahmVal = nullptr; static ULONGLONG g_mahmValAt = 0;
static json readMahmRaw() {
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, "MAHMSharedMemory");
    if (!h) return nullptr;
    json out = nullptr;
    void* base = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (base) {
        const uint32_t* hdr = (const uint32_t*)base;
        if (hdr[0] == 0x4D41484D && hdr[3]) {
            uint32_t headerSize = hdr[2], numEntries = hdr[3], entrySize = hdr[4];
            static const std::pair<const char*, const char*> WANT[] = {
                {"cpuTemp", "CPU temperature"}, {"cpuClock", "CPU clock"}, {"cpuPower", "CPU power"},
                {"gpuTemp", "GPU temperature"}, {"gpuClock", "Core clock"}, {"gpuPower", "Power"},
                {"gpuLoad", "GPU usage"}, {"gpuVram", "Memory usage"} };
            json o = json::object();
            for (uint32_t i = 0; i < numEntries; ++i) {
                const char* e = (const char*)base + headerSize + (size_t)i * entrySize;
                for (auto& [key, name] : WANT)
                    if (strncmp(e, name, 260) == 0) { float f = *(const float*)(e + 5 * 260); o[key] = (int)(f >= 0 ? f + 0.5f : f - 0.5f); }
            }
            if (!o.empty()) out = o;
        }
        UnmapViewOfFile(base);
    }
    CloseHandle(h);
    return out;
}
static json readMahm() {
    ULONGLONG now = GetTickCount64();
    if (g_mahmValAt && now - g_mahmValAt < 150) return g_mahmVal;
    g_mahmVal = readMahmRaw(); g_mahmValAt = now;
    return g_mahmVal;
}
static json readSenseCpu() {   // CPU-Temp/-Watt vom PawnIO-Broker
    if (!shmOpen(g_senseShm, "Local\\LumoraOSDSense")) return nullptr;
    uint32_t now = GetTickCount();
    shmWriteApp(g_senseShm, 1);
    SenseShmFull s{};
    if (shmRead(g_senseShm, s) && s.magic == SENSE_MAGIC && (uint32_t)(now - s.brokerTick) <= 3000 && s.tempX10 > 0)
        return { {"temp", s.tempX10 / 10.0}, {"power", s.powerX10 > 0 ? json(s.powerX10 / 10.0) : json(nullptr)} };
    return nullptr;
}
// Laeuft der Sensor-Broker gerade (frischer brokerTick)? Fuer die Quellen-Anzeige.
static bool senseBrokerAlive() {
    if (!shmOpen(g_senseShm, "Local\\LumoraOSDSense")) return false;
    SenseShmFull s{}; uint32_t now = GetTickCount();
    return shmRead(g_senseShm, s) && s.magic == SENSE_MAGIC && (uint32_t)(now - s.brokerTick) <= 3000;
}
static void brokersEnsure() {   // beide Aufgaben anstossen (idempotent, 4s-Spawn-Sperre)
    if (GetTickCount64() - g_brokerSpawnAt < 4000) return;
    g_brokerSpawnAt = GetTickCount64();
    // WICHTIG (wie Electrons startBroker): Sections App-first erstellen UND wanted=1
    // VOR dem Task-Start schreiben - der Broker prueft das Flag beim Hochkommen und
    // beendet sich sonst sofort wieder ("App will keine Werte").
    shmOpen(g_fpsShm, "Local\\LumoraOSDFps"); shmWriteApp(g_fpsShm, 1);
    shmOpen(g_senseShm, "Local\\LumoraOSDSense"); shmWriteApp(g_senseShm, 1);
    std::thread([]() { runTask(L"LumoraOSD-FPS"); runTask(L"LumoraOSD-Sensors"); }).detach();
}

// --- OSD-Ersteinrichtung (EIN Erklaer-Dialog, EIN UAC) ------------------------
// Beim OSD-Einschalten das einrichten, was fuer die Vollausstattung fehlt: FPS-Task
// (PresentMon), PawnIO-Treiber (Download + Authenticode-Pruefung von der offiziellen
// Quelle + Silent-Install) und Sensor-Task. Alles Elevated in EINEM Aufruf ueber
// lumora-elevate.exe -> genau EIN UAC-Dialog (zeigt Lumora, nicht "PowerShell").
// 1:1 aus main.js ensureOsdSetup portiert.
static std::atomic<bool> g_osdSetupRunning{ false };
static std::wstring pawnioDirW() {
    wchar_t pf[MAX_PATH]; if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) wcscpy_s(pf, L"C:\\Program Files");
    return std::wstring(pf) + L"\\PawnIO";
}
static bool pawnioInstalled() { return GetFileAttributesW((pawnioDirW() + L"\\PawnIOLib.dll").c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool schtaskPresent(const wchar_t* task) {
    std::string out = runCaptureOutput(std::wstring(L"schtasks /query /tn \"") + task + L"\"", 8000);
    return !out.empty() && out.find("ERROR") == std::string::npos && out.find("FEHLER") == std::string::npos;
}
static bool fpsTaskPresent()   { return schtaskPresent(L"LumoraOSD-FPS"); }
static bool senseTaskPresent() { return schtaskPresent(L"LumoraOSD-Sensors"); }
static bool fpsNeedsSetup() {
    std::string src = loadSettings().value("osdFpsSource", std::string("auto"));
    bool useRtss = (src == "rtss");   // RTSS-Weg (Task #24) braucht keinen FPS-Task
    return !useRtss && !fpsTaskPresent();
}
static bool sensorsNeedSetup() {
    if (!lubroker::cpuSensorModule()) return false;   // CPU nicht von PawnIO abgedeckt
    json m = readMahm();
    if (!m.is_null() && m.contains("cpuTemp")) return false;   // Afterburner liefert bereits
    return !pawnioInstalled() || !senseTaskPresent();
}
static void ensureOsdSetup() {
    if (g_osdSetupRunning.load()) return;
    if (loadSettings().value("osdSetupDeclined", false)) return;
    bool needFps = fpsNeedsSetup();
    bool sensorGap = sensorsNeedSetup();
    bool needPawnio = sensorGap && !pawnioInstalled();
    bool needSense = sensorGap && !senseTaskPresent();
    if (!needFps && !needPawnio && !needSense) return;
    g_osdSetupRunning = true;
    std::thread([needFps, needPawnio, needSense]() {
        auto q = [](std::wstring s) { size_t p = 0; while ((p = s.find(L'\'', p)) != std::wstring::npos) { s.insert(p, 1, L'\''); p += 2; } return s; };
        // 0) Erklaer-Dialog - listet nur, was wirklich fehlt.
        std::wstring parts;
        if (needFps)        parts += L"• FPS-Messung: kleiner Hintergrunddienst (PresentMon, liegt Lumora bei)\n";
        if (needPawnio)     parts += L"• CPU-Temperatur & -Verbrauch: signierter Open-Source-Treiber PawnIO (wird von der offiziellen Quelle geladen)\n";
        else if (needSense) parts += L"• CPU-Temperatur & -Verbrauch: Hintergrunddienst fuer den PawnIO-Treiber\n";
        std::wstring dlg = L"Damit das OSD alle Werte anzeigen kann, richtet Lumora einmalig ein:\n\n" + parts +
            L"\nGleich fragt Windows EINMAL nach deiner Bestaetigung (Administratorrechte). Danach laeuft alles automatisch – ohne weitere Nachfragen.";
        if (MessageBoxW(g_hwnd, dlg.c_str(), L"OSD einrichten", MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND) != IDOK) {
            json s = loadSettings(); s["osdSetupDeclined"] = true; writeFile(settingsPath(), s.dump(2));
            sendToUi("osd-fps-off", nullptr);   // Schalter zurueck auf "aus", nicht staendig neu fragen
            g_osdSetupRunning = false; return;
        }
        auto status     = [](const std::string& m) { sendToUiMulti("osd-setup-status", json::array({ m, false })); };
        auto statusDone = [](const std::string& m) { sendToUiMulti("osd-setup-status", json::array({ m, true })); };
        auto statusOff  = []()                      { sendToUiMulti("osd-setup-status", json::array({ nullptr, false })); };
        // 1) PawnIO-Installer non-elevated laden + Authenticode-Signatur pruefen.
        std::wstring setupExe;
        if (needPawnio) {
            status("OSD-Einrichtung: Lade PawnIO-Treiber herunter und pruefe die Signatur ...");
            wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
            setupExe = std::wstring(tmp) + L"PawnIO_setup.exe";
            std::wstring dl = L"Invoke-WebRequest -Uri 'https://github.com/namazso/PawnIO.Setup/releases/latest/download/PawnIO_setup.exe' -OutFile '"
                + q(setupExe) + L"' -UseBasicParsing; (Get-AuthenticodeSignature '" + q(setupExe) + L"').Status";
            std::string out = runCaptureOutput(L"powershell -NoProfile -Command \"" + dl + L"\"", 180000);
            if (out.find("Valid") == std::string::npos) {
                statusOff();
                MessageBoxW(g_hwnd, L"Der PawnIO-Treiber konnte nicht geladen oder verifiziert werden.\nBitte Internetverbindung pruefen - die Einrichtung wird beim naechsten Einschalten des OSD erneut angeboten.",
                    L"PawnIO-Download fehlgeschlagen", MB_OK | MB_ICONERROR);
                g_osdSetupRunning = false; return;
            }
        }
        // 2) EIN elevated Aufruf: [PawnIO-Silent-Install] + [FPS-Task] + [Sensor-Task].
        wchar_t exeW[MAX_PATH]; GetModuleFileNameW(nullptr, exeW, MAX_PATH);
        std::wstring exe = q(exeW);
        auto taskPs = [&](const std::wstring& task, const std::wstring& arg, const std::wstring& v) -> std::wstring {
            return L"$a" + v + L"=New-ScheduledTaskAction -Execute '" + exe + L"' -Argument '" + arg + L"'; "
                   L"$p" + v + L"=New-ScheduledTaskPrincipal -UserId $env:USERNAME -RunLevel Highest -LogonType Interactive; "
                   L"$s" + v + L"=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew; "
                   L"Register-ScheduledTask -TaskName '" + task + L"' -Action $a" + v + L" -Principal $p" + v + L" -Settings $s" + v + L" -Force";
        };
        std::vector<std::wstring> ps;
        if (needPawnio) ps.push_back(L"Start-Process -FilePath '" + q(setupExe) + L"' -ArgumentList '-install','-silent' -Wait");
        if (needFps)    ps.push_back(taskPs(L"LumoraOSD-FPS", L"--fps-broker", L""));
        if (needSense || needPawnio) ps.push_back(taskPs(L"LumoraOSD-Sensors", L"--sensor-broker", L"2"));
        std::wstring inner; for (size_t i = 0; i < ps.size(); ++i) { if (i) inner += L"; "; inner += ps[i]; }
        std::string b64 = b64encode((const uint8_t*)inner.data(), inner.size() * 2);   // UTF-16LE wie PowerShell -EncodedCommand
        std::wstring b64w = widen(b64);
        std::wstring elevExe = binDir() + L"\\lumora-elevate.exe";
        std::wstring outer = (GetFileAttributesW(elevExe.c_str()) != INVALID_FILE_ATTRIBUTES)
            ? L"Start-Process -FilePath '" + q(elevExe) + L"' -Verb RunAs -WindowStyle Hidden -ArgumentList '--ps-encoded','" + b64w + L"'"
            : L"Start-Process powershell -Verb RunAs -WindowStyle Hidden -ArgumentList '-NoProfile -EncodedCommand " + b64w + L"'";
        status("OSD-Einrichtung: Warte auf deine Bestaetigung (Windows-Sicherheitsabfrage) ...");
        runCaptureOutput(L"powershell -NoProfile -Command \"" + outer + L"\"", 20000);
        // 3) Warten, bis alles da ist (Install braucht Momente), dann Broker anwerfen.
        bool working = false, ok = false;
        for (int tries = 0; tries < 90; ++tries) {
            bool fpsOk = !needFps || fpsTaskPresent();
            bool pioOk = !needPawnio || pawnioInstalled();
            bool senseOk = (!needSense && !needPawnio) || senseTaskPresent();
            bool progressed = (needPawnio && pawnioInstalled()) || (needFps && fpsTaskPresent()) || ((needSense || needPawnio) && senseTaskPresent());
            if (!working && progressed) { working = true; status("OSD-Einrichtung: Installiere und richte Hintergrunddienste ein ..."); }
            if (fpsOk && pioOk && senseOk) { ok = true; break; }
            Sleep(1000);
        }
        statusDone(ok ? "OSD-Einrichtung abgeschlossen - alle Werte kommen gleich rein. ✓"
                      : "OSD-Einrichtung nicht abgeschlossen - erneut ueber Einstellungen → Overlay.");
        g_brokerSpawnAt = 0; brokersEnsure();   // Broker sofort anwerfen (Spawn-Sperre zuruecksetzen)
        g_osdSetupRunning = false;
    }).detach();
}
static json readGpuPdh();
// --- NVML (NVIDIA-Treiber-API, nvml.dll): temp/power/clock/vram wie readNvmlHandle ---
struct NvmlUtil { unsigned int gpu, mem; };
struct NvmlMem { unsigned long long total, freeB, used; };
static struct {
    bool tried = false, ok = false;
    int (*init)() = nullptr;
    int (*count)(unsigned int*) = nullptr;
    int (*byIndex)(unsigned int, void**) = nullptr;
    int (*name)(void*, char*, unsigned int) = nullptr;
    int (*util)(void*, NvmlUtil*) = nullptr;
    int (*temp)(void*, int, unsigned int*) = nullptr;
    int (*power)(void*, unsigned int*) = nullptr;
    int (*clock)(void*, int, unsigned int*) = nullptr;
    int (*mem)(void*, NvmlMem*) = nullptr;
    std::vector<std::pair<void*, std::string>> devices;   // handle + Modellname
} g_nvml;
// NVIDIA-Namen kuerzen (1:1 aus main.js:341): "NVIDIA "- und "GeForce "-Vorbau weg,
// sonst passt "RTX 5080" nicht in die OSD-/Anzeige-Breite (Electron hatte das schon).
static std::string cleanNvidiaName(const std::string& raw) {
    std::string n = raw;
    n = std::regex_replace(n, std::regex("^NVIDIA\\s+", std::regex::icase), "");
    n = std::regex_replace(n, std::regex("GeForce\\s+", std::regex::icase), "");
    size_t a = n.find_first_not_of(" \t"), b = n.find_last_not_of(" \t");
    return a == std::string::npos ? std::string("NVIDIA") : n.substr(a, b - a + 1);
}
static void nvmlInitOnce() {
    if (g_nvml.tried) return;
    g_nvml.tried = true;
    HMODULE m = LoadLibraryW(L"nvml.dll");
    if (!m) m = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!m) return;
    g_nvml.init = (int(*)())GetProcAddress(m, "nvmlInit_v2");
    g_nvml.count = (int(*)(unsigned int*))GetProcAddress(m, "nvmlDeviceGetCount_v2");
    g_nvml.byIndex = (int(*)(unsigned int, void**))GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
    g_nvml.name = (int(*)(void*, char*, unsigned int))GetProcAddress(m, "nvmlDeviceGetName");
    g_nvml.util = (int(*)(void*, NvmlUtil*))GetProcAddress(m, "nvmlDeviceGetUtilizationRates");
    g_nvml.temp = (int(*)(void*, int, unsigned int*))GetProcAddress(m, "nvmlDeviceGetTemperature");
    g_nvml.power = (int(*)(void*, unsigned int*))GetProcAddress(m, "nvmlDeviceGetPowerUsage");
    g_nvml.clock = (int(*)(void*, int, unsigned int*))GetProcAddress(m, "nvmlDeviceGetClockInfo");
    g_nvml.mem = (int(*)(void*, NvmlMem*))GetProcAddress(m, "nvmlDeviceGetMemoryInfo");
    if (!g_nvml.init || !g_nvml.count || !g_nvml.byIndex || !g_nvml.util || g_nvml.init() != 0) return;
    unsigned int n = 0; g_nvml.count(&n);
    for (unsigned int i = 0; i < n; ++i) {
        void* h = nullptr;
        if (g_nvml.byIndex(i, &h) != 0 || !h) continue;
        char nm[96] = {}; if (g_nvml.name) g_nvml.name(h, nm, sizeof(nm));
        g_nvml.devices.push_back({ h, nm[0] ? cleanNvidiaName(nm) : "NVIDIA" });
    }
    g_nvml.ok = !g_nvml.devices.empty();
}
static json readNvmlHandle(void* h, const std::string& model) {
    NvmlUtil u{}; if (g_nvml.util(h, &u) != 0) return nullptr;
    unsigned int t = 0, mw = 0, c = 0; NvmlMem mem{};
    if (g_nvml.temp) g_nvml.temp(h, 0, &t);          // 0 = NVML_TEMPERATURE_GPU
    if (g_nvml.power) g_nvml.power(h, &mw);
    if (g_nvml.clock) g_nvml.clock(h, 0, &c);        // 0 = NVML_CLOCK_GRAPHICS
    if (g_nvml.mem) g_nvml.mem(h, &mem);
    return { {"brand", "NVIDIA"}, {"name", model}, {"load", u.gpu}, {"temp", t},
             {"power", (double)((int)(mw / 100.0 + 0.5)) / 10.0},   // W mit 1 Nachkommastelle wie main.js
             {"clock", c}, {"vram", (long long)(mem.used / 1048576)} };
}
// AMD-Namen bereinigen (wie main.js): (R)/(TM) raus, "AMD "-Prefix + " Graphics"-Suffix weg.
static std::string cleanAmdName(const std::string& raw) {
    std::string n = raw;
    n = std::regex_replace(n, std::regex("\\((R|TM)\\)", std::regex::icase), "");
    n = std::regex_replace(n, std::regex("^AMD\\s+", std::regex::icase), "");
    n = std::regex_replace(n, std::regex("\\s+Graphics$", std::regex::icase), "");
    size_t a = n.find_first_not_of(" \t"), b = n.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : n.substr(a, b - a + 1);
}
// VRAM (MB) des ersten DXGI-Adapters einer VendorID - ADL-PMLog liefert kein VRAM.
static json readVramMbForVendor(UINT vendorId) {
    ComPtr<IDXGIFactory1> fac; CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    if (!fac) return nullptr;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> ad; if (fac->EnumAdapters1(i, &ad) != S_OK) break;
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
        if (d.VendorId != vendorId) continue;
        ComPtr<IDXGIAdapter3> a3; ad.As(&a3);
        if (a3) { DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) return (long long)(mi.CurrentUsage / 1048576); }
        break;
    }
    return nullptr;
}
// Name des ersten echten (nicht Software-)DXGI-Adapters, fuer den MAHM-Zweig.
static std::string gpuNameFirst() {
    ComPtr<IDXGIFactory1> fac; CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    if (fac) for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> ad; if (fac->EnumAdapters1(i, &ad) != S_OK) break;
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        // Vorbau kuerzen wie in den NVML-/ADL-Zweigen (Anzeige-Breite)
        return cleanNvidiaName(cleanAmdName(narrow(d.Description)));
    }
    return "GPU";
}
// ADL-Sensoren eines Adapters -> GPU-JSON (temp/clock/power/load), VRAM via DXGI.
static json readAdlGpu(int idx, const std::string& rawName) {
    luadl::GpuVals v = luadl::read(idx, rawName);
    if (!v.ok) return nullptr;
    return { {"brand", "AMD"}, {"name", cleanAmdName(v.name)},
             {"load", v.hasLoad ? json(v.load) : json(nullptr)},
             {"temp", v.hasTemp ? json(v.temp) : json(nullptr)},
             {"power", v.hasPower ? json(v.power) : json(nullptr)},
             {"clock", v.hasClock ? json(v.clock) : json(nullptr)},
             {"vram", readVramMbForVendor(0x1002)} };
}
// Automatik: NVIDIA (NVML) -> AMD (ADL) -> Afterburner (MAHM) -> DXGI/PDH.
// Feste Auswahl (osdGpu nvml:N / adl:N) hat Vorrang, faellt sonst auf Automatik.
static json readGpuNative() {
    nvmlInitOnce();
    std::string sel = loadSettings().value("osdGpu", "auto");
    std::smatch sm;
    if (sel != "auto" && std::regex_match(sel, sm, std::regex("^(nvml|adl):(\\d+)$"))) {
        std::string kind = sm[1].str(); int n = atoi(sm[2].str().c_str());
        if (kind == "nvml" && g_nvml.ok && (size_t)n < g_nvml.devices.size()) {
            json r = readNvmlHandle(g_nvml.devices[n].first, g_nvml.devices[n].second); if (!r.is_null()) return r;
        }
        if (kind == "adl" && luadl::available()) {
            std::string nm; for (auto& a : luadl::adapters()) if (a.idx == n) { nm = a.name; break; }
            json r = readAdlGpu(n, nm); if (!r.is_null()) return r;
        }
    }
    if (g_nvml.ok) { json r = readNvmlHandle(g_nvml.devices[0].first, g_nvml.devices[0].second); if (!r.is_null()) return r; }
    if (luadl::available()) { json r = readAdlGpu(luadl::activeIdx(), luadl::activeName()); if (!r.is_null()) return r; }
    json m = readMahm();
    if (!m.is_null() && m.contains("gpuTemp")) {
        return { {"brand", "GPU"}, {"name", gpuNameFirst()},
                 {"load", m.value("gpuLoad", json(nullptr))}, {"temp", m["gpuTemp"]},
                 {"power", m.value("gpuPower", json(nullptr))}, {"clock", m.value("gpuClock", json(nullptr))},
                 {"vram", m.value("gpuVram", json(nullptr))} };
    }
    return readGpuPdh();
}
static json readGpuPdh() {
    ComPtr<IDXGIFactory1> fac; CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    ComPtr<IDXGIAdapter1> pick; DXGI_ADAPTER_DESC1 pd{};
    if (fac) { ComPtr<IDXGIAdapter1> nv, amd, intel;
        for (UINT i = 0;; ++i) { ComPtr<IDXGIAdapter1> ad; if (fac->EnumAdapters1(i, &ad) != S_OK) break; DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
            if (d.VendorId == 0x10DE && !nv) nv = ad; else if (d.VendorId == 0x1002 && !amd) amd = ad; else if (d.VendorId == 0x8086 && !intel) intel = ad; }
        pick = nv ? nv : amd ? amd : intel;
    }
    if (!pick) return nullptr;
    pick->GetDesc1(&pd);
    std::string name = narrow(pd.Description);
    std::string brand = pd.VendorId == 0x10DE ? "NVIDIA" : pd.VendorId == 0x1002 ? "AMD" : pd.VendorId == 0x8086 ? "INTEL" : "GPU";
    json load = nullptr, vram = nullptr;
    // GPU-3D-Last: Summe aller 3D-Engine-Instanzen (Wildcard-Counter -> Array)
    if (g_pdhGpu) {
        DWORD bs = 0, cnt = 0;
        PdhGetFormattedCounterArrayW(g_pdhGpu, PDH_FMT_DOUBLE, &bs, &cnt, nullptr);
        if (bs) { std::vector<uint8_t> buf(bs);
            auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
            if (PdhGetFormattedCounterArrayW(g_pdhGpu, PDH_FMT_DOUBLE, &bs, &cnt, items) == ERROR_SUCCESS) {
                double sum = 0; for (DWORD i = 0; i < cnt; ++i) sum += items[i].FmtValue.doubleValue;
                load = (int)(std::min)(100.0, sum + 0.5);
            }
        }
    }
    ComPtr<IDXGIAdapter3> a3; pick.As(&a3);   // echter VRAM-Verbrauch (wie Taskmanager)
    if (a3) { DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) vram = (long long)(mi.CurrentUsage / 1048576);
    }
    return { {"brand", brand}, {"name", name}, {"load", load}, {"temp", nullptr}, {"power", nullptr}, {"clock", nullptr}, {"vram", vram} };
}
static void osdDataTick() {
    if (!g_osdHwnd || !IsWindowVisible(g_osdHwnd)) return;
    // Oberste Lage NACHDRUECKEN (wie Electrons moveTop je Anzeige): ein startendes
    // Spiel setzt sich sonst ueber das TOPMOST-Overlay (User-Befund: OSD weg im Spiel).
    if (!g_osdEdit) SetWindowPos(g_osdHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    {   // DIAGNOSE "OSD weg im Spiel": Vordergrund-Fenster bei Wechsel protokollieren
        static HWND lastFg = nullptr; HWND fg = GetForegroundWindow();
        if (fg && fg != lastFg && fg != g_osdHwnd) { lastFg = fg;
            RECT fr{}; GetWindowRect(fg, &fr);
            HMONITOR hm2 = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY); MONITORINFO mi2{ sizeof(mi2) }; GetMonitorInfoW(hm2, &mi2);
            bool full = fr.left <= mi2.rcMonitor.left && fr.top <= mi2.rcMonitor.top && fr.right >= mi2.rcMonitor.right && fr.bottom >= mi2.rcMonitor.bottom;
            wchar_t cls[64] = {}, ti[64] = {}; GetClassNameW(fg, cls, 63); GetWindowTextW(fg, ti, 63);
            // Exklusives Vollbild (D3D-Fullscreen-Exclusive) messen: DARUEBER kann ein
            // DirectComposition-Overlay NICHT gezeichnet werden (DWM umgangen) - das
            // unterscheidet den Fall "OSD dauerhaft weg" von blossem Z-Order-Flackern.
            QUERY_USER_NOTIFICATION_STATE quns = (QUERY_USER_NOTIFICATION_STATE)0; SHQueryUserNotificationState(&quns);
            LONG_PTR fgEx = GetWindowLongPtrW(fg, GWL_EXSTYLE); LONG_PTR fgSt = GetWindowLongPtrW(fg, GWL_STYLE);
            bcLogStream("osd-fg: '" + narrow(ti) + "' klasse=" + narrow(cls) + " vollbild=" + std::to_string(full)
                + " exklusivVollbild=" + std::to_string(quns == QUNS_RUNNING_D3D_FULL_SCREEN) + " quns=" + std::to_string((int)quns)
                + " fgTopmost=" + std::to_string((fgEx & WS_EX_TOPMOST) != 0) + " fgPopup=" + std::to_string((fgSt & WS_POPUP) != 0)
                + " osdSichtbar=" + std::to_string(IsWindowVisible(g_osdHwnd) != 0));
        }
    }
    PdhCollectQueryData(g_pdhQ);   // frisches Sample fuer die Delta-Zaehler
    json payload = { {"gpu", readGpuNative()}, {"cpu", readCpuNative()} };
    // Bei abgelehnter Einrichtung sendet der schnelle FPS-Tick nichts -> hier "—".
    if (loadSettings().value("osdSetupDeclined", false)) payload["fps"] = "\xE2\x80\x94";
    sendToOsd("osd-data", payload);
}
// Schneller FPS/Frametime-Tick (~30 Hz) fuer den lebendigen Frametime-Graphen; die
// Zahlen drosselt osd.html selbst auf lesbare ~4 Hz (1:1 aus main.js startFps).
static void osdFpsTick() {
    if (!g_osdHwnd || !IsWindowVisible(g_osdHwnd)) return;
    if (loadSettings().value("osdSetupDeclined", false)) return;   // dann liefert der Sensor-Tick fps:"—"
    json f = readFps();   // RTSS oder PresentMon-Broker je nach osdFpsSource (Quelle gecacht)
    if (f.is_object()) sendToOsd("osd-data", { {"fps", f["fps"]}, {"frametime", f["frametime"]} });
    else { sendToOsd("osd-data", { {"fps", "\xE2\x80\xA6"} }); if (!g_fpsUseRtss) brokersEnsure(); }
}
static void sendToOsd(const std::string& channel, const json& payload) {   // threadsicher wie sendToUi
    if (!g_osdHwnd) return;
    json m = { {"channel", channel}, {"payload", payload} };
    PostMessageW(g_osdHwnd, WM_SHELL_OSDMSG, 0, (LPARAM)new std::wstring(widen(m.dump())));
}
static void applyOsdConfig() {
    if (!g_osdWv) return;
    json s = loadSettings();
    double z = (std::max)(0.4, (std::min)(3.0, s.value("osdScale", 1.0)));
    if (g_osdCtrl) g_osdCtrl->put_ZoomFactor(z);
    sendToOsd("osd-config", { {"corner", s.value("osdCorner", "tl")}, {"opacity", s.value("osdOpacity", 0.55)},
                              {"theme", s.value("osdTheme", "compact")}, {"fields", s.value("osdFields", json())},
                              {"accent", s.value("osdAccent", "#74e857")} });
}
static ComPtr<IDCompositionDevice> g_dcompDev;
static ComPtr<IDCompositionTarget> g_dcompTarget;
static ComPtr<IDCompositionVisual> g_dcompVisual;
static ComPtr<ICoreWebView2CompositionController> g_osdComp;
static LRESULT CALLBACK osdWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Live-Edit: Maus-Ereignisse ans WebView2 durchreichen - der Composition-Modus
    // bekommt sie NICHT automatisch (SendMouseInput ist Pflicht, MS-Doku).
    if (g_osdEdit && g_osdComp && ((m >= WM_MOUSEMOVE && m <= WM_MBUTTONDBLCLK) || m == WM_MOUSEWHEEL || m == WM_MOUSELEAVE)) {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        UINT32 mouseData = 0;
        if (m == WM_MOUSEWHEEL) { mouseData = (UINT32)GET_WHEEL_DELTA_WPARAM(w); ScreenToClient(h, &pt); }   // Wheel liefert Screen-Koordinaten
        g_osdComp->SendMouseInput((COREWEBVIEW2_MOUSE_EVENT_KIND)m, (COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS)LOWORD(w), mouseData, pt);
        return 0;
    }
    switch (m) {
    // CLICK-THROUGH (ausser im Edit-Modus): WS_EX_TRANSPARENT wirkt OHNE WS_EX_LAYERED
    // nicht auf Hit-Tests (User-Befund: OSD blockierte Maus, Hand-Cursor). HTTRANSPARENT
    // reicht alle Maus-Ereignisse ans darunterliegende Fenster durch.
    case WM_NCHITTEST: return g_osdEdit ? HTCLIENT : HTTRANSPARENT;
    case WM_SETCURSOR: if (!g_osdEdit) return TRUE; SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
    case WM_SHELL_OSDMSG: { auto* s = (std::wstring*)l; if (s) { if (g_osdWv) g_osdWv->PostWebMessageAsJson(s->c_str()); delete s; } return 0; }
    case WM_SIZE: if (g_osdCtrl) { RECT rc; GetClientRect(h, &rc); g_osdCtrl->put_Bounds(rc); } return 0;
    case WM_DESTROY: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void createOsdWindow() {
    if (g_osdHwnd) return;
    static bool reg = false;
    if (!reg) { WNDCLASSW wc{}; wc.lpfnWndProc = osdWndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"LumoraOsd"; RegisterClassW(&wc); reg = true; }
    // Hauptmonitor-Flaeche minus 1px unten (bricht die Vollbild-Erkennung von Windows nicht).
    // WS_EX_NOREDIRECTIONBITMAP: keine eigene Redirection-Surface -> DirectComposition scheint
    // per-pixel-transparent durch (Voraussetzung fuer echte WebView2-Transparenz). KEIN
    // WS_EX_LAYERED (das killte das WebView2-Rendering); click-through via WS_EX_TRANSPARENT.
    HMONITOR hm = MonitorFromPoint({ 0,0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
    int w = mi.rcMonitor.right - mi.rcMonitor.left, hgt = mi.rcMonitor.bottom - mi.rcMonitor.top - 1;
    // Click-through zu FREMDEN Fenstern verlangt WS_EX_LAYERED|WS_EX_TRANSPARENT
    // (HTTRANSPARENT wirkt nur thread-intern - MS-Doku; User-Befund: nichts mehr
    // anklickbar). Mit WS_EX_NOREDIRECTIONBITMAP ist LAYERED unkritisch: es gibt
    // keine Redirection-Surface, SetLayeredWindowAttributes ENTFAELLT (dessen
    // Aufruf hatte frueher das HWND-Controller-Rendering gekillt) - der DComp-
    // Visual-Baum wird unabhaengig composited (Standard-Overlay-Kombi).
    g_osdHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"LumoraOsd", L"", WS_POPUP, mi.rcMonitor.left, mi.rcMonitor.top, w, hgt, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_osdHwnd) { bcLogStream("osd: CreateWindowExW fehlgeschlagen err=" + std::to_string(GetLastError())); return; }
    bcLogStream("osd: Overlay-Fenster erstellt (Composition)");
    wchar_t lad[MAX_PATH] = {}; GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    std::wstring userData = std::wstring(lad) + L"\\lumora-shell";
    CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
                ComPtr<ICoreWebView2Environment3> env3;
                if (FAILED(res) || !env || !g_osdHwnd || FAILED(env->QueryInterface(IID_PPV_ARGS(&env3))) || !env3) { bcLogStream("osd: Environment3 fehlt"); return res; }
                env3->CreateCoreWebView2CompositionController(g_osdHwnd,
                    Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                        [](HRESULT r2, ICoreWebView2CompositionController* comp) -> HRESULT {
                            if (FAILED(r2) || !comp || !g_osdHwnd) { bcLogStream("osd: CompositionController-Init " + std::to_string(r2)); return r2; }
                            g_osdComp = comp;
                            comp->QueryInterface(IID_PPV_ARGS(&g_osdCtrl));   // gleiches Objekt als ICoreWebView2Controller
                            if (!g_osdCtrl) return E_NOINTERFACE;
                            g_osdCtrl->get_CoreWebView2(&g_osdWv);
                            ComPtr<ICoreWebView2Controller2> c2; g_osdCtrl.As(&c2);
                            if (c2) { COREWEBVIEW2_COLOR clr{ 0, 0, 0, 0 }; c2->put_DefaultBackgroundColor(clr); }   // voll transparent (nur mit Composition!)
                            // DirectComposition-Baum: Device -> Target(hwnd) -> Visual(WebView2-Inhalt)
                            if (SUCCEEDED(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&g_dcompDev))) && g_dcompDev) {
                                g_dcompDev->CreateTargetForHwnd(g_osdHwnd, TRUE, &g_dcompTarget);
                                g_dcompDev->CreateVisual(&g_dcompVisual);
                                if (g_dcompVisual) { g_osdComp->put_RootVisualTarget(g_dcompVisual.Get());
                                    if (g_dcompTarget) g_dcompTarget->SetRoot(g_dcompVisual.Get());
                                    g_dcompDev->Commit(); }
                            } else bcLogStream("osd: DCompositionCreateDevice fehlgeschlagen");
                            RECT rc; GetClientRect(g_osdHwnd, &rc); g_osdCtrl->put_Bounds(rc);
                            g_osdCtrl->put_IsVisible(TRUE);
                            ComPtr<ICoreWebView2_3> wv3; g_osdWv.As(&wv3);
                            if (wv3) wv3->SetVirtualHostNameToFolderMapping(L"app.lumora", g_appDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            std::wstring shim = SHIM_JS; size_t vp = shim.find(L"%SHELL_VERSION%");
                            if (vp != std::wstring::npos) shim.replace(vp, 15, widen(shellVersion()));
                            g_osdWv->AddScriptToExecuteOnDocumentCreated(shim.c_str(), nullptr);
                            // FEHLTE: Nachrichten VOM OSD-WebView (osd-edit-*: Fertig/Theme/Ecke/
                            // Skalierung/Felder) empfangen und an handleChannel leiten - sonst war der
                            // Live-Edit tot ("Fertig" schloss nicht, Design liess sich nicht wechseln).
                            g_osdWv->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                    LPWSTR j = nullptr;
                                    if (SUCCEEDED(a->get_WebMessageAsJson(&j)) && j) { bcLogStream("osd-wv-msg: " + narrow(j)); onWebMessage(j, g_osdHwnd); CoTaskMemFree(j); }
                                    return S_OK;
                                }).Get(), nullptr);
                            g_osdWv->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT { g_osdLoaded = true; bcLogStream("osd: osd.html geladen"); applyOsdConfig(); return S_OK; }).Get(), nullptr);
                            g_osdWv->Navigate(L"https://app.lumora/osd.html");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}
static void showOsd() { createOsdWindow(); sensorsInit(); ensureOsdSetup(); brokersEnsure(); SetTimer(g_hwnd, TIMER_OSDDATA, 200, nullptr); SetTimer(g_hwnd, TIMER_OSDFPS, 33, nullptr); if (g_osdHwnd) { ShowWindow(g_osdHwnd, SW_SHOWNOACTIVATE); SetWindowPos(g_osdHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); if (g_osdLoaded) applyOsdConfig(); } }
static void hideOsd() { KillTimer(g_hwnd, TIMER_OSDDATA); KillTimer(g_hwnd, TIMER_OSDFPS); shmWriteApp(g_fpsShm, 0); shmWriteApp(g_senseShm, 0); if (g_osdHwnd) ShowWindow(g_osdHwnd, SW_HIDE); }
static void syncOsdVisibility() { if (loadSettings().value("osdEnabled", false)) showOsd(); else hideOsd(); }
// Live-Edit-Modus (Alt+Shift+O, wie Electrons setOsdEditMode): Overlay wird kurz
// interaktiv (Ecke ziehen, Mausrad = Groesse, Editbar-Buttons); "Fertig" schaltet zurueck.
static void setOsdEditMode(bool on) {
    if (on) {
        json s = loadSettings();
        if (!s.value("osdEnabled", false)) { s["osdEnabled"] = true; writeFile(settingsPath(), s.dump(2)); }
        showOsd();
        if (!g_osdHwnd) return;
        g_osdEdit = true;
        // Anklickbar machen: NUR TRANSPARENT+NOACTIVATE weg (LAYERED MUSS bleiben - es
        // traegt die DComp-Transparenz). OHNE SWP_FRAMECHANGED wird der Style-Wechsel
        // nicht wirksam -> Fenster faengt die Maus nicht -> KEIN Klick erreicht das
        // WebView (weder "Fertig" noch Design-Wahl; genau der User-Befund).
        LONG_PTR ex = GetWindowLongPtrW(g_osdHwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(g_osdHwnd, GWL_EXSTYLE, ex & ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE));
        SetWindowPos(g_osdHwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        SetForegroundWindow(g_osdHwnd);
        sendToOsd("osd-edit", true);
    } else {
        g_osdEdit = false;
        if (g_osdHwnd) {
            LONG_PTR ex = GetWindowLongPtrW(g_osdHwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(g_osdHwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        }
        sendToOsd("osd-edit", false);
    }
}
static void toggleOsdSetting() {   // wie Electrons toggleOverlay (Hotkey/Gamepad/Kanal)
    json s = loadSettings(); s["osdEnabled"] = !s.value("osdEnabled", false);
    writeFile(settingsPath(), s.dump(2));
    syncOsdVisibility();
    sendToUi("osd-config", nullptr);   // UI-Checkbox darf nachziehen (liest Settings neu)
}
// Poll-Hotkey-Liste aus den Settings bauen (1:1 aus main.js rebuildHotkeys). Wird
// von registerHotkeys aufgerufen (Start + nach jeder Hotkey-Aenderung).
static void rebuildKbHotkeys() {
    json s = loadSettings();
    std::vector<KbHotkey> list;
    auto add = [&](const std::string& accel, std::function<void()> action) {
        if (accel.empty()) return;
        UINT mods = 0, vk = 0;
        if (!parseAccelerator(accel, mods, vk)) return;
        list.push_back({ vk, (mods & MOD_ALT) != 0, (mods & MOD_CONTROL) != 0, (mods & MOD_SHIFT) != 0, action, false });
    };
    add(s.value("toggleHotkey", "Alt+L"), toggleMainWindow);
    add(s.value("osdHotkey", "Alt+O"), toggleOsdSetting);
    add(s.value("osdEditHotkey", "Alt+Shift+O"), []() { setOsdEditMode(!g_osdEdit); });
    add(s.value("osdAbHotkey", "Alt+B"), []() { lurtss::toggleAbOsd(); });
    add(s.value("streamHotkey", ""), []() { std::thread([]() { if (g_bcState.value("active", false)) stopBroadcast(); else startBroadcast(); }).detach(); });
    std::lock_guard<std::mutex> lk(g_kbMx);
    g_kbHotkeys = std::move(list);
}

// --- Auto-Update: NUR voller Installer, NIEMALS Datei-Update (Fleet-Desaster,
// [[file-updater-bug]]). Prueft einen JSON-Feed {version,url,notes,notesEn}; bei
// neuerer Version -> update-available; auf Wunsch Download + Start des Installers,
// der die vorhandene Version ueber den Umstiegs-/Parallel-Weg ersetzt.
static const char* UPDATE_FEED_DEFAULT = "https://lumora-streaming.de/native-update.json";
static std::atomic<bool> g_updBusy{ false };
static std::string g_updUrl, g_updFile;
static int cmpVer(const std::string& a, const std::string& b) {
    int x[3] = { 0,0,0 }, y[3] = { 0,0,0 };
    sscanf_s(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]); sscanf_s(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
static void checkForUpdates(bool manual) {
    std::thread([manual]() {
        std::string feed = loadSettings().value("updateFeed", std::string(UPDATE_FEED_DEFAULT));
        luart::HttpResp r = luart::httpGet(feed);
        if (r.status != 200) { if (manual) sendToUi("update-error", { {"message", "Update-Server nicht erreichbar"} }); return; }
        json j = json::parse(r.body, nullptr, false);
        std::string ver = j.is_object() ? j.value("version", "") : "";
        if (ver.empty()) { if (manual) sendToUi("update-none", nullptr); return; }
        if (cmpVer(shellVersion(), ver) < 0) {
            g_updUrl = j.value("url", "");
            sendToUi("update-available", { {"version", ver}, {"notes", j.value("notes", "")}, {"notesEn", j.value("notesEn", "")} });
        } else if (manual) sendToUi("update-none", nullptr);
    }).detach();
}
static void setupAutoUpdate() { std::thread([]() { Sleep(4000); checkForUpdates(false); }).detach(); }

// Zentraler Kanal-Dispatcher (Phase-2-Checkliste: capture-cpp/lumora-shell/IPC-INVENTAR.md).
// Unbekannte Kanaele -> null (Promise loest auf, UI haengt nicht).
static json handleChannel(const std::string& channel, const json& args) {
    if (channel == "check-for-updates") { checkForUpdates(true); return true; }
    if (channel == "download-update") {
        if (g_updBusy.load() || g_updUrl.empty()) return false;
        g_updBusy = true;
        std::thread([]() {
            luart::HttpResp r = luart::httpGet(g_updUrl, L"", 180000);   // Installer ist klein (~13 MB)
            if (r.status != 200 || r.body.empty()) { sendToUi("update-error", { {"message", "Download fehlgeschlagen"} }); g_updBusy = false; return; }
            wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
            g_updFile = narrow(tmp) + "Lumora-Native-Update.exe";
            HANDLE f = CreateFileW(widen(g_updFile).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(f, r.body.data(), (DWORD)r.body.size(), &w, nullptr); CloseHandle(f); }
            sendToUi("update-progress", { {"percent", 100} });
            sendToUi("update-ready", { {"version", ""} });
            g_updBusy = false;
        }).detach();
        return true;
    }
    if (channel == "install-update") {
        if (g_updFile.empty()) return false;
        ShellExecuteW(nullptr, L"open", widen(g_updFile).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        g_quitting = true; PostMessageW(g_hwnd, WM_CLOSE, 0, 0);   // App beenden, Installer laeuft
        return true;
    }
    if (channel == "get-app-settings") return loadSettings();
    if (channel == "set-app-settings") {                      // Merge wie Object.assign in main.js
        if (args.size() >= 1 && args[0].is_object()) {
            json s = loadSettings(); s.update(args[0]);
            writeFile(settingsPath(), s.dump(2));
            // Stream-Einstellung geaendert, waehrend gestreamt wird: Encoder-Parameter live
            // nachziehen (bcApplyCfg = nahtlose Bitrate/Quelle; Vollneustart nur fps/scaleH).
            // AUSGENOMMEN die Nicht-Encoder-Keys (Audit-Lehre: TURN/IPv6 rissen den Stream grundlos ab).
            static const std::set<std::string> noRestart = { "streamBufferMs", "streamAdaptive", "streamTurnEnabled",
                "streamTurnUrl", "streamTurnUser", "streamTurnPass", "streamTurnForce", "streamForceIPv6",
                "streamLastHosts", "streamHotkey", "streamFastRestart", "streamDoorman" };
            bool touch = false;
            for (auto& [k, v] : args[0].items()) if (k.rfind("stream", 0) == 0 && !noRestart.count(k)) touch = true;
            if (args[0].value("streamAdaptive", true) == false && g_adaptLevel > 0 && g_bcState.value("active", false)) {
                g_adaptLevel = 0; g_adaptUpAt = 0; g_adaptUpHold = 0; touch = true;   // zurueck zur vollen Bitrate
            }
            if (touch && g_bcState.value("active", false)) SetTimer(g_hwnd, TIMER_BCAPPLY, 500, nullptr);   // debounced
            // Systemseitige Einstellungen sofort anwenden (wie der Electron-Handler)
            if (args[0].contains("autostart") || args[0].contains("startMinimized")) applyAutostart();
            if (args[0].contains("minimizeToTray")) { if (args[0].value("minimizeToTray", false)) createTray(); else destroyTray(); }
            // OSD-Einstellung dabei? Overlay-Sichtbarkeit + Konfiguration live nachziehen (wie Electron)
            for (auto& [k, v] : args[0].items()) if (k.rfind("osd", 0) == 0) { syncOsdVisibility(); applyOsdConfig(); break; }
        }
        return true;
    }
    if (channel == "set-hotkey") {   // (accelerator, which) wie Electron; Rueckgabe {ok}
        std::string acc = args.size() >= 1 && args[0].is_string() ? args[0].get<std::string>() : "";
        std::string which = args.size() >= 2 && args[1].is_string() ? args[1].get<std::string>() : "";
        std::string key = which == "osd" ? "osdHotkey" : which == "osdEdit" ? "osdEditHotkey" : which == "osdAb" ? "osdAbHotkey" : which == "stream" ? "streamHotkey" : "toggleHotkey";
        json s = loadSettings(); s[key] = acc; writeFile(settingsPath(), s.dump(2));
        return { {"ok", registerHotkeys()} };
    }
    if (channel == "save-games" && args.size() >= 1 && args[0].is_string()) {   // UI liefert fertigen JSON-String
        writeFile(dataDir() + L"\\games.json", args[0].get<std::string>()); return true;
    }
    if (channel == "save-prefs" && args.size() >= 1 && args[0].is_string()) {
        writeFile(dataDir() + L"\\prefs.json", args[0].get<std::string>()); return true;
    }
    if (channel == "launch-game" && args.size() >= 1 && args[0].is_string()) return launchGame(args);
    if (channel == "open-game-folder" && args.size() >= 1 && args[0].is_string()) {
        std::wstring p = widen(args[0].get<std::string>());
        std::wstring param = L"/select,\"" + p + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (channel == "scan-games") return lushell::scanGames(args.size() >= 1 ? args[0] : json::array());   // Steam+Xbox+Ordner (weitere Stores folgen)
    // ---- Streaming (nativer Weg; V1 = LAN-Phase, Router-Phase folgt) ----
    if (channel == "start-broadcast") return startBroadcast();
    if (channel == "stop-broadcast") return stopBroadcast();
    if (channel == "broadcast-status") return g_bcState;
    if (channel == "list-viewers") {
        json out = json::array();
        for (auto& s : bcMtxReaders()) {
            std::string ip = bcExtractIp(s.value("remoteCandidate", ""));
            std::string name; std::string q = s.value("query", "");
            size_t np = q.find("name="); if (np != std::string::npos) { name = q.substr(np + 5); size_t amp = name.find('&'); if (amp != std::string::npos) name = name.substr(0, amp); }
            out.push_back({ {"id", s.value("id", "")}, {"ip", ip.empty() ? "(verbindet\xE2\x80\xA6)" : ip}, {"ua", bcUaShort(s.value("userAgent", ""))},
                            {"since", (long long)time(nullptr) * 1000}, {"bytes", s.value("bytesSent", 0ll)}, {"name", name} });
        }
        return out;
    }
    if (channel == "kick-viewer" && args.size() >= 1 && args[0].is_string()) {
        // (id, ip, block): bei block die IP fuer diese Sitzung sperren (wie Electron)
        if (args.size() >= 3 && args[2].is_boolean() && args[2].get<bool>() && args.size() >= 2 && args[1].is_string()) {
            std::string ip = args[1].get<std::string>();
            if (!ip.empty() && ip.find("verbindet") == std::string::npos) g_blockedIps.insert(ip);
        }
        auto r = lusrv::proxyLocal(MTX_API_PORT, "POST", "/v3/webrtcsessions/kick/" + args[0].get<std::string>(), "", "", "");
        return { {"ok", r.status >= 200 && r.status < 300} };
    }
    // ---- Tuersteher-Verwaltung (1:1) ----
    if (channel == "doorman-decide" && args.size() >= 2 && args[0].is_string() && args[1].is_string()) {
        std::string vid = args[0].get<std::string>(), mode = args[1].get<std::string>();
        if (vid.empty()) return false;
        std::string nm = "Gast";
        { std::lock_guard<std::mutex> lk(g_doorMx);
          auto it = g_knocks.find(vid);
          if (it != g_knocks.end() && !it->second.name.empty()) nm = it->second.name;
          if (mode == "allow") { if (it != g_knocks.end()) it->second.status = "granted"; }
          else if (mode == "deny") { if (it != g_knocks.end()) { it->second.status = "denied"; it->second.deniedAt = GetTickCount64(); } }
          else if (mode == "always" || mode == "ban") {
              json s = loadSettings();
              if (mode == "always") { s["streamRegulars"][vid] = { {"name", nm}, {"since", (long long)time(nullptr) * 1000} }; if (s.contains("streamBanned")) s["streamBanned"].erase(vid); if (it != g_knocks.end()) it->second.status = "granted"; }
              else { s["streamBanned"][vid] = { {"name", nm}, {"since", (long long)time(nullptr) * 1000} }; if (s.contains("streamRegulars")) s["streamRegulars"].erase(vid); if (it != g_knocks.end()) { it->second.status = "denied"; it->second.deniedAt = GetTickCount64(); } }
              writeFile(settingsPath(), s.dump(2));
          }
        }
        bcSyncDoorman();
        return true;
    }
    if (channel == "doorman-lists") {
        json s = loadSettings();
        auto toArr = [](const json& o) { json a = json::array();
            if (o.is_object()) for (auto& [vid, v] : o.items()) a.push_back({ {"vid", vid}, {"name", v.value("name", "Gast")}, {"since", v.value("since", 0ll)} });
            return a; };
        return { {"regulars", toArr(s.value("streamRegulars", json::object()))}, {"banned", toArr(s.value("streamBanned", json::object()))} };
    }
    if (channel == "doorman-remove" && args.size() >= 2 && args[0].is_string() && args[1].is_string()) {
        json s = loadSettings();
        std::string kind = args[0].get<std::string>(), vid = args[1].get<std::string>();
        if (kind == "regular" && s.contains("streamRegulars")) s["streamRegulars"].erase(vid);
        else if (kind == "banned" && s.contains("streamBanned")) s["streamBanned"].erase(vid);
        writeFile(settingsPath(), s.dump(2));
        return true;
    }
    if (channel == "preview-whep" && args.size() >= 1 && args[0].is_string()) {
        // Vorschau direkt an den eigenen /whep-Weg (User-Agent markiert -> nicht als Zuschauer gezaehlt)
        auto r = lusrv::proxyLocal(MTX_WHEP_PORT, "POST", std::string("/") + MTX_PATH + "/whep", "application/sdp", "LumoraPreview", args[0].get<std::string>());
        json out = { {"ok", r.status >= 200 && r.status < 300}, {"answer", r.body}, {"session", nullptr} };
        if (r.headers.count("Location")) {
            std::string loc = r.headers["Location"]; size_t h = loc.find("//");
            if (h != std::string::npos) { size_t sl = loc.find('/', h + 2); loc = sl == std::string::npos ? "/" : loc.substr(sl); }
            std::string pre = std::string("/") + MTX_PATH + "/whep"; size_t pp = loc.find(pre);
            if (pp != std::string::npos) loc = loc.substr(0, pp) + "/whep" + loc.substr(pp + pre.size());
            out["session"] = loc;
        }
        return out;
    }
    if (channel == "preview-whep-stop" && args.size() >= 1 && args[0].is_string()) {
        std::string sess = args[0].get<std::string>();   // '/whep/<id>' -> mediamtx-Pfad
        std::string pre = "/whep"; size_t pp = sess.find(pre);
        std::string target = std::string("/") + MTX_PATH + "/whep" + (pp != std::string::npos ? sess.substr(pp + pre.size()) : sess);
        lusrv::proxyLocal(MTX_WHEP_PORT, "DELETE", target, "", "", "");
        return true;
    }
    if (channel == "set-hdr-tonemap") {
        if (args.size() >= 1 && args[0].is_object()) {
            json s = loadSettings(); json u;
            if (args[0].contains("mode")) u["streamHdrMode"] = (std::max)(0, (std::min)(3, args[0].value("mode", 0)));
            if (args[0].contains("exposure")) u["streamHdrExposure"] = (std::max)(0.05, (std::min)(3.0, args[0].value("exposure", 0.3937)));
            s.update(u); writeFile(settingsPath(), s.dump(2));
            bcWriteHdrControl();
        }
        json s2 = loadSettings();
        return { {"mode", s2.value("streamHdrMode", 0)}, {"exposure", s2.value("streamHdrExposure", 0.3937)} };
    }
    if (channel == "list-sources") {
        // Monitore nativ (Label wie Electron: "Bildschirm N - WxH [· Haupt]") + Fenster via Helfer --list
        json out = json::array();
        struct MonCtx { json* out; int i; };
        MonCtx mc{ &out, 0 };
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = (MonCtx*)lp;
            MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
            std::string label = "Bildschirm " + std::to_string(c->i + 1) + " \xE2\x80\x93 " +
                std::to_string(mi.rcMonitor.right - mi.rcMonitor.left) + "\xC3\x97" + std::to_string(mi.rcMonitor.bottom - mi.rcMonitor.top);
            if (mi.dwFlags & MONITORINFOF_PRIMARY) label += " \xC2\xB7 Haupt";
            c->out->push_back({ {"value", "screen:" + std::to_string(c->i)}, {"label", label}, {"icon", ""}, {"kind", "screen"} });
            c->i++;
            return TRUE;
        }, (LPARAM)&mc);
        // Fenster: Helfer --list (Zeilen "hwnd\ttitel\ticonDataUrl")
        std::string raw = runCaptureOutput(L"\"" + binDir() + L"\\lumora-capture-native.exe\" --list", 15000);
        std::istringstream ls(raw); std::string line;
        while (std::getline(ls, line)) {
            while (!line.empty() && (line.back() == '\r')) line.pop_back();
            size_t t1 = line.find('\t'); if (t1 == std::string::npos) continue;
            size_t t2 = line.find('\t', t1 + 1);
            std::string hwnd = line.substr(0, t1);
            std::string title = t2 == std::string::npos ? line.substr(t1 + 1) : line.substr(t1 + 1, t2 - t1 - 1);
            std::string icon = t2 == std::string::npos ? "" : line.substr(t2 + 1);
            if (hwnd.empty() || title.empty()) continue;
            std::string tl = title; for (auto& c : tl) c = (char)tolower((unsigned char)c);
            if (tl == "lumora" || title == "Program Manager") continue;   // eigene App/Desktop ausblenden (wie Electron)
            // Helfer liefert ROHES PNG-Base64 - das data-URL-Praefix ergaenzt die App (User-Befund: Icons kaputt)
            out.push_back({ {"value", "window:" + hwnd}, {"label", title}, {"icon", icon.empty() ? "" : "data:image/png;base64," + icon}, {"kind", "window"} });
        }
        return out;
    }
    if (channel == "test-connectivity") return runConnectivityTest();
    // ---- Gruppe (gruppe.php-Vermittlung) ----
    if (channel == "group-start") {
        if (!g_group.is_null()) return groupPublicState();
        if (!g_bcState.value("active", false)) {   // Ein-Klick: Stream automatisch mit hochziehen
            startBroadcast();
            if (!g_bcState.value("active", false)) return { {"active", false}, {"error", "stream-start-failed"} };
        }
        for (int i = 0; i < 40 && g_bcState.value("opening", true); ++i) Sleep(500);   // Router-Phase abwarten (linkV4/V6!)
        json empty = json::object();
        json c = groupRelay("create", json::object(), &empty);
        if (!c.is_object() || !c.value("ok", false) || c.value("code", "").empty()) return { {"active", false}, {"error", "relay-unreachable"} };
        g_group = { {"code", c.value("code", "")}, {"members", json::array()}, {"relayFails", 0} };
        json self = groupSelfEntry();
        json r = groupRelay("update", { {"code", c.value("code", "")} }, &self);
        if (r.is_object() && r.value("ok", false)) g_group["members"] = r.value("members", json::array());
        json s = loadSettings(); s["groupLastCode"] = c.value("code", ""); writeFile(settingsPath(), s.dump(2));
        SetTimer(g_hwnd, TIMER_GROUP, 8000, nullptr);
        lulan::beaconStart(g_group.value("code", ""), groupDisplayName(), groupMemberId());   // eigene Gruppe im LAN bewerben
        groupPushState();
        g_bcState["link"] = bcShareUrl(); bcPushState();   // Teilen-Link = Grid-Link (alle Streams)
        return groupPublicState();
    }
    if (channel == "group-join" && args.size() >= 1 && args[0].is_string()) {
        if (!g_group.is_null()) return { {"active", false}, {"error", "already-in-group"} };
        std::string raw = args[0].get<std::string>(); for (auto& ch : raw) ch = (char)toupper((unsigned char)ch);
        std::smatch m; std::string code;
        if (std::regex_search(raw, m, std::regex("[?&]CODE=([A-Z2-9]{6})\\b"))) code = m[1];
        else if (std::regex_match(raw, m, std::regex("\\s*([A-Z2-9]{6})\\s*"))) code = m[1];
        if (code.empty()) return { {"active", false}, {"error", "bad-code"} };
        if (!g_bcState.value("active", false)) {
            startBroadcast();
            if (!g_bcState.value("active", false)) return { {"active", false}, {"error", "stream-start-failed"} };
        }
        for (int i = 0; i < 40 && g_bcState.value("opening", true); ++i) Sleep(500);
        json self = groupSelfEntry();
        json r = groupRelay("update", { {"code", code} }, &self);
        if (!r.is_object() || !r.value("ok", false)) return { {"active", false}, {"error", r.is_object() ? r.value("error", "relay-unreachable") : "relay-unreachable"} };
        g_group = { {"code", code}, {"members", r.value("members", json::array())}, {"relayFails", 0} };
        json s = loadSettings(); s["groupLastCode"] = code; writeFile(settingsPath(), s.dump(2));
        SetTimer(g_hwnd, TIMER_GROUP, 8000, nullptr);
        lulan::beaconStart(g_group.value("code", ""), groupDisplayName(), groupMemberId());   // eigene Gruppe im LAN bewerben
        groupPushState();
        g_bcState["link"] = bcShareUrl(); bcPushState();
        return groupPublicState();
    }
    if (channel == "group-leave") {
        if (g_group.is_null()) return groupPublicState();
        std::string code = g_group.value("code", "");
        KillTimer(g_hwnd, TIMER_GROUP); lulan::beaconStop();   // LAN-Beacon einstellen (weiter lauschen)
        g_group = nullptr;
        json leave = { {"id", groupMemberId()} };
        groupRelay("leave", { {"code", code} }, &leave);
        groupPushState();
        std::string su = bcShareUrl();
        if (!su.empty()) g_bcState["link"] = su; else if (!g_bcState.value("linkV4", "").empty()) g_bcState["link"] = g_bcState["linkV4"];
        bcPushState();
        return groupPublicState();
    }
    if (channel == "group-status") return groupPublicState();
    // ---- Artwork/Netz (laufen im Worker-Thread, s. onWebMessage) ----
    if (channel == "fetch-cover") {
        std::string name = args.size() >= 1 && args[0].is_string() ? args[0].get<std::string>() : "";
        std::string appId = args.size() >= 2 && args[1].is_string() ? args[1].get<std::string>() : "";
        json steam = luart::fetchCoverSteam(name, appId);
        if (steam.is_string()) return steam;
        return appId.empty() ? luart::fetchCoverMSStore(name) : json(nullptr);   // wie main.js: MS Store nur ohne feste appId
    }
    if (channel == "fetch-hero") {
        std::string name = args.size() >= 1 && args[0].is_string() ? args[0].get<std::string>() : "";
        std::string appId = args.size() >= 2 && args[1].is_string() ? args[1].get<std::string>() : "";
        return luart::fetchSteamHero(name, appId);
    }
    if (channel == "fetch-game-info") {
        std::string name = args.size() >= 1 && args[0].is_string() ? args[0].get<std::string>() : "";
        std::string appId = args.size() >= 2 && args[1].is_string() ? args[1].get<std::string>() : "";
        return luart::fetchGameInfo(name, appId);
    }
    if (channel == "fetch-image-url")
        return luart::fetchImageUrl(args.size() >= 1 && args[0].is_string() ? args[0].get<std::string>() : "");
    if (channel == "search-steam")
        return luart::searchSteamArt(args.size() >= 1 && args[0].is_string() ? args[0].get<std::string>() : "",
                                     args.size() >= 2 && args[1].is_string() && args[1] == "hero");
    if (channel == "search-msstore") {
        if (args.size() >= 2 && args[1].is_string() && args[1] == "hero") return json::array();   // MS Store hat keine Hero-Banner
        return luart::searchMsArt(args.size() >= 1 && args[0].is_string() ? args[0].get<std::string>() : "");
    }
    if (channel == "search-sgdb") {
        std::string key = loadSettings().value("steamGridDbKey", "");
        if (key.empty()) return json::array();
        return luart::sgdbArtwork(args.size() >= 1 && args[0].is_string() ? args[0].get<std::string>() : "",
                                  (args.size() >= 2 && args[1].is_string() && args[1] == "hero") ? "hero" : "cover", key);
    }
    if (channel == "store-media" && args.size() >= 1 && args[0].is_object()) {
        // {id, kind, dataUrl:"data:image/x;base64,..."} -> Datei im media-Ordner, alte Varianten weg,
        // eindeutiger Stempel-Name gegen Browser-Cache (wie main.js). Rueckgabe: absoluter Pfad.
        std::string id = args[0].value("id", ""), kind = args[0].value("kind", ""), durl = args[0].value("dataUrl", "");
        size_t c = durl.find(";base64,");
        if (id.empty() || kind.empty() || durl.rfind("data:image/", 0) != 0 || c == std::string::npos) return nullptr;
        std::string ext = durl.substr(11, c - 11);
        if (ext == "jpeg") ext = "jpg"; else if (ext == "x-icon") ext = "ico"; else if (ext == "svg+xml") ext = "svg";
        std::string b64 = durl.substr(c + 8);
        // Base64 -> Bytes
        static const auto dec = []() { std::array<int8_t, 256> t; t.fill(-1); const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; for (int i = 0; i < 64; ++i) t[(uint8_t)a[i]] = (int8_t)i; return t; }();
        std::string bytes; bytes.reserve(b64.size() * 3 / 4); uint32_t acc = 0; int bits = 0;
        for (char ch : b64) { int8_t v = dec[(uint8_t)ch]; if (v < 0) continue; acc = (acc << 6) | v; bits += 6; if (bits >= 8) { bits -= 8; bytes.push_back((char)((acc >> bits) & 0xFF)); } }
        std::wstring mdir = dataDir() + L"\\media";
        CreateDirectoryW(mdir.c_str(), nullptr);
        std::wstring wid = widen(id), wkind = widen(kind);
        WIN32_FIND_DATAW fd{}; HANDLE h = FindFirstFileW((mdir + L"\\" + wid + L"-" + wkind + L"*").c_str(), &fd);   // alte Varianten desselben Typs entfernen
        if (h != INVALID_HANDLE_VALUE) { do { std::wstring fn = fd.cFileName; if (fn.rfind(wid + L"-" + wkind + L".", 0) == 0 || fn.rfind(wid + L"-" + wkind + L"-", 0) == 0) DeleteFileW((mdir + L"\\" + fn).c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); }
        wchar_t stamp[32]; _ui64tow_s(GetTickCount64(), stamp, 32, 36);
        std::wstring file = mdir + L"\\" + wid + L"-" + wkind + L"-" + stamp + L"." + widen(ext);
        if (!writeFile(file, bytes)) return nullptr;
        return narrow(file);
    }
    if (channel == "delete-media" && args.size() >= 1 && args[0].is_string()) {
        std::wstring mdir = dataDir() + L"\\media", wid = widen(args[0].get<std::string>());
        WIN32_FIND_DATAW fd{}; HANDLE h = FindFirstFileW((mdir + L"\\" + wid + L"-*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { do { DeleteFileW((mdir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); }
        return true;
    }
    if (channel == "browse-game")        return pickPathDialog(L"Spiel auswaehlen", false, L"Spiele", L"*.exe;*.lnk");
    if (channel == "browse-icon")        return pickPathDialog(L"Icon auswaehlen (.exe oder .ico)", false, L"Icon-Dateien", L"*.ico;*.exe");
    if (channel == "browse-scan-folder") return pickPathDialog(L"Ordner scannen", true, nullptr, nullptr);
    if (channel == "get-file-icon" && args.size() >= 1 && args[0].is_string()) {
        std::wstring p = widen(args[0].get<std::string>());
        std::wstring plow = p; for (auto& c : plow) c = towlower(c);
        // 1) Steam-Spiel: echtes Store-Icon aus dem librarycache (kleinste 40-Hex-JPG, wie main.js)
        if (plow.find(L"steamapps") != std::wstring::npos) {
            std::string appId = lulaunch::steamAppIdForExe(p);
            if (!appId.empty()) {
                wchar_t sp[MAX_PATH] = {}; DWORD sz = sizeof(sp);
                if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, sp, &sz) == ERROR_SUCCESS) {
                    std::wstring dir = std::wstring(sp) + L"\\appcache\\librarycache\\" + widen(appId);
                    std::error_code ec; uintmax_t best = UINT64_MAX; std::filesystem::path bestP;
                    static const std::wregex hexjpg(LR"(^[0-9a-f]{40}\.jpg$)", std::regex::icase);
                    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                        if (!std::regex_match(e.path().filename().wstring(), hexjpg)) continue;
                        uintmax_t s2 = e.file_size(ec); if (ec) { ec.clear(); continue; }
                        if (s2 < best) { best = s2; bestP = e.path(); }
                    }
                    if (!bestP.empty()) {
                        std::string jpg = readFile(bestP.wstring());
                        if (!jpg.empty()) return "data:image/jpeg;base64," + b64encode((const uint8_t*)jpg.data(), jpg.size());
                    }
                }
            }
        }
        // 2) Exe-/Datei-Icon
        return fileIconDataUrl(p);
    }
    if (channel == "list-gpus") {   // auslesbare GPUs (NVML + ADL), Format {id,label} wie listGpus
        nvmlInitOnce(); luadl::setup();
        json out = json::array();
        for (size_t i = 0; i < g_nvml.devices.size(); ++i) out.push_back({ {"id", "nvml:" + std::to_string(i)}, {"label", g_nvml.devices[i].second} });
        for (auto& a : luadl::adapters()) {
            std::string nm = cleanAmdName(a.name);
            out.push_back({ {"id", "adl:" + std::to_string(a.idx)}, {"label", nm + (a.dedicated ? "" : " (Onboard)")} });
        }
        return out;
    }
    // ---- OSD (Overlay steht; Live-Justierung schreibt Settings + zieht sie sofort nach) ----
    // "Fertig" kommt OHNE Argument (osd.html: invoke('osd-edit-done')) -> MUSS vor den
    // args>=1-Guard, sonst wird es verschluckt und der Live-Edit laesst sich nicht
    // schliessen (genau der User-Befund: "Fertig hat keine Wirkung").
    if (channel == "osd-edit-done") { setOsdEditMode(false); return true; }
    if (channel.rfind("osd-edit-", 0) == 0 && args.size() >= 1) {
        json s = loadSettings();
        if (channel == "osd-edit-corner" && args[0].is_string()) s["osdCorner"] = args[0];
        else if (channel == "osd-edit-opacity" && args[0].is_number()) s["osdOpacity"] = args[0];
        else if (channel == "osd-edit-scale" && args[0].is_number()) {   // osd.html sendet ein DELTA (+1/-1), nicht den Wert
            double z = s.value("osdScale", 1.0) + (args[0].get<double>() > 0 ? 0.05 : -0.05);
            z = (std::max)(0.5, (std::min)(2.0, (double)(int)(z * 100 + 0.5) / 100.0));   // klemmen + auf 2 Dezimalen runden
            s["osdScale"] = z;
        }
        else if (channel == "osd-edit-theme" && args[0].is_string()) s["osdTheme"] = args[0];
        else if (channel == "osd-edit-fields") s["osdFields"] = args[0];
        writeFile(settingsPath(), s.dump(2));
        applyOsdConfig();
        return true;
    }
    if (channel == "setup-osd") {   // Einstellungen -> Overlay -> "OSD einrichten": Dialog+UAC sofort anbieten
        json s = loadSettings(); s["osdSetupDeclined"] = false; writeFile(settingsPath(), s.dump(2));
        ensureOsdSetup();   // zeigt nur, was wirklich fehlt (FPS-Task/PawnIO/Sensor-Task)
        return s;
    }
    if (channel == "osd-sources") {   // welche Quelle liefert GPU/CPU/FPS (1:1 aus main.js)
        nvmlInitOnce(); luadl::setup();
        json m = readMahm();
        bool mGpu = !m.is_null() && m.contains("gpuTemp");
        bool mCpu = !m.is_null() && m.contains("cpuTemp");
        json s = loadSettings();
        bool declined = s.value("osdSetupDeclined", false);
        std::string gpu = g_nvml.ok ? "NVIDIA-Treiber (NVML)"
            : luadl::available() ? "AMD-Treiber (ADL)"
            : mGpu ? "MSI Afterburner" : "keine erkannt";
        std::string cpu = mCpu ? "MSI Afterburner"
            : senseBrokerAlive() ? "PawnIO-Treiber"
            : (lubroker::cpuSensorModule() && pawnioInstalled() && senseTaskPresent()) ? "PawnIO-Treiber (startet mit dem OSD)"
            : declined ? "nicht eingerichtet (nur Last/RAM/Takt)"
            : "Einrichtung folgt beim OSD-Start (bis dahin Last/RAM/Takt)";
        std::string src = s.value("osdFpsSource", std::string("auto"));
        bool useRtss = (src == "rtss") ? true : (src == "presentmon") ? false : lurtss::available();
        std::string fps = useRtss ? "RTSS/Afterburner"
            : fpsTaskPresent() ? "PresentMon"
            : declined ? "nicht eingerichtet"
            : "PresentMon (Einrichtung folgt beim OSD-Start)";
        return { {"gpu", gpu}, {"cpu", cpu}, {"fps", fps} };
    }
    if (channel == "toggle-window") { toggleMainWindow(); return true; }
    if (channel == "toggle-osd") { toggleOsdSetting(); return true; }
    if (channel == "minimize-window") { ShowWindow(g_hwnd, SW_MINIMIZE); return true; }
    if (channel == "toggle-maximize") { ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE); return true; }
    if (channel == "close-window")    { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return true; }
    if (channel == "shell-open-external") {
        if (args.size() >= 1 && args[0].is_string()) {
            std::wstring u = widen(args[0].get<std::string>());
            if (u.rfind(L"http", 0) == 0) ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return true;
    }
    return nullptr;
}

// Netz-/Scan-Kanaele blockieren den UI-Thread nicht (in Electron waren sie async):
// sie laufen in einem Worker-Thread; die Antwort kommt per WM_SHELL_REPLY zurueck.
static bool isSlowChannel(const std::string& c) {
    return c == "scan-games" || c == "fetch-cover" || c == "fetch-hero" || c == "fetch-game-info" ||
           c == "fetch-image-url" || c == "search-steam" || c == "search-msstore" || c == "search-sgdb" ||
           c == "launch-game" ||   // HDR-Wartezeit (3s) + AUMID-Suche
           c == "start-broadcast" || c == "stop-broadcast" || c == "list-sources" ||   // Relay-Ready/Helfer-Lauf
           c == "list-viewers" || c == "kick-viewer" || c == "preview-whep" || c == "preview-whep-stop" ||
           c == "group-start" || c == "group-join" || c == "group-leave" || c == "group-status" ||
           c == "test-connectivity";   // STUN+UPnP-Proben ~10s   // Vermittlungs-Netz
}
static void onWebMessage(const std::wstring& raw, HWND replyWnd) {
    json msg = json::parse(narrow(raw), nullptr, false);
    if (!msg.is_object()) return;
    long long id = msg.value("id", 0ll);
    std::string channel = msg.value("channel", "");
    json args = msg.contains("args") && msg["args"].is_array() ? msg["args"] : json::array();
    // Antwort ans QUELL-Fenster (Haupt-UI / Doorman / OSD) ueber dessen Marshalling-Message
    UINT replyMsg = (replyWnd == g_doorHwnd) ? WM_SHELL_DOORMSG : (replyWnd == g_osdHwnd) ? WM_SHELL_OSDMSG : WM_SHELL_REPLY;
    if (isSlowChannel(channel)) {
        std::thread([id, channel, args, replyWnd, replyMsg]() {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // ShellLink/AppsFolder im Worker
            json result = nullptr;
            try { result = handleChannel(channel, args); } catch (...) {}
            json resp = { {"id", id}, {"result", result} };
            PostMessageW(replyWnd, replyMsg, 0, (LPARAM)new std::wstring(widen(resp.dump())));
            CoUninitialize();
        }).detach();
        return;
    }
    json result = nullptr;
    try { result = handleChannel(channel, args); } catch (...) {}
    if (id > 0) {
        json resp = { {"id", id}, {"result", result} };
        PostMessageW(replyWnd, replyMsg, 0, (LPARAM)new std::wstring(widen(resp.dump())));
    }
}

// ---- Rahmenloses Fenster (wie Electrons frame:false, Titlebar/Knoepfe kommen aus dem UI) ----
// Drag laeuft ueber -webkit-app-region:drag (styles.css) via WebView2-NonClientRegionSupport;
// Resize ueber einen schmalen Host-Randstreifen (WebView2-Bounds leicht eingerueckt, Farbe =
// App-Hintergrund #0f0f0f -> unsichtbar). window-state.json wie in Electron persistiert.
static const int GRIP = 6;      // Seiten/unten
static const int GRIP_TOP = 3;  // oben schmaler (darueber beginnt die Drag-Titlebar im UI)

static std::wstring windowStatePath() { return dataDir() + L"\\window-state.json"; }
// In DIP-Koordinaten speichern (wie Electron) - sonst laufen die Werte bei DPI-Skalierung
// zwischen Shell und Electron-App auseinander (gleiche Datei, Parallelbetrieb).
static void saveWindowState(HWND h) {
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (!GetWindowPlacement(h, &wp)) return;
    RECT& r = wp.rcNormalPosition;
    UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
    json s = { {"x", MulDiv(r.left, 96, dpi)}, {"y", MulDiv(r.top, 96, dpi)},
               {"width", MulDiv(r.right - r.left, 96, dpi)}, {"height", MulDiv(r.bottom - r.top, 96, dpi)},
               {"maximized", wp.showCmd == SW_SHOWMAXIMIZED} };
    writeFile(windowStatePath(), s.dump(2));
}
static void placeWebView(HWND h) {
    if (!g_controller) return;
    RECT rc; GetClientRect(h, &rc);
    if (!IsZoomed(h)) { rc.left += GRIP; rc.right -= GRIP; rc.top += GRIP_TOP; rc.bottom -= GRIP; }
    g_controller->put_Bounds(rc);
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCALCSIZE: {   // BEIDE wParam-Faelle behandeln: beim initialen Fensteraufbau kommt
        auto* pr = (RECT*)l;   // FALSE - unbehandelt blieb da die System-Titelleiste stehen (Start im Fenster-Modus)
        if (IsZoomed(h)) {     // maximiert die unsichtbaren Frame-Insets abziehen, sonst ragt das Fenster ueber den Monitor
            int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            pr->left += fx; pr->right -= fx; pr->top += fy; pr->bottom -= fy;
        }
        return 0;
    }
    case WM_NCHITTEST: {   // Resize-Griffe auf dem Host-Randstreifen (der Rest gehoert dem WebView2)
        LRESULT def = DefWindowProcW(h, m, w, l);
        if (def != HTCLIENT || IsZoomed(h)) return def;
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        RECT rc; GetClientRect(h, &rc);
        bool L = p.x < GRIP, R = p.x >= rc.right - GRIP, T = p.y < GRIP, B = p.y >= rc.bottom - GRIP;
        if (T && L) return HTTOPLEFT; if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT; if (B && R) return HTBOTTOMRIGHT;
        if (T) return HTTOP; if (B) return HTBOTTOM; if (L) return HTLEFT; if (R) return HTRIGHT;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {   // wie Electron: minWidth 700 / minHeight 500 (DPI-skaliert)
        auto* mmi = (MINMAXINFO*)l;
        UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
        mmi->ptMinTrackSize.x = MulDiv(700, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(500, dpi, 96);
        return 0;
    }
    case WM_SIZE:
        placeWebView(h);
        if (g_webview) {   // UI wechselt damit das Maximieren-/Wiederherstellen-Icon
            if (w == SIZE_MAXIMIZED) sendToUi("window-maximized", nullptr);
            else if (w == SIZE_RESTORED) sendToUi("window-unmaximized", nullptr);
        }
        return 0;
    case WM_SETFOCUS:
        // Fenster-Fokus IMMER ins WebView2 weiterreichen: Chromium liefert Gamepad-Input
        // nur bei Dokument-Fokus. Deckt App-Start + spaeter eingeschaltete Controller ab
        // (der Hotkey-Pfad macht es zusaetzlich selbst in showMainWindow).
        if (g_controller) g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_SHELL_DOORSYNC:
        createDoormanWindow();
        bcSyncDoorman();
        return 0;
    case WM_SHELL_REPLY: {   // fertige Worker-Antwort im UI-Thread an das WebView2 posten
        auto* s = (std::wstring*)l;
        if (s) { if (g_webview) g_webview->PostWebMessageAsJson(s->c_str()); delete s; }
        return 0;
    }
    case WM_TIMER:
        if (w == TIMER_LAUNCH) { launchTick(); return 0; }
        if (w == TIMER_EXTWATCH) { extWatchTick(); return 0; }
        if (w == TIMER_VIEWER) { bcViewerTick(); return 0; }
        if (w == TIMER_ADAPT) { bcAdaptTick(); return 0; }
        if (w == TIMER_IPWATCH) { bcIpWatchTick(); return 0; }
        if (w == TIMER_BCAPPLY) { KillTimer(h, TIMER_BCAPPLY); if (g_bcState.value("active", false)) bcApplyCfg(bcStreamCfg(g_bcState.value("encoder", ""))); return 0; }
        if (w == TIMER_XINPUT) { xinputTick(); return 0; }
        if (w == TIMER_LANPUSH) {   // im LAN entdeckte Gruppen an die UI (veraltete raus)
            json arr = json::array();
            for (auto& [code, name] : lulan::groups()) arr.push_back({ {"code", code}, {"name", name} });
            sendToUi("lan-groups", arr);
            return 0;
        }
        if (w == TIMER_OSDDATA) { osdDataTick(); return 0; }
        if (w == TIMER_OSDFPS) { osdFpsTick(); return 0; }
        if (w == TIMER_GROUP) {   // Heartbeat im Worker (Netz, 8s-Timeout) - nie doppelt
            if (!g_groupTickBusy.exchange(true)) std::thread([]() { groupTickOnce(); g_groupTickBusy = false; }).detach();
            return 0;
        }
        if (w == TIMER_WATCH) {   // Einzelstream-Registrierung auffrischen
            if (!g_watchCode.empty()) std::thread([]() { json self = groupSelfEntry(); groupRelay("update", { {"code", g_watchCode} }, &self); }).detach();
            return 0;
        }
        break;
    case WM_SHELL_TRAY:
        if (LOWORD(l) == NIN_BALLOONUSERCLICK) { showMainWindow(); sendToUi("show-forward-help", nullptr); return 0; }   // Portfreigabe-Balloon geklickt
        if (LOWORD(l) == WM_LBUTTONUP) { toggleMainWindow(); return 0; }
        if (LOWORD(l) == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Öffnen");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, L"Beenden");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(h);   // Pflicht, sonst schliesst das Menue nicht (MS-Doku)
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) showMainWindow();
            else if (cmd == 2) { g_quitting = true; PostMessageW(h, WM_CLOSE, 0, 0); }
            return 0;
        }
        return 0;
    case WM_HOTKEY:
        if (w == HK_TOGGLE) { toggleMainWindow(); return 0; }
        if (w == HK_OSD) { toggleOsdSetting(); return 0; }
        if (w == HK_OSDEDIT) { setOsdEditMode(!g_osdEdit); return 0; }
        if (w == HK_OSDAB) { lurtss::toggleAbOsd(); return 0; }   // Alt+B: Afterburner-OSD (RTSS) toggeln
        if (w == HK_STREAM) {   // Stream-Hotkey: an/aus (im Worker, blockiert den Hotkey nicht)
            std::thread([]() { if (g_bcState.value("active", false)) stopBroadcast(); else startBroadcast(); }).detach();
            return 0;
        }
        break;
    case WM_COPYDATA: {   // Zweitinstanz reicht ihre Kommandozeile durch (Deep-Link/Fokus)
        auto* cds = (COPYDATASTRUCT*)l;
        std::string arg = cds && cds->lpData ? std::string((const char*)cds->lpData, cds->cbData) : "";
        if (arg.find("lumora://") != std::string::npos) handleDeepLink(arg.substr(arg.find("lumora://")));
        else if (arg.find("--minimized") == std::string::npos) showMainWindow();   // wie Electron: --minimized-Zweitstart holt NICHT nach vorne
        return TRUE;
    }
    case WM_CLOSE:
        if (loadSettings().value("minimizeToTray", false) && !g_quitting) { ShowWindow(h, SW_HIDE); return 0; }   // in den Infobereich statt beenden
        if (g_bcState.value("active", false)) stopBroadcast();   // Stream + Kindprozesse sauber beenden
        if (!g_group.is_null()) {   // fullTeardown: Gruppe beim App-Ende sauber verlassen (nicht serverseitig haengen lassen)
            std::string code = g_group.value("code", ""); json leave = { {"id", groupMemberId()} };
            groupRelay("leave", { {"code", code} }, &leave); g_group = nullptr;
        }
        destroyTray();
        saveWindowState(h); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    // BROKER-MODI (elevated, via geplante Aufgabe, KEIN Fenster): PresentMon-FPS bzw.
    // PawnIO-Sensoren sammeln und ins Shared Memory schreiben (wie Electrons Lumora.exe
    // --fps-broker/--sensor-broker). Muss VOR jeder Fenster-/COM-Init laufen.
    {
        int ac = 0; LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &ac);
        wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring bd = exe; bd = bd.substr(0, bd.find_last_of(L'\\')) + L"\\bin";
        if (GetFileAttributesW((bd + L"\\PresentMon.exe").c_str()) == INVALID_FILE_ATTRIBUTES) bd = bd.substr(0, bd.find_last_of(L'\\'));   // Dev: exe-Ordner
        for (int i = 1; i < ac; ++i) {
            if (wcscmp(av[i], L"--fps-broker") == 0) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); return lubroker::runFpsBroker(bd); }
            if (wcscmp(av[i], L"--sensor-broker") == 0) return lubroker::runSensorBroker(bd);   // PawnIO CPU-Temp/-Power
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // COM (FileDialogs, WebView2) - GUI-Thread = STA
    Gdiplus::GdiplusStartupInput gsi; ULONG_PTR gtok = 0; Gdiplus::GdiplusStartup(&gtok, &gsi, nullptr);   // Datei-Icons (PNG)
    { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }   // Sockets frueh (bcLanIp/mtx-Probe/SSDP laufen VOR dem HTTP-Server)
    // Single-Instance (wie Electron requestSingleInstanceLock): Zweitstart reicht seine
    // Kommandozeile per WM_COPYDATA an die erste Instanz durch (Deep-Link/Fokus) und endet.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LumoraShellSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"LumoraShell", nullptr);
        if (other) {
            std::string cmd; int ac = 0; LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &ac);
            for (int i = 1; i < ac; ++i) { if (i > 1) cmd += " "; int n = WideCharToMultiByte(CP_UTF8, 0, av[i], -1, nullptr, 0, nullptr, nullptr); std::string a(n ? n - 1 : 0, 0); WideCharToMultiByte(CP_UTF8, 0, av[i], -1, &a[0], n, nullptr, nullptr); cmd += a; }
            COPYDATASTRUCT cds{ 1, (DWORD)cmd.size(), (void*)cmd.data() };
            SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cds);
        }
        return 0;
    }
    // App-Ordner (index.html) finden: --appdir > aktuelles Verzeichnis > exe-Ordner >
    // von dort aufwaerts (Doppelklick aus build/Release im Quellbaum). Im spaeteren
    // Installer liegt das UI direkt neben der exe - dann greift Stufe 3 sofort.
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // Selbsttest ohne UI: Settings laden + Merge IN-MEMORY (echte Datei bleibt unberuehrt),
    // Ergebnis nach %TEMP%\lumora-shell-test.txt - automatisierte Verifikation des Dispatchers.
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-sensors") == 0) {
        // Sensor-Teil 1 am echten System: 2 Samples (Delta-Zaehler), dann Werte.
        sensorsInit(); shmOpen(g_fpsShm, "Local\\LumoraOSDFps"); shmOpen(g_senseShm, "Local\\LumoraOSDSense"); brokersEnsure();
        PdhCollectQueryData(g_pdhQ); Sleep(1100);
        for (int w = 0; w < 8; ++w) { shmWriteApp(g_fpsShm, 1); shmWriteApp(g_senseShm, 1); Sleep(1000); }   // Broker hochkommen lassen
        PdhCollectQueryData(g_pdhQ);   // FRISCHES Sample direkt vor dem Lesen (wie osdDataTick)
        json cpu = readCpuNative(), gpu = readGpuNative();
        json fps = readBrokerFps(), sense = readSenseCpu();
        for (int w = 0; w < 12 && fps.is_null(); ++w) { Sleep(1000); shmWriteApp(g_fpsShm, 1); shmWriteApp(g_senseShm, 1); fps = readBrokerFps(); if (sense.is_null()) sense = readSenseCpu(); }
        FpsShmFull rawF{}; bool rOk = shmRead(g_fpsShm, rawF);
        std::string s = "cpu=" + cpu.dump() + "\ngpu=" + gpu.dump() + "\nfps=" + fps.dump() + " sense=" + sense.dump() +
            "\nshm: h=" + std::to_string(g_fpsShm.h != nullptr) + " read=" + std::to_string(rOk) + " gle=" + std::to_string(GetLastError()) +
            " magic=" + std::to_string(rawF.magic) + " brokerTick=" + std::to_string(rawF.brokerTick) + " fps=" + std::to_string(rawF.fps) + " tick=" + std::to_string(GetTickCount()) + "\n";
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        writeFile(std::wstring(tmp) + L"\\lumora-shell-test.txt", s);
        return 0;
    }
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-conn") == 0) {
        // Verbindungstest am ECHTEN Anschluss (STUN + UPnP-Proben + IPv6-Pinhole).
        json r = runConnectivityTest();
        std::string s = "verdict=" + r.value("verdict", "?") + " ip=" + r.value("publicIp", "?") + "\n";
        for (auto& st : r.value("steps", json::array()))
            s += "  [" + st.value("state", "?") + "] " + st.value("label", "?") + ": " + st.value("detail", "").substr(0, 90) + "\n";
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        writeFile(std::wstring(tmp) + L"\\lumora-shell-test.txt", s);
        return 0;
    }
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-doorman") == 0) {
        // HMAC gegen den bekannten RFC-Testvektor + Gate-Logik pruefen. Einzigartige vid je
        // Lauf; der ban-Eintrag wird am Ende wieder aus den ECHTEN Settings entfernt.
        std::string testVid = "testvid-" + std::to_string(GetTickCount64());
        std::string mac = hmacSha256Hex("key", "The quick brown fox jumps over the lazy dog");
        bool macOk = mac == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8";
        std::string g1 = bcApproveGate(testVid, "");        // ohne Name -> pending, KEIN Eintrag
        std::string g2 = bcApproveGate(testVid, "Karsten"); // mit Name -> pending + Anfrage
        json dec = handleChannel("doorman-decide", json::array({ testVid, "allow" }));
        std::string g3 = bcApproveGate(testVid, "Karsten"); // nach allow -> granted
        json dec2 = handleChannel("doorman-decide", json::array({ testVid, "ban" }));
        std::string g4 = bcApproveGate(testVid, "Karsten"); // nach ban -> banned
        handleChannel("doorman-remove", json::array({ "banned", testVid }));   // Settings sauber hinterlassen
        std::string s = std::string("hmac=") + (macOk ? "OK" : ("FALSCH:" + mac)) +
            " gate:" + g1 + "/" + g2 + "/" + g3 + "/" + g4 + " (erwartet pending/pending/granted/banned)\n";
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        writeFile(std::wstring(tmp) + L"\\lumora-shell-test.txt", s);
        return 0;
    }
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-upnp") == 0) {
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
        std::string s;
        ULONGLONG t0 = GetTickCount64();
        auto locs = luupnp::discover(3000);
        s += "discover: " + std::to_string(locs.size()) + " IGDs in " + std::to_string(GetTickCount64() - t0) + "ms";
        for (auto& l : locs) s += " [" + l + "]";
        s += "\n";
        std::string ip = luupnp::getExternalIp();
        s += "externalIp: " + (ip.empty() ? "FEHLT" : ip) + "\n";
        std::string v6 = luupnp::publicIPv6();
        s += "publicIPv6: " + (v6.empty() ? "FEHLT" : v6) + "\n";
        bool m = luupnp::mapPort(48787, "TCP", "Lumora UPnP-Test");
        s += "mapPort(48787): " + std::to_string(m) + "\n";
        if (m) { luupnp::unmapPort(48787, "TCP"); s += "unmap: ok\n"; }
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        writeFile(std::wstring(tmp) + L"\\lumora-shell-test.txt", s);
        return 0;
    }
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-broadcast") == 0) {
        // E2E-Selbsttest: ECHTER Stream-Start (Relay + nativer Helfer) -> mediamtx-API
        // muss den Pfad mit H264+Opus 'ready' melden -> sauber stoppen.
        wchar_t cwd[MAX_PATH] = {}; GetCurrentDirectoryW(MAX_PATH, cwd); g_appDir = cwd;
        json st = startBroadcast();
        Sleep(9000);   // Router-Phase (SSDP 3s + SOAP) abwarten
        std::string s = "start: active=" + std::to_string(st.value("active", false)) + " enc=" + st.value("encoder", "?") + " link=" + st.value("lanLink", "?") + "\n";
        Sleep(5000);
        auto r = luart::httpGet("http://127.0.0.1:" + std::to_string(MTX_API_PORT) + "/v3/paths/get/" + MTX_PATH);
        json j = json::parse(r.body, nullptr, false);
        s += "paths/get: status=" + std::to_string(r.status) + " ready=" + (j.is_object() ? std::to_string(j.value("ready", false)) : "?") +
             " tracks=" + (j.is_object() && j.contains("tracks") ? j["tracks"].dump() : "?") + "\n";
        auto ply = luart::httpGet("http://127.0.0.1:8787/");
        s += "player: " + std::to_string(ply.status) + " " + std::to_string(ply.body.size()) + "B\n";
        stopBroadcast();
        s += "stop: ok\n";
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        writeFile(std::wstring(tmp) + L"\\lumora-shell-test.txt", s);
        return 0;
    }
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-http") == 0) {
        // Selbsttest: Streaming-Server auf 8787 starten + per Selbstabfrage verifizieren.
        wchar_t cwd[MAX_PATH] = {}; GetCurrentDirectoryW(MAX_PATH, cwd); g_appDir = cwd;   // player.html im Projektroot
        std::string s;
        if (g_streamSrv.start(BROADCAST_PORT, handleStreamHttp)) {
            auto inst = luart::httpGet("http://127.0.0.1:8787/instanz");
            auto cfg = luart::httpGet("http://127.0.0.1:8787/cfg");
            auto ply = luart::httpGet("http://127.0.0.1:8787/");
            auto nf = luart::httpGet("http://127.0.0.1:8787/gibtsnicht");
            s = "instanz=" + std::to_string(inst.status) + ":" + inst.body +
                "\ncfg=" + std::to_string(cfg.status) + ":" + cfg.body.substr(0, 120) +
                "\nplayer=" + std::to_string(ply.status) + " " + std::to_string(ply.body.size()) + "B" +
                "\n404=" + std::to_string(nf.status) + "\n";
            g_streamSrv.stop();
        } else s = "SERVER-START FEHLGESCHLAGEN (Port belegt? Electron-Lumora streamt gerade?)\n";
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        writeFile(std::wstring(tmp) + L"\\lumora-shell-test.txt", s);
        return 0;
    }
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-launch") == 0) {
        // Selbsttest der Spielstart-Bausteine mit ECHTEN Pfaden dieses Systems.
        std::string aid = lulaunch::steamAppIdForExe(L"c:/program files (x86)/steam\\steamapps\\common\\Teardown\\teardown.exe");
        std::wstring aumid = lulaunch::xboxAumidForGame(L"C:\\XboxGames\\Forza Horizon 6\\Content\\forzahorizon6.exe");
        bool pn = lulaunch::processByName(L"explorer.exe");
        bool pf = lulaunch::anyProcessInFolder(L"C:\\Windows");
        int sr = lulaunch::steamAppRunning("1167630");
        std::string s = "steamAppId(Teardown)=" + (aid.empty() ? "FEHLT" : aid) +
            " aumid(FH6)=" + (aumid.empty() ? "FEHLT" : narrow(aumid)) +
            " procByName(explorer)=" + (pn ? "1" : "0") + " procInFolder(Windows)=" + (pf ? "1" : "0") +
            " steamRunning=" + std::to_string(sr) + "\n";
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        writeFile(std::wstring(tmp) + L"\\lumora-shell-test.txt", s);
        return 0;
    }
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-art") == 0) {
        // Selbsttest: echte Netz-Kette (Steam-Suche -> AppId -> GetItems -> Cover-Download).
        std::string id = luart::resolveSteamAppId("Teardown");
        json cover = luart::fetchCoverSteam("Teardown", "");
        json info = luart::fetchGameInfo("Teardown", "");
        std::string s = "appId=" + (id.empty() ? "FEHLT" : id) +
            " cover=" + (cover.is_string() ? std::to_string(cover.get<std::string>().size()) + "B" : "FEHLT") +
            " info=" + (info.is_object() ? info.value("releaseYear", "?") + "/" + info.value("developer", "?") : "FEHLT") + "\n";
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        writeFile(std::wstring(tmp) + L"\\lumora-shell-test.txt", s);
        return 0;
    }
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-scan") == 0) {
        // Selbsttest: echten Steam+Xbox-Scan laufen lassen, Anzahl + Beispiele in die Testdatei.
        json r = lushell::scanGames(json::array());
        std::string per = "steam=" + std::to_string(lushell::scanSteam().size()) + " epic=" + std::to_string(lushell::scanEpic().size()) +
            " gog=" + std::to_string(lushell::scanGOG().size()) + " ubi=" + std::to_string(lushell::scanUbisoft().size()) +
            " xbox=" + std::to_string(lushell::scanXbox().size()) + " ea=" + std::to_string(lushell::scanEA().size()) +
            " rock=" + std::to_string(lushell::scanRockstar().size()) + " bnet=" + std::to_string(lushell::scanBattleNet().size()) +
            " amzn=" + std::to_string(lushell::scanAmazon().size()) + " riot=" + std::to_string(lushell::scanRiot().size()) + "\n";
        std::string s = per + "gefunden=" + std::to_string(r.size()) + "\n";
        for (size_t k = 0; k < r.size() && k < 40; ++k) s += "  " + r[k].value("source", "?") + ": " + r[k].value("name", "?") + " -> " + r[k].value("path", "?") + "\n";
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        writeFile(std::wstring(tmp) + L"\\lumora-shell-test.txt", s);
        return 0;
    }
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-ipc") == 0) {
        json s = handleChannel("get-app-settings", json::array());
        json m = s; m.update(json{ {"__testfeld", 42} });
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        writeFile(std::wstring(tmp) + L"\\lumora-shell-test.txt",
            "settings_keys=" + std::to_string(s.size()) + " merged_keys=" + std::to_string(m.size()) +
            " testfeld=" + std::to_string(m.value("__testfeld", 0)) + "\n");
        return 0;
    }
    auto hasIndex = [](const std::wstring& d) { return GetFileAttributesW((d + L"\\index.html").c_str()) != INVALID_FILE_ATTRIBUTES; };
    g_appDir.clear();
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--appdir") == 0 && i + 1 < argc) g_appDir = argv[i + 1];
    if (g_appDir.empty() || !hasIndex(g_appDir)) {
        wchar_t cwd[MAX_PATH] = {}; GetCurrentDirectoryW(MAX_PATH, cwd);
        if (hasIndex(cwd)) g_appDir = cwd;
        else {
            wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring d = exe; d = d.substr(0, d.find_last_of(L'\\'));
            for (int up = 0; up < 6 && !d.empty(); ++up) {
                if (hasIndex(d)) { g_appDir = d; break; }
                size_t p = d.find_last_of(L'\\'); if (p == std::wstring::npos) break; d = d.substr(0, p);
            }
        }
    }
    if (g_appDir.empty() || !hasIndex(g_appDir)) {
        MessageBoxW(nullptr, L"index.html nicht gefunden.\nMit --appdir <Ordner> starten.", L"Lumora Shell", MB_ICONERROR); return 1;
    }

    WNDCLASSW wc{}; wc.lpfnWndProc = wndProc; wc.hInstance = hInst; wc.lpszClassName = L"LumoraShell";
    wc.hIcon = LoadIconW(hInst, L"IDI_ICON1"); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(15, 15, 15));   // #0f0f0f wie Electron (auch Farbe der Resize-Randstreifen)
    RegisterClassW(&wc);
    // Fensterzustand wie Electron aus window-state.json (Position nur, wenn noch auf einem Monitor sichtbar)
    json st = json::parse(readFile(windowStatePath()), nullptr, false);
    if (!st.is_object()) st = json::object();
    UINT sdpi = GetDpiForSystem(); if (!sdpi) sdpi = 96;   // DIP -> physisch (Electron-kompatible Datei)
    int wwidth = MulDiv(st.value("width", 900), sdpi, 96), wheight = MulDiv(st.value("height", 600), sdpi, 96);
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT;
    if (st.contains("x") && st.contains("y")) {
        RECT tr{ MulDiv(st["x"].get<int>(), sdpi, 96), MulDiv(st["y"].get<int>(), sdpi, 96), 0, 0 };
        tr.right = tr.left + wwidth; tr.bottom = tr.top + wheight;
        if (MonitorFromRect(&tr, MONITOR_DEFAULTTONULL)) { wx = tr.left; wy = tr.top; }
    }
    HWND hwnd = CreateWindowExW(0, L"LumoraShell", L"Lumora", WS_OVERLAPPEDWINDOW,
        wx, wy, wwidth, wheight, nullptr, nullptr, hInst, nullptr);
    g_hwnd = hwnd;
    // Frame-Neuberechnung erzwingen (WM_NCCALCSIZE greift sonst erst bei der naechsten Groessenaenderung)
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    // --minimized (Autostart): versteckt starten (Tray uebernimmt); sonst normal/maximiert.
    bool startMin = false;
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--minimized") == 0) startMin = true;
    if (startMin) createTray();
    else ShowWindow(hwnd, st.value("maximized", false) ? SW_MAXIMIZE : nShow);
    if (loadSettings().value("minimizeToTray", false)) createTray();
    registerHotkeys();
    applyAutostart();
    registerProtocol();   // lumora://-Handler auf die native exe (Umstieg von Electron)
    setupAutoUpdate();    // 4 s nach Start still nach einer neueren Version schauen
    // Deep-Link als Startargument (lumora://...) direkt verarbeiten
    for (int i = 1; i < argc; ++i) { std::string a = narrow(argv[i]); if (a.rfind("lumora://", 0) == 0) handleDeepLink(a); }
    SetTimer(hwnd, TIMER_EXTWATCH, 2000, nullptr);   // Fremdstart-Watcher (HDR-Automatik + Spielzeit fuer nicht-Lumora-Starts)
    timeBeginPeriod(1);                              // praeziser Hintergrund-Poll (wie Electron/RTSS)
    SetTimer(hwnd, TIMER_XINPUT, 40, nullptr);       // nativer Gamepad-Hotkey-Poll (25 Hz)
    lulan::listenStart();                            // LAN-Beacon-Empfang ab Start (fremde Gruppen sehen)
    SetTimer(hwnd, TIMER_LANPUSH, 5000, nullptr);    // im LAN sichtbare Gruppen an die UI
    syncOsdVisibility();   // OSD-Overlay gemaess Einstellung anzeigen

    // WebView2-Umgebung (System-Runtime; UserData separat, stoert die Electron-App nicht).
    wchar_t lad[MAX_PATH] = {}; GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    std::wstring userData = std::wstring(lad) + L"\\lumora-shell";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(res) || !env) { MessageBoxW(hwnd, L"WebView2-Runtime nicht verfuegbar.\n(Installer bringt den Bootstrapper mit.)", L"Lumora Shell", MB_ICONERROR); PostQuitMessage(2); return res; }
                env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT res2, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(res2) || !ctrl) { PostQuitMessage(3); return res2; }
                            g_controller = ctrl;
                            g_controller->get_CoreWebView2(&g_webview);
                            placeWebView(hwnd);
                            // Dokument-Fokus sofort setzen (Gamepad-API braucht ihn; das erste
                            // WM_SETFOCUS kam ggf. schon VOR der Controller-Erzeugung)
                            g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                            // -webkit-app-region:drag aus styles.css aktivieren (Fenster ziehen,
                            // Doppelklick-Maximieren, System-Menue - wie Electrons frame:false).
                            { ComPtr<ICoreWebView2Settings> set0; g_webview->get_Settings(&set0);
                              ComPtr<ICoreWebView2Settings9> set9; if (set0) set0.As(&set9);
                              if (set9) set9->put_IsNonClientRegionSupportEnabled(TRUE); }
                            // Projektordner als https://app.lumora/, Datenordner (%APPDATA%\lumora)
                            // als https://data.lumora/ einblenden (sendSync-Emulation + spaeter Medien)
                            ComPtr<ICoreWebView2_3> wv3; g_webview.As(&wv3);
                            if (wv3) {
                                wv3->SetVirtualHostNameToFolderMapping(L"app.lumora", g_appDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                wv3->SetVirtualHostNameToFolderMapping(L"data.lumora", dataDir().c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                // Cover/Hero: aktueller Medienordner + Alt-Pfad (hdr-launcher) aus aelteren games.json-Eintraegen
                                wv3->SetVirtualHostNameToFolderMapping(L"media.lumora", (dataDir() + L"\\media").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                wchar_t ad[MAX_PATH] = {}; GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
                                wv3->SetVirtualHostNameToFolderMapping(L"media0.lumora", (std::wstring(ad) + L"\\hdr-launcher\\media").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }
                            std::wstring shim = SHIM_JS;   // Versions-Platzhalter fuellen
                            size_t vp = shim.find(L"%SHELL_VERSION%");
                            if (vp != std::wstring::npos) shim.replace(vp, 15, widen(shellVersion()));
                            g_webview->AddScriptToExecuteOnDocumentCreated(shim.c_str(), nullptr);
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR j = nullptr;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&j)) && j) { onWebMessage(j, g_hwnd); CoTaskMemFree(j); }
                                        return S_OK;
                                    }).Get(), nullptr);
                            g_webview->Navigate(L"https://app.lumora/index.html");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(hr)) { MessageBoxW(hwnd, L"WebView2-Init fehlgeschlagen.", L"Lumora Shell", MB_ICONERROR); return 2; }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
