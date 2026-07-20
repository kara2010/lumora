// Spiele-Scanner (Portierung aus main.js, identische Heuristik/Formate):
// scanSteam (Registry SteamPath + libraryfolders.vdf + appmanifest_*.acf),
// scanXbox (<Laufwerk>:\XboxGames\<Name>\Content), scanFolder (Unterordner).
// Ergebnis je Spiel: {name, path, source} - exakt wie der Electron-Handler.
// Erstversion synchron im UI-Thread (kurzer Freeze beim Scan-Klick; spaeter Thread).
// Weitere Stores (Epic/GOG/Ubisoft/EA/...) folgen nach demselben Muster.
#pragma once
#include <windows.h>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>
#include "json.hpp"

namespace lushell {
namespace sfs = std::filesystem;
using nlohmann::json;

// Skip-Muster 1:1 aus main.js (Installer/Redist/Helfer-Exes bzw. Support-Ordner).
static const std::regex SKIP_EXE_RE(
    "\\b(setup|install\\w*|uninst|redist|vcredist|dotnet|dxsetup|directx|crash|report|handler|helper|register|physx|oalinst|vc_redist|launcher(?!.*game)|creativeengine|anticheat|easyanticheat|battleye|beservice|touchup|cleanup|activation)\\b",
    std::regex::icase);
static const std::regex SKIP_DIR_RE(
    "^(redist|_commonredist|directx|support|crash|logs|__installer|temp|bin32|appdata|prerequisites)\\b",
    std::regex::icase);

inline std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
inline std::string alnumLower(const std::string& s) {
    std::string o; for (char c : s) { if (isalnum((unsigned char)c)) o.push_back((char)tolower((unsigned char)c)); }
    return o;
}

struct ExeCand { std::wstring name, full; unsigned long long size; };

inline void collectExes(const sfs::path& dir, int depth, std::vector<ExeCand>& out) {
    if (depth > 2) return;
    std::error_code ec;
    for (auto& e : sfs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) {
            std::wstring fn = e.path().filename().wstring();
            std::string fn8 = toUtf8(fn);
            if (fn8.size() > 4 && _wcsicmp(fn.c_str() + fn.size() - 4, L".exe") == 0 && !std::regex_search(fn8, SKIP_EXE_RE)) {
                unsigned long long sz = e.file_size(ec); if (ec) { ec.clear(); sz = 0; }
                out.push_back({ fn, e.path().wstring(), sz });
            }
        } else if (e.is_directory(ec) && depth < 2) {
            std::string dn = toUtf8(e.path().filename().wstring());
            if (!std::regex_search(dn, SKIP_DIR_RE)) collectExes(e.path(), depth + 1, out);
        }
    }
}

// Groesste/bestbenannte Exe im Spielordner (Score = Dateigroesse + Namens-Bonus, wie main.js).
inline std::wstring findMainExe(const sfs::path& gameDir, const std::wstring& gameName) {
    std::vector<ExeCand> exes; collectExes(gameDir, 0, exes);
    if (exes.empty()) return L"";
    std::string nameBase = alnumLower(toUtf8(gameName.empty() ? gameDir.filename().wstring() : gameName));
    const ExeCand* best = nullptr; double bestScore = -1;
    for (auto& e : exes) {
        std::string base = toUtf8(e.name); if (base.size() > 4) base = base.substr(0, base.size() - 4);
        base = alnumLower(base);
        double score = (double)e.size;
        if (base == nameBase) score += 1e12;
        else if (!base.empty() && !nameBase.empty() && (nameBase.find(base) != std::string::npos || base.find(nameBase) != std::string::npos)) score += 1e9;
        if (score > bestScore) { bestScore = score; best = &e; }
    }
    return best ? best->full : L"";
}

inline std::string readTextFile(const sfs::path& p) {
    FILE* f = nullptr; _wfopen_s(&f, p.wstring().c_str(), L"rb"); if (!f) return "";
    std::string s; char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f); return s;
}
inline std::string vdfValue(const std::string& content, const std::string& key) {
    std::smatch m;
    if (std::regex_search(content, m, std::regex("\"" + key + "\"\\s+\"([^\"]+)\"", std::regex::icase))) return m[1];
    return "";
}

inline json scanSteam() {
    json out = json::array();
    wchar_t sp[MAX_PATH] = {}; DWORD sz = sizeof(sp);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, sp, &sz) != ERROR_SUCCESS) return out;
    std::vector<sfs::path> libs; libs.push_back(sfs::path(sp) / L"steamapps");
    std::string lib = readTextFile(sfs::path(sp) / L"steamapps" / L"libraryfolders.vdf");
    std::regex pathRe("\"path\"\\s+\"([^\"]+)\"", std::regex::icase);
    for (auto it = std::sregex_iterator(lib.begin(), lib.end(), pathRe); it != std::sregex_iterator(); ++it) {
        std::string p = (*it)[1]; std::string clean; for (size_t i = 0; i < p.size(); ++i) { if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i; clean.push_back(p[i]); }
        libs.push_back(sfs::path(sfs::u8path(clean)) / L"steamapps");
    }
    std::error_code ec;
    for (auto& lp : libs) {
        for (auto& e : sfs::directory_iterator(lp, ec)) {
            std::wstring fn = e.path().filename().wstring();
            if (fn.rfind(L"appmanifest_", 0) != 0 || e.path().extension() != L".acf") continue;
            std::string acf = readTextFile(e.path());
            std::string name = vdfValue(acf, "name"), inst = vdfValue(acf, "installdir");
            if (name.empty() || inst.empty()) continue;
            sfs::path gameDir = lp / L"common" / sfs::u8path(inst);
            if (!sfs::exists(gameDir, ec)) continue;
            std::wstring exe = findMainExe(gameDir, sfs::u8path(name).wstring());
            if (!exe.empty()) out.push_back({ {"name", name}, {"path", toUtf8(exe)}, {"source", "Steam"} });
        }
    }
    return out;
}

inline json scanXbox() {
    json out = json::array(); std::error_code ec;
    DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drives & (1u << d))) continue;
        sfs::path root = std::wstring(1, L'A' + d) + L":\\XboxGames";
        if (!sfs::exists(root, ec)) continue;
        for (auto& e : sfs::directory_iterator(root, ec)) {
            if (!e.is_directory(ec)) continue;
            sfs::path content = e.path() / L"Content";
            sfs::path dir = sfs::exists(content, ec) ? content : e.path();
            std::wstring exe = findMainExe(dir, e.path().filename().wstring());
            if (!exe.empty()) out.push_back({ {"name", toUtf8(e.path().filename().wstring())}, {"path", toUtf8(exe)}, {"source", "Xbox"} });
        }
    }
    return out;
}

inline json scanFolder(const std::wstring& folder) {
    json out = json::array(); std::error_code ec;
    for (auto& e : sfs::directory_iterator(folder, ec)) {
        if (!e.is_directory(ec)) continue;
        std::string dn = toUtf8(e.path().filename().wstring());
        if (std::regex_search(dn, SKIP_DIR_RE)) continue;
        std::wstring exe = findMainExe(e.path(), e.path().filename().wstring());
        if (!exe.empty()) out.push_back({ {"name", dn}, {"path", toUtf8(exe)}, {"source", "Ordner"} });
    }
    return out;
}

// scan-games-Handler: Steam + Xbox + Zusatzordner, nach Pfad dedupliziert (wie main.js).
inline json scanGames(const json& extraFolders) {
    json all = json::array();
    for (auto& g : scanSteam()) all.push_back(g);
    for (auto& g : scanXbox()) all.push_back(g);
    if (extraFolders.is_array())
        for (auto& f : extraFolders)
            if (f.is_string()) { std::wstring w(f.get<std::string>().begin(), f.get<std::string>().end());
                int n = MultiByteToWideChar(CP_UTF8, 0, f.get<std::string>().c_str(), -1, nullptr, 0);
                std::wstring wf(n ? n - 1 : 0, 0); MultiByteToWideChar(CP_UTF8, 0, f.get<std::string>().c_str(), -1, &wf[0], n);
                for (auto& g : scanFolder(wf)) all.push_back(g); }
    json out = json::array(); std::vector<std::string> seen;
    for (auto& g : all) {
        std::string k = g.value("path", ""); for (auto& c : k) c = (char)tolower((unsigned char)c);
        bool dup = false; for (auto& s : seen) if (s == k) { dup = true; break; }
        if (!dup) { seen.push_back(k); out.push_back(g); }
    }
    return out;
}

} // namespace lushell
