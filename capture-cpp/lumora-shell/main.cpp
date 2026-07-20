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
#include <wrl.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "WebView2.h"
#include "json.hpp"
#include "scan_games.h"
#pragma comment(lib, "gdiplus.lib")

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
    else if (d && d.channel) { (listeners[d.channel] || []).forEach((f) => { try { f({}, d.payload) } catch {} }); }
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
static void sendToUi(const std::string& channel, const json& payload) {
    if (!g_webview) return;
    json m = { {"channel", channel}, {"payload", payload} };
    g_webview->PostWebMessageAsJson(widen(m.dump()).c_str());
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

// Zentraler Kanal-Dispatcher (Phase-2-Checkliste: capture-cpp/lumora-shell/IPC-INVENTAR.md).
// Unbekannte Kanaele -> null (Promise loest auf, UI haengt nicht).
static json handleChannel(const std::string& channel, const json& args) {
    if (channel == "get-app-settings") return loadSettings();
    if (channel == "set-app-settings") {                      // Merge wie Object.assign in main.js
        if (args.size() >= 1 && args[0].is_object()) {
            json s = loadSettings(); s.update(args[0]);
            writeFile(settingsPath(), s.dump(2));
        }
        return true;
    }
    if (channel == "save-games" && args.size() >= 1 && args[0].is_string()) {   // UI liefert fertigen JSON-String
        writeFile(dataDir() + L"\\games.json", args[0].get<std::string>()); return true;
    }
    if (channel == "save-prefs" && args.size() >= 1 && args[0].is_string()) {
        writeFile(dataDir() + L"\\prefs.json", args[0].get<std::string>()); return true;
    }
    if (channel == "launch-game" && args.size() >= 1 && args[0].is_string()) {
        // Erstversion: Basis-Start (HDR-Schalter/Steam-Erkennung/Prozess-Watch folgen mit dem HDR-Modul).
        std::wstring p = widen(args[0].get<std::string>());
        sendToUi("launch-status", "launching");
        std::wstring dir = p.substr(0, p.find_last_of(L'\\'));
        ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        return true;
    }
    if (channel == "open-game-folder" && args.size() >= 1 && args[0].is_string()) {
        std::wstring p = widen(args[0].get<std::string>());
        std::wstring param = L"/select,\"" + p + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (channel == "scan-games") return lushell::scanGames(args.size() >= 1 ? args[0] : json::array());   // Steam+Xbox+Ordner (weitere Stores folgen)
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
    if (channel == "get-file-icon" && args.size() >= 1 && args[0].is_string())
        return fileIconDataUrl(widen(args[0].get<std::string>()));   // Erstversion: Datei-Icon (Steam-Original folgt mit dem Library-Modul)
    if (channel == "list-gpus") return json::array();   // OSD-Sensorik (nvml/adl) folgt mit dem OSD-Modul
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

static void onWebMessage(const std::wstring& raw) {
    json msg = json::parse(narrow(raw), nullptr, false);
    if (!msg.is_object()) return;
    long long id = msg.value("id", 0ll);
    std::string channel = msg.value("channel", "");
    json args = msg.contains("args") && msg["args"].is_array() ? msg["args"] : json::array();
    json result = nullptr;
    try { result = handleChannel(channel, args); } catch (...) {}
    if (id > 0 && g_webview) {
        json resp = { {"id", id}, {"result", result} };
        g_webview->PostWebMessageAsJson(widen(resp.dump()).c_str());
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
    case WM_NCCALCSIZE:
        if (w) {   // Client = ganzes Fenster (keine System-Titelleiste/-Knoepfe mehr);
            auto* pr = (RECT*)l;   // maximiert die unsichtbaren Frame-Insets abziehen, sonst ragt das Fenster ueber den Monitor
            if (IsZoomed(h)) {
                int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                pr->left += fx; pr->right -= fx; pr->top += fy; pr->bottom -= fy;
            }
            return 0;
        }
        break;
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
    case WM_CLOSE: saveWindowState(h); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // COM (FileDialogs, WebView2) - GUI-Thread = STA
    Gdiplus::GdiplusStartupInput gsi; ULONG_PTR gtok = 0; Gdiplus::GdiplusStartup(&gtok, &gsi, nullptr);   // Datei-Icons (PNG)
    // App-Ordner (index.html) finden: --appdir > aktuelles Verzeichnis > exe-Ordner >
    // von dort aufwaerts (Doppelklick aus build/Release im Quellbaum). Im spaeteren
    // Installer liegt das UI direkt neben der exe - dann greift Stufe 3 sofort.
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // Selbsttest ohne UI: Settings laden + Merge IN-MEMORY (echte Datei bleibt unberuehrt),
    // Ergebnis nach %TEMP%\lumora-shell-test.txt - automatisierte Verifikation des Dispatchers.
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--test-scan") == 0) {
        // Selbsttest: echten Steam+Xbox-Scan laufen lassen, Anzahl + Beispiele in die Testdatei.
        json r = lushell::scanGames(json::array());
        std::string s = "gefunden=" + std::to_string(r.size()) + "\n";
        for (size_t k = 0; k < r.size() && k < 6; ++k) s += "  " + r[k].value("source", "?") + ": " + r[k].value("name", "?") + " -> " + r[k].value("path", "?") + "\n";
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
    ShowWindow(hwnd, st.value("maximized", false) ? SW_MAXIMIZE : nShow);

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
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&j)) && j) { onWebMessage(j); CoTaskMemFree(j); }
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
