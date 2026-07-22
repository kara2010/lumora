// Komponenten-Updater: tauscht einzelne Dateien (Shell/Helfer/UI) ohne vollen
// Installer. Ersetzt die alte Regel "niemals Datei-Update" (Fleet-Desaster der
// Electron-Zeit) durch ein Design, das deren Fehlerbild konstruktiv verhindert:
//   1) MANIFEST components.json: kompletter Soll-Zustand (Version + pro Datei
//      Pfad/SHA-256/Groesse) - der Updater kennt immer den vollstaendigen Stand.
//   2) STAGE -> VERIFY -> SWAP, alles oder nichts: erst ALLE geaenderten Dateien
//      in den Staging-Ordner laden, dort SHA-256 UND Authenticode-Signatur
//      (nur UNSER Zertifikat-Thumbprint) pruefen. Erst dann tauschen:
//      Ziel -> .old umbenennen (geht auch bei laufender Exe), Neues an den Platz.
//      Jeder Fehler -> vollstaendiger Rollback der .old-Dateien. Ein Mischbestand
//      ist damit unmoeglich.
//   3) Der volle Installer bleibt als Fallback-Kanal bestehen (native-update.json).
// Getauschte Shell wird erst beim naechsten App-Start aktiv (kein Zwangs-Neustart);
// .old-Reste raeumt der naechste Start weg (cleanupOldFiles).
#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "json.hpp"

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace luupd {
using nlohmann::json;
namespace ufs = std::filesystem;

// Thumbprint (SHA-1) unseres Certum-Codesign-Zertifikats - NUR damit signierte
// Binaries werden akzeptiert (gleicher Wert wie in build-installer.ps1).
static const char* CERT_THUMBPRINT = "ec6b6b6fdebdb88941519f15e9570994ce3e14e3";

struct Deps {   // Abhaengigkeiten aus main.cpp (kein Zugriff auf dessen Statics)
    std::function<nlohmann::json()> loadSettings;
    std::function<std::string(const std::string&)> httpGetBody;   // "" bei Fehler
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&, const json&)> notify;  // sendToUi
};

// --- SHA-256 einer Datei (CNG) als lowercase-Hex ---
inline std::string sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 16); DWORD got = 0;
        while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
            BCryptHashData(h, buf.data(), got, 0);
        unsigned char dig[32];
        if (BCryptFinishHash(h, dig, sizeof(dig), 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; ++i) sprintf_s(hex + i * 2, 3, "%02x", dig[i]);
            out = hex;
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

// --- Authenticode: Signatur gueltig UND Signer-Thumbprint = unser Zertifikat ---
inline bool verifySignature(const std::wstring& path) {
    WINTRUST_FILE_INFO fi{ sizeof(fi) }; fi.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{ sizeof(wd) };
    wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE; wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG st = WinVerifyTrust(nullptr, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    if (st != ERROR_SUCCESS) return false;

    // Signer-Zertifikat extrahieren und Thumbprint vergleichen
    HCERTSTORE store = nullptr; HCRYPTMSG msg = nullptr; bool ok = false;
    if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY,
        0, nullptr, nullptr, nullptr, &store, &msg, nullptr)) {
        DWORD sz = 0;
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &sz)) {
            std::vector<unsigned char> buf(sz);
            auto* si = (CMSG_SIGNER_INFO*)buf.data();
            if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, si, &sz)) {
                CERT_INFO ci{}; ci.Issuer = si->Issuer; ci.SerialNumber = si->SerialNumber;
                PCCERT_CONTEXT cert = CertFindCertificateInStore(store,
                    X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, nullptr);
                if (cert) {
                    unsigned char tp[20]; DWORD tpLen = sizeof(tp);
                    if (CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, tp, &tpLen)) {
                        char hex[41];
                        for (DWORD i = 0; i < tpLen; ++i) sprintf_s(hex + i * 2, 3, "%02x", tp[i]);
                        ok = _stricmp(hex, CERT_THUMBPRINT) == 0;
                    }
                    CertFreeCertificateContext(cert);
                }
            }
        }
    }
    if (msg) CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);
    return ok;
}

// .old-Reste eines frueheren Swaps entfernen (beim App-Start aufrufen;
// die eigene alte Exe ist dann nicht mehr gesperrt).
inline void cleanupOldFiles(const std::wstring& appDir) {
    std::error_code ec;
    for (auto it = ufs::recursive_directory_iterator(appDir, ec); it != ufs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && it->path().extension() == L".old")
            ufs::remove(it->path(), ec);
    }
}

// 3-teiliger Versionsvergleich (wie cmpVer in main.cpp)
inline int cmpVer3(const std::string& a, const std::string& b) {
    int x[3] = { 0,0,0 }, y[3] = { 0,0,0 };
    sscanf_s(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]); sscanf_s(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

// Ein Update-Durchlauf. Rueckgabe: Manifest-Version, wenn Komponenten getauscht
// wurden (Aufrufer merkt sie sich als componentsVersion), sonst "".
inline std::string runOnce(const Deps& d, const std::wstring& appDir, const std::string& currentVer) {
    json s = d.loadSettings();
    std::string feed = s.value("componentFeed", std::string("https://lumora-streaming.de/updates/components.json"));

    std::string body = d.httpGetBody(feed);
    if (body.empty()) return "";
    json m = json::parse(body, nullptr, false);
    if (!m.is_object() || !m.contains("files")) return "";
    std::string mver = m.value("version", "");
    std::string baseUrl = m.value("baseUrl", "");
    if (mver.empty() || baseUrl.empty()) return "";
    if (cmpVer3(currentVer, mver) >= 0) return "";   // nichts Neueres

    // --- 1) Abweichende Dateien ermitteln (SHA-256 gegen Ist-Zustand) ---
    struct Item { std::string path, sha; uint64_t size; };
    std::vector<Item> todo;
    for (auto& f : m["files"]) {
        std::string rel = f.value("path", ""), sha = f.value("sha256", "");
        if (rel.empty() || sha.empty()) return "";                     // defektes Manifest: gar nichts tun
        if (rel.find("..") != std::string::npos) return "";            // Pfad-Traversal ausschliessen
        std::wstring target = appDir + L"\\" + std::wstring(rel.begin(), rel.end());
        if (_stricmp(sha256File(target).c_str(), sha.c_str()) != 0)
            todo.push_back({ rel, sha, f.value("size", (uint64_t)0) });
    }
    if (todo.empty()) return mver;   // Stand entspricht bereits dem Manifest
    d.log("comp-update: " + mver + ", " + std::to_string(todo.size()) + " Datei(en)");

    // --- 2) STAGE: alles herunterladen ---
    wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
    ufs::path stage = ufs::path(tmp) / L"lumora-comp-stage";
    std::error_code ec;
    ufs::remove_all(stage, ec); ufs::create_directories(stage, ec);
    for (auto& it : todo) {
        std::string url = baseUrl + it.path;
        for (auto& c : url) if (c == '\\') c = '/';
        std::string data = d.httpGetBody(url);
        if (data.empty()) { d.log("comp-update: Download fehlgeschlagen: " + it.path); return ""; }
        ufs::path dst = stage / it.path;
        ufs::create_directories(dst.parent_path(), ec);
        HANDLE f = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return "";
        DWORD w = 0; WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr); CloseHandle(f);
    }

    // --- 3) VERIFY: SHA-256 aller Staging-Dateien + Authenticode bei exe/dll ---
    for (auto& it : todo) {
        ufs::path st = stage / it.path;
        if (_stricmp(sha256File(st.wstring()).c_str(), it.sha.c_str()) != 0) {
            d.log("comp-update: Hash-Fehler: " + it.path); return "";
        }
        std::wstring ext = st.extension().wstring();
        if (_wcsicmp(ext.c_str(), L".exe") == 0 || _wcsicmp(ext.c_str(), L".dll") == 0) {
            if (!verifySignature(st.wstring())) { d.log("comp-update: Signatur-Fehler: " + it.path); return ""; }
        }
    }

    // --- 4) SWAP, alles oder nichts: Ziel -> .old, Staging -> Ziel; Fehler -> Rollback ---
    std::vector<std::wstring> swapped;   // Ziele mit vorhandener .old-Sicherung
    bool fail = false;
    for (auto& it : todo) {
        std::wstring target = appDir + L"\\" + std::wstring(it.path.begin(), it.path.end());
        ufs::create_directories(ufs::path(target).parent_path(), ec);
        std::wstring old = target + L".old";
        DeleteFileW(old.c_str());
        bool hadTarget = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (hadTarget && !MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { fail = true; break; }
        if (!MoveFileExW((stage / it.path).c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED)) {
            if (hadTarget) MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);   // diese eine sofort zurueck
            fail = true; break;
        }
        if (hadTarget) swapped.push_back(target);
    }
    if (fail) {   // ROLLBACK: alle bereits getauschten Ziele wiederherstellen
        for (auto& target : swapped) {
            std::wstring nu = target + L".new-failed";
            DeleteFileW(nu.c_str());
            MoveFileExW(target.c_str(), nu.c_str(), MOVEFILE_REPLACE_EXISTING);
            MoveFileExW((target + L".old").c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
            DeleteFileW(nu.c_str());
        }
        d.log("comp-update: Swap fehlgeschlagen -> Rollback, alter Stand intakt");
        return "";
    }
    ufs::remove_all(stage, ec);
    d.log("comp-update: " + mver + " angewendet (" + std::to_string(todo.size()) + " Datei(en))");
    d.notify("component-update-done", { {"version", mver}, {"files", (int)todo.size()} });
    return mver;
}

} // namespace luupd
