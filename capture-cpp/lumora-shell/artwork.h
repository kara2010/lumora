// Artwork-/Netzwerk-Modul (1:1-Portierung aus main.js inkl. aller Detail-Fixes):
// - Steam: storesearch + IStoreBrowseService/GetItems (gehashte Asset-URLs!) + CDN-Fallbacks
// - MS Store: storeedgefd-Suche (Poster/BoxArt, w=600&h=900-Resize mit '?'-Regel)
// - SteamGridDB: autocomplete + grids/heroes (Bearer-Key aus den App-Einstellungen)
// - Namensabgleich: cleanGameName (Editions-Suffix!) + closeNameMatch (0.7-Schwelle) -
//   NIE blind "erstes Ergebnis" uebernehmen (Cover-Bug-Lehre).
// Transport: WinHTTP, folgt Redirects, User-Agent "Lumora/1.0". Bilder als data-URLs.
#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <regex>
#include "json.hpp"
#include "scan_games.h"   // lushell::toUtf8
#pragma comment(lib, "winhttp.lib")

namespace luart {
using nlohmann::json;
using lushell::toUtf8;

inline std::wstring toW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
inline std::string b64(const uint8_t* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t x = (uint32_t)d[i] << 16; if (i + 1 < n) x |= (uint32_t)d[i + 1] << 8; if (i + 2 < n) x |= d[i + 2];
        o.push_back(t[(x >> 18) & 63]); o.push_back(t[(x >> 12) & 63]);
        o.push_back(i + 1 < n ? t[(x >> 6) & 63] : '='); o.push_back(i + 2 < n ? t[x & 63] : '=');
    }
    return o;
}
// encodeURIComponent-Aequivalent (UTF-8, unreserved bleiben)
inline std::string urlEnc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF"; std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.push_back((char)c);
        else { o.push_back('%'); o.push_back(hex[c >> 4]); o.push_back(hex[c & 15]); }
    }
    return o;
}

struct HttpResp { int status = 0; std::string body; };
// GET mit Redirect-Folge (WinHTTP-Default) + optionalen Zusatz-Headern + Timeout
// (Default 15s; kurze Werte fuer Erreichbarkeits-Proben wie den /instanz-Hairpin-Check,
// der sonst mit dem 60s-WinHTTP-Default die ganze Router-Phase blockiert).
inline HttpResp httpGet(const std::string& url8, const std::wstring& extraHeaders = L"", int timeoutMs = 15000) {
    HttpResp r; std::wstring url = toW(url8);
    URL_COMPONENTS uc{ sizeof(uc) }; wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255; uc.lpszUrlPath = path; uc.dwUrlPathLength = 4095;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return r;
    HINTERNET ses = WinHttpOpen(L"Lumora/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return r;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (req) WinHttpSetTimeouts(req, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    if (req && WinHttpSendRequest(req, extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str(),
        extraHeaders.empty() ? 0 : (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD st = 0, sz = sizeof(st);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
        r.status = (int)st;
        for (;;) { DWORD avail = 0; if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
            size_t off = r.body.size(); r.body.resize(off + avail); DWORD got = 0;
            if (!WinHttpReadData(req, &r.body[off], avail, &got)) { r.body.resize(off); break; }
            r.body.resize(off + got); if (!got) break; }
    }
    if (req) WinHttpCloseHandle(req); if (con) WinHttpCloseHandle(con); if (ses) WinHttpCloseHandle(ses);
    return r;
}

// ---- Namensabgleich (1:1 aus main.js) ----
inline std::string cleanGameName(const std::string& name) {
    std::string s = std::regex_replace(name, std::regex("[\xE2\x84\xA2\xC2\xAE\xC2\xA9]"), " ");   // TM/(R)/(C) als UTF-8-Bytes
    s = std::regex_replace(s, std::regex("\\s+(deluxe|ultimate|gold|goty|definitive|complete|standard|premium|legendary|anniversary)\\s+edition\\s*$", std::regex::icase), "");
    s = std::regex_replace(s, std::regex("\\s+edition\\s*$", std::regex::icase), "");
    s = std::regex_replace(s, std::regex("\\s+"), " ");
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}
inline std::string normName(const std::string& s) {
    std::string o; for (char c : s) { if (isalnum((unsigned char)c)) o.push_back((char)tolower((unsigned char)c)); }
    return o;
}
inline bool closeNameMatch(const std::string& a, const std::string& b) {
    std::string x = normName(a), y = normName(b);
    if (x.empty() || y.empty()) return false;
    if (x == y) return true;
    if (x.size() < 4 || y.size() < 4) return false;
    if (x.find(y) == std::string::npos && y.find(x) == std::string::npos) return false;
    return (double)(std::min)(x.size(), y.size()) / (double)(std::max)(x.size(), y.size()) >= 0.7;
}

// ---- Bild-Downloads (Magic-Bytes + Mindestgroessen wie main.js) ----
inline json fetchFirstImage(const std::vector<std::string>& urls) {   // Steam: echtes JPEG > 5000 B (Platzhalter-Falle)
    for (auto& u : urls) { if (u.empty()) continue;
        HttpResp r = httpGet(u); const std::string& b = r.body;
        if (r.status == 200 && b.size() > 5000 && (uint8_t)b[0] == 0xFF && (uint8_t)b[1] == 0xD8)
            return "data:image/jpeg;base64," + b64((const uint8_t*)b.data(), b.size());
    }
    return nullptr;
}
inline json fetchAnyImage(const std::string& u) {   // SGDB: JPEG/PNG/WebP > 1500 B
    HttpResp r = httpGet(u); const std::string& b = r.body;
    if (r.status != 200 || b.size() < 1500) return nullptr;
    std::string mime;
    if ((uint8_t)b[0] == 0xFF && (uint8_t)b[1] == 0xD8) mime = "image/jpeg";
    else if ((uint8_t)b[0] == 0x89 && (uint8_t)b[1] == 0x50) mime = "image/png";
    else if ((uint8_t)b[0] == 0x52 && (uint8_t)b[1] == 0x49 && b.size() > 9 && (uint8_t)b[8] == 0x57 && (uint8_t)b[9] == 0x45) mime = "image/webp";
    if (mime.empty()) return nullptr;
    return "data:" + mime + ";base64," + b64((const uint8_t*)b.data(), b.size());
}
inline json fetchImageUrl(const std::string& u) {   // "eigenes Bild": JPEG/PNG > 1000 B
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) return nullptr;
    HttpResp r = httpGet(u); const std::string& b = r.body;
    if (r.status == 200 && b.size() > 1000) {
        if ((uint8_t)b[0] == 0xFF && (uint8_t)b[1] == 0xD8) return "data:image/jpeg;base64," + b64((const uint8_t*)b.data(), b.size());
        if ((uint8_t)b[0] == 0x89 && (uint8_t)b[1] == 0x50) return "data:image/png;base64," + b64((const uint8_t*)b.data(), b.size());
    }
    return nullptr;
}

// ---- Steam ----
static const char* STEAM_ASSET_BASE = "https://shared.akamai.steamstatic.com/store_item_assets/";
inline std::vector<std::string> steamCoverUrls(const std::string& appId) {
    return { "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/" + appId + "/library_600x900.jpg",
             "https://cdn.akamai.steamstatic.com/steam/apps/" + appId + "/library_600x900.jpg" };
}
// ECHTE Asset-URLs (Content-Hash!) ueber die offizielle Store-API; map appId -> {name,cover,hero,isDlc}
inline json steamGetItems(const std::vector<std::string>& appIds) {
    json map = json::object();
    if (appIds.empty()) return map;
    json ids = json::array(); for (auto& a : appIds) ids.push_back({ {"appid", atoll(a.c_str())} });
    json input = { {"ids", ids}, {"context", {{"language","english"},{"country_code","US"}}},
                   {"data_request", {{"include_assets",true},{"include_basic_info",true}}} };
    HttpResp r = httpGet("https://api.steampowered.com/IStoreBrowseService/GetItems/v1/?input_json=" + urlEnc(input.dump()));
    if (r.status != 200) return map;
    json j = json::parse(r.body, nullptr, false); if (!j.is_object()) return map;
    for (auto& it : j.value("response", json::object()).value("store_items", json::array())) {
        json a = it.value("assets", json::object());
        std::string fmt = a.value("asset_url_format", "");
        auto mk = [&](const std::string& f) -> json {
            if (fmt.empty() || f.empty()) return nullptr;
            std::string u = fmt; size_t p = u.find("${FILENAME}"); if (p != std::string::npos) u.replace(p, 11, f);
            return std::string(STEAM_ASSET_BASE) + u;
        };
        bool isDlc = it.value("type", 0) != 0 || (it.contains("related_items") && it["related_items"].contains("parent_appid"));
        map[std::to_string(it.value("id", 0ll))] = { {"name", it.value("name","")}, {"cover", mk(a.value("library_capsule",""))}, {"hero", mk(a.value("library_hero",""))}, {"isDlc", isDlc} };
    }
    return map;
}
inline json steamSearch(const std::string& term) {   // storesearch-Rohtreffer [{appId,name}]
    HttpResp r = httpGet("https://store.steampowered.com/api/storesearch/?term=" + urlEnc(cleanGameName(term)) + "&l=english&cc=US");
    if (r.status != 200) return json::array();
    json j = json::parse(r.body, nullptr, false);
    json out = json::array();
    if (j.is_object()) for (auto& i : j.value("items", json::array()))
        out.push_back({ {"appId", std::to_string(i.value("id", 0ll))}, {"name", i.value("name","")} });
    return out;
}
inline std::string resolveSteamAppId(const std::string& gameName) {
    std::string cleaned = cleanGameName(gameName), target = normName(cleaned);
    if (target.empty()) return "";
    json items = steamSearch(gameName);
    for (auto& i : items) if (normName(i.value("name", "")) == target) return i.value("appId", "");
    for (auto& i : items) if (closeNameMatch(cleanGameName(i.value("name", "")), cleaned)) return i.value("appId", "");
    return "";   // KEIN "erstes Ergebnis"-Fallback (Cover-Bug-Lehre)
}
inline json fetchCoverSteam(const std::string& gameName, std::string appId) {
    if (appId.empty()) appId = resolveSteamAppId(gameName);
    if (appId.empty()) return nullptr;
    json a = steamGetItems({ appId }).value(appId, json::object());
    std::vector<std::string> urls;
    if (a.value("cover", json()).is_string()) urls.push_back(a["cover"].get<std::string>());
    for (auto& u : steamCoverUrls(appId)) urls.push_back(u);
    return fetchFirstImage(urls);
}
inline json fetchSteamHero(const std::string& gameName, std::string appId) {
    if (appId.empty()) appId = resolveSteamAppId(gameName);
    if (appId.empty()) return nullptr;
    json a = steamGetItems({ appId }).value(appId, json::object());
    std::vector<std::string> urls;
    if (a.value("hero", json()).is_string()) urls.push_back(a["hero"].get<std::string>());
    urls.push_back("https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/" + appId + "/library_hero.jpg");
    urls.push_back("https://cdn.akamai.steamstatic.com/steam/apps/" + appId + "/library_hero.jpg");
    return fetchFirstImage(urls);
}
inline json searchSteamArt(const std::string& term, bool wantHero) {
    std::string cleaned = cleanGameName(term);
    json steam = json::array();
    for (auto& c : steamSearch(term)) if (closeNameMatch(cleanGameName(c.value("name", "")), cleaned)) { steam.push_back(c); if (steam.size() >= 12) break; }
    std::vector<std::string> ids; for (auto& c : steam) ids.push_back(c.value("appId", ""));
    json assets = ids.empty() ? json::object() : steamGetItems(ids);
    json out = json::array();
    for (auto& c : steam) {
        std::string id = c.value("appId", "");
        json a = assets.value(id, json());
        if (!a.is_object() || a.value("isDlc", false)) continue;   // DLCs ausblenden
        if (wantHero) {
            std::vector<std::string> hu;
            if (a.value("hero", json()).is_string()) hu.push_back(a["hero"].get<std::string>());
            hu.push_back(std::string(STEAM_ASSET_BASE) + "steam/apps/" + id + "/library_hero.jpg");
            std::vector<std::string> seen;
            for (auto& u : hu) { json img = fetchFirstImage({ u });
                if (img.is_string()) { std::string s = img.get<std::string>();
                    bool dup = false; for (auto& sv : seen) if (sv == s) { dup = true; break; }
                    if (!dup) { seen.push_back(s); out.push_back({ {"source","Steam"},{"appId",id},{"name",c.value("name","")},{"cover",img} }); } } }
        } else {
            std::vector<std::string> cu;
            if (a.value("cover", json()).is_string()) cu.push_back(a["cover"].get<std::string>());
            for (auto& u : steamCoverUrls(id)) cu.push_back(u);
            json img = fetchFirstImage(cu);
            if (img.is_string()) out.push_back({ {"source","Steam"},{"appId",id},{"name",c.value("name","")},{"cover",img} });
        }
    }
    return out;
}

// ---- MS Store ----
inline json msStoreCandidates(const std::string& term) {
    HttpResp r = httpGet("https://storeedgefd.dsx.mp.microsoft.com/v9.0/search?query=" + urlEnc(cleanGameName(term)) + "&market=US&locale=en-us&deviceFamily=Windows.Desktop");
    if (r.status != 200) return json::array();
    json j = json::parse(r.body, nullptr, false);
    json out = json::array();
    if (!j.is_object()) return out;
    for (auto& res : j.value("Payload", json::object()).value("SearchResults", json::array())) {
        json imgs = res.value("Images", json::array()); json pick = nullptr;
        for (auto& i : imgs) if (i.value("ImageType", "") == "Poster") { pick = i; break; }
        if (pick.is_null()) for (auto& i : imgs) if (i.value("ImageType", "") == "BoxArt") { pick = i; break; }
        if (pick.is_null() && imgs.size()) pick = imgs[0];
        if (pick.is_object() && !pick.value("Url", "").empty())
            out.push_back({ {"name", res.value("Title","")}, {"imageUrl", pick.value("Url","")} });
    }
    return out;
}
inline std::string msImageUrl(const std::string& url) {   // Resize mit '?' anhaengen ('&' ohne Query = Server-500)
    return url + (url.find('?') != std::string::npos ? "&" : "?") + "w=600&h=900&format=jpg";
}
inline json fetchMsImage(const std::string& url) {
    HttpResp r = httpGet(msImageUrl(url));
    if (!(r.status == 200 && r.body.size() > 3000)) r = httpGet(url);
    if (r.status == 200 && r.body.size() > 3000)
        return "data:image/jpeg;base64," + b64((const uint8_t*)r.body.data(), r.body.size());
    return nullptr;
}
inline json fetchCoverMSStore(const std::string& gameName) {
    std::string cleaned = cleanGameName(gameName);
    for (auto& c : msStoreCandidates(gameName))
        if (closeNameMatch(cleanGameName(c.value("name", "")), cleaned)) return fetchMsImage(c.value("imageUrl", ""));
    return nullptr;   // Namensabgleich Pflicht (Editions-Suffix-Befund 2026-07-17)
}
inline json searchMsArt(const std::string& term) {
    std::string cleaned = cleanGameName(term);
    json out = json::array(); int taken = 0;
    for (auto& c : msStoreCandidates(term)) {
        if (taken >= 4) break;
        if (!closeNameMatch(cleanGameName(c.value("name", "")), cleaned)) continue;
        ++taken;
        json cover = fetchMsImage(c.value("imageUrl", ""));
        if (cover.is_string()) out.push_back({ {"source","Microsoft Store"},{"name",c.value("name","")},{"cover",cover} });
    }
    return out;
}

// ---- SteamGridDB (Bearer-Key) ----
inline json sgdbApi(const std::string& pathQuery, const std::string& key) {
    HttpResp r = httpGet("https://www.steamgriddb.com/api/v2" + pathQuery, L"Authorization: Bearer " + toW(key));
    if (r.status != 200) return nullptr;
    json j = json::parse(r.body, nullptr, false);
    return (j.is_object() && j.value("success", false)) ? j.value("data", json()) : json(nullptr);
}
inline json sgdbArtwork(const std::string& term, const std::string& kind, const std::string& key) {
    json out = json::array();
    if (key.empty()) return out;
    std::string cleaned = cleanGameName(term);
    json gamesRaw = sgdbApi("/search/autocomplete/" + urlEnc(cleaned), key);
    if (!gamesRaw.is_array() || gamesRaw.empty()) return out;
    json games = json::array();
    for (auto& g : gamesRaw) if (closeNameMatch(cleanGameName(g.value("name", "")), cleaned)) games.push_back(g);
    int gi = 0;
    for (auto& g : games) {
        if (++gi > 2) break;   // bis zu 2 passende Spiele
        std::string ep = (kind == "hero")
            ? "/heroes/game/" + std::to_string(g.value("id", 0ll)) + "?dimensions=1920x620,3840x1240&types=static&nsfw=false&humor=false&limit=8"
            : "/grids/game/" + std::to_string(g.value("id", 0ll)) + "?dimensions=600x900,660x930&types=static&nsfw=false&humor=false&limit=8";
        json items = sgdbApi(ep, key);
        if (!items.is_array()) continue;
        for (auto& it : items) {
            if (out.size() >= 8) break;   // Gesamt-Limit gegen zu grosse Downloads
            std::string u = it.value("url", ""); if (u.empty()) continue;
            json img = fetchAnyImage(u);
            if (img.is_string()) out.push_back({ {"source","SteamGridDB"},{"name",g.value("name","")},{"cover",img} });
        }
    }
    return out;
}

// ---- Spiel-Infos (Steam appdetails, deutsch) ----
inline json fetchGameInfo(const std::string& gameName, std::string appId) {
    if (appId.empty()) appId = resolveSteamAppId(gameName);
    if (appId.empty()) return nullptr;
    HttpResp r = httpGet("https://store.steampowered.com/api/appdetails?appids=" + appId + "&l=german");
    if (r.status != 200) return nullptr;
    json j = json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains(appId)) return nullptr;
    json d = j[appId].value("data", json());
    if (!d.is_object()) return nullptr;
    std::string year, date = d.value("release_date", json::object()).value("date", "");
    std::smatch m; if (std::regex_search(date, m, std::regex("\\d{4}"))) year = m[0];
    json genres = json::array(); int gc = 0;
    for (auto& g : d.value("genres", json::array())) { if (gc++ >= 4) break; genres.push_back(g.value("description", "")); }
    json devs = d.value("developers", json::array());
    return { {"description", d.value("short_description","")}, {"genres", genres}, {"releaseYear", year},
             {"developer", devs.size() ? devs[0].get<std::string>() : ""} };
}

} // namespace luart
