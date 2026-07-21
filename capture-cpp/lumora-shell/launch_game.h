// Spielstart-Modul (1:1-Portierung des launch-game-Handlers aus main.js):
// - HDR-Automatik (HDRCmd on/off + 3s-Wartezeit + hdr-status/launch-status-Pushes)
// - Xbox/UWP via AUMID (nativ ueber FOLDERID_AppsFolder statt Get-StartApps-PowerShell)
// - Steam-Spiele UEBER Steam (steam://rungameid - DRM + korrekte Steam-Spielzeit)
// - .lnk via ShellExecute, Admin via PowerShell Start-Process -Verb RunAs
// - Lauf-Erkennung: Steam-Registry ODER Exe-Name ODER Prozess-im-Spielordner
//   (nativ: Toolhelp-Snapshot statt tasklist/PowerShell), 4s-Takt, Start-Timeout
//   30s (Button frei) / 120s (aufgeben), Ende nach 2 leeren Checks (~8s).
// - play-session-Push (Spielzeit) + playtime-log.txt wie im Original.
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <string>
#include <vector>
#include <regex>
#include "json.hpp"
#include "scan_games.h"

namespace lulaunch {
using nlohmann::json;
namespace sfs = std::filesystem;

inline std::vector<std::wstring> tokenizeArgs(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wregex re(LR"("[^"]*"|\S+)");
    for (auto it = std::wsregex_iterator(s.begin(), s.end(), re); it != std::wsregex_iterator(); ++it) {
        std::wstring t = it->str();
        if (t.size() >= 2 && t.front() == L'"' && t.back() == L'"') t = t.substr(1, t.size() - 2);
        out.push_back(t);
    }
    return out;
}

// HDR global an/aus - EIGENER Code statt HDRCmd.exe (HDRTray, GPLv3, entfernt).
// Pro aktivem Display-Target: erst die moderne 24H2-API (SET_HDR_STATE), bei
// aelterem Windows Fallback auf SET_ADVANCED_COLOR_STATE (Win10 1709+, gleiche
// Wirkung dort). HDR-faehig? -> GET_ADVANCED_COLOR_INFO_2 (highDynamicRangeSupported),
// Fallback klassisches GET_ADVANCED_COLOR_INFO (advancedColorSupported).
inline void setHDR(bool on) {
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode, modes.data(), nullptr) != ERROR_SUCCESS) return;
    paths.resize(nPath);
    for (auto& p : paths) {
        const LUID adapter = p.targetInfo.adapterId; const UINT32 target = p.targetInfo.id;
        // HDR-Faehigkeit pruefen (24H2-Info zuerst, sonst klassisch)
        bool hdrCapable = false;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 i2{};
        i2.header = { DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2, sizeof(i2), adapter, target };
        if (DisplayConfigGetDeviceInfo(&i2.header) == ERROR_SUCCESS) hdrCapable = i2.highDynamicRangeSupported;
        else {
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO i1{};
            i1.header = { DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(i1), adapter, target };
            if (DisplayConfigGetDeviceInfo(&i1.header) == ERROR_SUCCESS) hdrCapable = i1.advancedColorSupported;
        }
        if (!hdrCapable) continue;
        // Umschalten: 24H2-API zuerst, sonst klassisch
        DISPLAYCONFIG_SET_HDR_STATE hs{};
        hs.header = { DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE, sizeof(hs), adapter, target };
        hs.enableHdr = on ? 1 : 0;
        if (DisplayConfigSetDeviceInfo(&hs.header) == ERROR_SUCCESS) continue;
        DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE as{};
        as.header = { DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE, sizeof(as), adapter, target };
        as.enableAdvancedColor = on ? 1 : 0;
        DisplayConfigSetDeviceInfo(&as.header);
    }
}

// Steam-Bibliotheken (SteamPath + libraryfolders.vdf) - wie getSteamLibraries in main.js.
inline std::vector<sfs::path> steamLibs() {
    std::vector<sfs::path> libs;
    wchar_t sp[MAX_PATH] = {}; DWORD sz = sizeof(sp);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, sp, &sz) != ERROR_SUCCESS) return libs;
    libs.push_back(sfs::path(sp) / L"steamapps");
    std::string lib = lushell::readTextFile(sfs::path(sp) / L"steamapps" / L"libraryfolders.vdf");
    std::regex pathRe("\"path\"\\s+\"([^\"]+)\"", std::regex::icase);
    for (auto it = std::sregex_iterator(lib.begin(), lib.end(), pathRe); it != std::sregex_iterator(); ++it) {
        std::string p = (*it)[1]; std::string clean;
        for (size_t i = 0; i < p.size(); ++i) { if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i; clean.push_back(p[i]); }
        libs.push_back(sfs::path(sfs::u8path(clean)) / L"steamapps");
    }
    return libs;
}

// AppId zu einer Exe unterhalb steamapps/common/<installdir>/ (acf-Scan wie main.js).
inline std::string steamAppIdForExe(const std::wstring& exePath) {
    std::string p8 = lushell::toUtf8(exePath); for (auto& c : p8) c = (char)tolower((unsigned char)c);
    std::smatch m;
    if (!std::regex_search(p8, m, std::regex(R"(steamapps[\\/]+common[\\/]+([^\\/]+)[\\/])"))) return "";
    std::string installdir = m[1];
    std::error_code ec;
    for (auto& lib : steamLibs()) {
        for (auto& e : sfs::directory_iterator(lib, ec)) {
            std::wstring fn = e.path().filename().wstring();
            if (fn.rfind(L"appmanifest_", 0) != 0 || e.path().extension() != L".acf") continue;
            std::string acf = lushell::readTextFile(e.path());
            std::string dir = lushell::vdfValue(acf, "installdir");
            for (auto& c : dir) c = (char)tolower((unsigned char)c);
            if (dir == installdir) {
                std::string id = lushell::vdfValue(acf, "appid");
                if (id.empty()) { std::smatch mm; std::string f8 = lushell::toUtf8(fn); if (std::regex_search(f8, mm, std::regex("\\d+"))) id = mm[0]; }
                return id;
            }
        }
    }
    return "";
}

// Laeuft das Steam-Spiel laut Steam? (Registry Apps\<id>\Running) 1/0/-1=unbekannt.
inline int steamAppRunning(const std::string& appId) {
    DWORD v = 0, sz = sizeof(v);
    std::wstring key = L"Software\\Valve\\Steam\\Apps\\" + luart::toW(appId);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"Running", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return -1;
    return v ? 1 : 0;
}

// .lnk-Ziel nativ aufloesen (IShellLink statt PowerShell/WScript).
inline std::wstring resolveLnkTarget(const std::wstring& lnkPath) {
    std::wstring target;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
            if (SUCCEEDED(pf->Load(lnkPath.c_str(), STGM_READ))) {
                wchar_t buf[MAX_PATH] = {};
                if (SUCCEEDED(sl->GetPath(buf, MAX_PATH, nullptr, 0)) && buf[0]) target = buf;
            }
            pf->Release();
        }
        sl->Release();
    }
    return target;
}
// Prozessname zum Spielpfad (bei .lnk das Ziel; Fallback <name>.exe wie main.js).
inline std::wstring resolveProcessName(const std::wstring& gamePath) {
    sfs::path p = gamePath;
    std::wstring ext = p.extension().wstring(); for (auto& c : ext) c = towlower(c);
    if (ext != L".lnk") return p.filename().wstring();
    std::wstring t = resolveLnkTarget(gamePath);
    if (!t.empty()) return sfs::path(t).filename().wstring();
    return p.stem().wstring() + L".exe";
}

// Xbox/UWP: AUMID ueber den AppsFolder (nativ statt Get-StartApps): Namens-Match
// "<XboxGames-Ordnername>*", kuerzester Treffer gewinnt (wie das PowerShell-Original).
inline std::wstring xboxAumidForGame(const std::wstring& gamePath) {
    std::wsmatch m;
    if (!std::regex_search(gamePath, m, std::wregex(LR"(\\XboxGames\\([^\\]+)\\)", std::regex::icase))) return L"";
    std::wstring folder = m[1]; std::wstring folderLow = folder; for (auto& c : folderLow) c = towlower(c);
    std::wstring bestId; size_t bestLen = SIZE_MAX;
    IShellItem* apps = nullptr;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&apps)))) return L"";
    IEnumShellItems* en = nullptr;
    if (SUCCEEDED(apps->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
        IShellItem* it = nullptr; ULONG got = 0;
        while (en->Next(1, &it, &got) == S_OK && got) {
            LPWSTR name = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
                std::wstring n = name; std::wstring nl = n; for (auto& c : nl) c = towlower(c);
                if (nl.rfind(folderLow, 0) == 0 && n.size() < bestLen) {
                    IShellItem2* it2 = nullptr;
                    if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&it2)))) {
                        LPWSTR id = nullptr;
                        if (SUCCEEDED(it2->GetString(PKEY_AppUserModel_ID, &id)) && id) { bestId = id; bestLen = n.size(); CoTaskMemFree(id); }
                        it2->Release();
                    }
                }
                CoTaskMemFree(name);
            }
            it->Release();
        }
        en->Release();
    }
    apps->Release();
    return bestId;
}

// Prozess mit diesem Exe-Namen vorhanden? (Toolhelp-Snapshot statt tasklist-CSV)
inline bool processByName(const std::wstring& exeName) {
    std::wstring low = exeName; for (auto& c : low) c = towlower(c);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) }; bool found = false;
    if (Process32FirstW(snap, &pe)) do {
        std::wstring n = pe.szExeFile; for (auto& c : n) c = towlower(c);
        if (n == low) { found = true; break; }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

// Irgendein Prozess aus dem Spielordner? (Launcher-Exes/anderer Prozessname/DRM-frei)
inline bool anyProcessInFolder(const std::wstring& dir) {
    std::wstring prefix = dir; if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
    for (auto& c : prefix) c = towlower(c);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) }; bool found = false;
    if (Process32FirstW(snap, &pe)) do {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t buf[MAX_PATH * 2] = {}; DWORD len = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
            std::wstring p = buf; for (auto& c : p) c = towlower(c);
            if (p.rfind(prefix, 0) == 0) found = true;
        }
        CloseHandle(h);
        if (found) break;
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

// Diagnose-Protokoll der Spielzeit-Erfassung (%APPDATA%\lumora\playtime-log.txt).
inline void playLog(const std::wstring& dataDir, const std::string& msg) {
    FILE* f = nullptr; _wfopen_s(&f, (dataDir + L"\\playtime-log.txt").c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME st; GetSystemTime(&st);
    char ts[40]; sprintf_s(ts, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fprintf(f, "%s  %s\n", ts, msg.c_str());
    fclose(f);
}

// Laufende Eigenstart-Ueberwachung (ein Eintrag pro launch-game; Tick alle 4s im UI-Timer).
struct LaunchSession {
    std::wstring gamePath, gameDir, exeName;
    std::string appId;              // Steam-AppId ('' = keins)
    bool isLnk = false, useHdr = false;
    bool started = false, reenabled = false, done = false;
    int absent = 0;
    ULONGLONG launchTs = 0, startTs = 0;
};

inline bool probeRunning(const LaunchSession& s) {
    if (!s.appId.empty() && steamAppRunning(s.appId) == 1) return true;   // 1) Steam-Registry
    if (processByName(s.exeName)) return true;                            // 2) Exe-Name
    if (!s.isLnk && anyProcessInFolder(s.gameDir)) return true;           // 3) Ordner-Prozess
    return false;
}

} // namespace lulaunch
