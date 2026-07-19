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
#include <shellapi.h>
#include <wrl.h>
#include <string>
#include <fstream>
#include <sstream>
#include "WebView2.h"
#include "json.hpp"

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

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: if (g_controller) { RECT rc; GetClientRect(h, &rc); g_controller->put_Bounds(rc); } return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // App-Ordner (index.html) finden: --appdir > aktuelles Verzeichnis > exe-Ordner >
    // von dort aufwaerts (Doppelklick aus build/Release im Quellbaum). Im spaeteren
    // Installer liegt das UI direkt neben der exe - dann greift Stufe 3 sofort.
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // Selbsttest ohne UI: Settings laden + Merge IN-MEMORY (echte Datei bleibt unberuehrt),
    // Ergebnis nach %TEMP%\lumora-shell-test.txt - automatisierte Verifikation des Dispatchers.
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
    wc.hbrBackground = CreateSolidBrush(RGB(11, 13, 20));   // dunkler App-Hintergrund bis WebView2 gemalt hat
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"LumoraShell", L"Lumora", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1400, 900, nullptr, nullptr, hInst, nullptr);
    g_hwnd = hwnd;
    ShowWindow(hwnd, nShow);

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
                            RECT rc; GetClientRect(hwnd, &rc); g_controller->put_Bounds(rc);
                            // Projektordner als https://app.lumora/, Datenordner (%APPDATA%\lumora)
                            // als https://data.lumora/ einblenden (sendSync-Emulation + spaeter Medien)
                            ComPtr<ICoreWebView2_3> wv3; g_webview.As(&wv3);
                            if (wv3) {
                                wv3->SetVirtualHostNameToFolderMapping(L"app.lumora", g_appDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                                wv3->SetVirtualHostNameToFolderMapping(L"data.lumora", dataDir().c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
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
