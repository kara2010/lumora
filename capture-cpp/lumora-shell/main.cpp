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

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static std::wstring g_appDir;

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
  const ipcRenderer = {
    invoke: (channel, ...args) => new Promise((resolve) => { const id = ++seq; pending.set(id, { resolve }); window.chrome.webview.postMessage({ id, channel, args }); }),
    send: (channel, ...args) => window.chrome.webview.postMessage({ id: 0, channel, args }),
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
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
// Primitive Feld-Extraktion aus dem WebMessage-JSON (PoC; Phase 2 bekommt einen echten Parser).
static long long jsonId(const std::wstring& j) {
    size_t p = j.find(L"\"id\":"); if (p == std::wstring::npos) return 0;
    return _wtoi64(j.c_str() + p + 5);
}
static std::wstring jsonStr(const std::wstring& j, const wchar_t* key) {
    std::wstring pat = std::wstring(L"\"") + key + L"\":\"";
    size_t p = j.find(pat); if (p == std::wstring::npos) return L"";
    p += pat.size(); std::wstring out;
    while (p < j.size() && j[p] != L'"') { if (j[p] == L'\\' && p + 1 < j.size()) ++p; out.push_back(j[p++]); }
    return out;
}

static void onWebMessage(const std::wstring& json) {
    long long id = jsonId(json);
    std::wstring channel = jsonStr(json, L"channel");
    std::wstring result = L"null";
    if (channel == L"get-app-settings") {
        wchar_t appdata[MAX_PATH] = {}; GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        std::string s = readFile(std::wstring(appdata) + L"\\lumora\\app-settings.json");
        if (!s.empty()) result = widen(s);      // Dateiinhalt IST JSON -> direkt als result einbetten
        else result = L"{}";
    } else if (channel == L"shell-open-external") {
        std::wstring url = jsonStr(json, L"args");   // args:["url"] -> erster String ("args":[" ist Teil des Musters unten)
        size_t p = json.find(L"\"args\":[\"");
        if (p != std::wstring::npos) { p += 9; std::wstring u; while (p < json.size() && json[p] != L'"') { if (json[p] == L'\\') ++p; u.push_back(json[p++]); }
            if (u.rfind(L"http", 0) == 0) ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
        result = L"true";
    }
    if (id > 0 && g_webview) {
        std::wstring resp = L"{\"id\":" + std::to_wstring(id) + L",\"result\":" + result + L"}";
        g_webview->PostWebMessageAsJson(resp.c_str());
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
                            // Projektordner als https://app.lumora/ einblenden (fetch/relative Pfade funktionieren)
                            ComPtr<ICoreWebView2_3> wv3; g_webview.As(&wv3);
                            if (wv3) wv3->SetVirtualHostNameToFolderMapping(L"app.lumora", g_appDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            g_webview->AddScriptToExecuteOnDocumentCreated(SHIM_JS, nullptr);
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
