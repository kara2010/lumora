// UPnP-IGD-Modul (1:1-Portierung aus main.js): SSDP-Discovery (M-SEARCH mit
// Early-Exit nach erstem IGD-Fund + 250ms-Nachfrist), Geraete-Beschreibung ->
// controlURL (WANIPConnection/WANPPPConnection bzw. WANIPv6FirewallControl),
// SOAP: GetExternalIPAddress / AddPortMapping / DeletePortMapping / AddPinhole
// (LeaseTime 86400 - LeaseTime 0 lehnen FritzBoxen ab!) / DeletePinhole.
// Dazu bcPublicIPv6 (udp6-connect-Trick: liefert die TEMPORAERE Privacy-Adresse -
// nur die akzeptiert der Router als Pinhole-Ziel).
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <regex>
#include "artwork.h"   // luart::httpGet (funktioniert auch fuer http://)

namespace luupnp {

struct Ctrl { std::string controlURL, serviceType, localIp; };

inline std::string lanIpFor() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons(53); inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    std::string ip;
    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in loc{}; int ll = sizeof(loc);
        if (getsockname(s, (sockaddr*)&loc, &ll) == 0) { char b[64] = {}; inet_ntop(AF_INET, &loc.sin_addr, b, sizeof(b)); ip = b; }
    }
    closesocket(s);
    return ip;
}

// SSDP: M-SEARCH an 239.255.255.250:1900, LOCATION-URLs einsammeln (Early-Exit).
inline std::vector<std::string> discover(int timeoutMs) {
    std::vector<std::string> locs;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return locs;
    std::string localIp = lanIpFor();
    sockaddr_in bnd{}; bnd.sin_family = AF_INET;
    inet_pton(AF_INET, localIp.empty() ? "0.0.0.0" : localIp.c_str(), &bnd.sin_addr);
    bind(s, (sockaddr*)&bnd, sizeof(bnd));
    DWORD ttl = 4; setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons(1900); inet_pton(AF_INET, "239.255.255.250", &dst.sin_addr);
    for (const char* st : { "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANPPPConnection:1" }) {
        std::string m = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
        sendto(s, m.c_str(), (int)m.size(), 0, (sockaddr*)&dst, sizeof(dst));
    }
    ULONGLONG t0 = GetTickCount64(); ULONGLONG graceUntil = 0;
    char buf[4096];
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now - t0 > (ULONGLONG)timeoutMs) break;
        if (graceUntil && now > graceUntil) break;   // Early-Exit-Nachfrist abgelaufen
        DWORD to = 150; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = 0; std::string resp = buf;
        if (!std::regex_search(resp, std::regex("InternetGatewayDevice|WAN(IP|PPP)Connection", std::regex::icase))) continue;
        std::smatch m;
        if (std::regex_search(resp, m, std::regex("LOCATION:\\s*(\\S+)", std::regex::icase))) {
            std::string loc = m[1];
            bool dup = false; for (auto& l : locs) if (l == loc) { dup = true; break; }
            if (!dup) { locs.push_back(loc); if (!graceUntil) graceUntil = GetTickCount64() + 250; }
        }
    }
    closesocket(s);
    return locs;
}

// SOAP-POST an die controlURL (via WinHTTP; Ziel ist der Router im LAN, http).
inline luart::HttpResp soap(const std::string& controlURL, const std::string& serviceType, const std::string& action, const std::string& inner) {
    std::string xml = "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:" + action + " xmlns:u=\"" + serviceType + "\">" + inner + "</u:" + action + "></s:Body></s:Envelope>";
    luart::HttpResp r;
    std::wstring url = luart::toW(controlURL);
    URL_COMPONENTS uc{ sizeof(uc) }; wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return r;
    HINTERNET ses = WinHttpOpen(L"Lumora/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, host, uc.nPort, 0) : nullptr;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    if (req) {
        DWORD to = 4000; WinHttpSetTimeouts(req, to, to, to, to);
        std::wstring hdrs = L"Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + luart::toW(serviceType) + L"#" + luart::toW(action) + L"\"\r\n";
        if (WinHttpSendRequest(req, hdrs.c_str(), (DWORD)-1, (LPVOID)xml.data(), (DWORD)xml.size(), (DWORD)xml.size(), 0) && WinHttpReceiveResponse(req, nullptr)) {
            DWORD st = 0, sz = sizeof(st);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
            r.status = (int)st;
            for (;;) { DWORD avail = 0; if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
                size_t off = r.body.size(); r.body.resize(off + avail); DWORD got = 0;
                if (!WinHttpReadData(req, &r.body[off], avail, &got)) { r.body.resize(off); break; }
                r.body.resize(off + got); if (!got) break; }
        }
    }
    if (req) WinHttpCloseHandle(req); if (con) WinHttpCloseHandle(con); if (ses) WinHttpCloseHandle(ses);
    return r;
}

inline std::string xmlTag(const std::string& body, const std::string& tag) {
    std::smatch m;
    if (std::regex_search(body, m, std::regex("<" + tag + ">([^<]*)</" + tag + ">", std::regex::icase))) return m[1];
    return "";
}
// controlURL relativ -> absolut (gegen die LOCATION-Basis).
inline std::string absUrl(const std::string& base, const std::string& u) {
    if (u.rfind("http", 0) == 0) return u;
    size_t h = base.find("//"); size_t sl = h == std::string::npos ? std::string::npos : base.find('/', h + 2);
    std::string origin = sl == std::string::npos ? base : base.substr(0, sl);
    return origin + (u.rfind('/', 0) == 0 ? "" : "/") + u;
}

// IGD-controlURL aufloesen (einmalig gecacht; wie upnpResolveControl).
inline Ctrl* resolveControl() {
    static Ctrl ctrl; static bool tried = false;
    if (tried) return ctrl.controlURL.empty() ? nullptr : &ctrl;
    tried = true;
    for (auto& loc : discover(3000)) {
        auto desc = luart::httpGet(loc);
        if (desc.status != 200 || !std::regex_search(desc.body, std::regex("InternetGatewayDevice", std::regex::icase))) continue;
        static const std::regex svcRe("<service>([\\s\\S]*?)</service>");
        std::sregex_iterator it(desc.body.begin(), desc.body.end(), svcRe), end;
        for (; it != end; ++it) {
            std::string svc = (*it)[1];
            std::string type = xmlTag(svc, "serviceType"), cu = xmlTag(svc, "controlURL");
            if (type.empty() || cu.empty()) continue;
            if (!std::regex_search(type, std::regex("WAN(IP|PPP)Connection", std::regex::icase))) continue;
            ctrl = { absUrl(loc, cu), type, lanIpFor() };
            return &ctrl;
        }
    }
    return nullptr;
}
// IGDv2-IPv6-Firewall-controlURL (fuer Pinholes).
inline Ctrl* resolveV6Firewall() {
    static Ctrl ctrl; static bool tried = false;
    if (tried) return ctrl.controlURL.empty() ? nullptr : &ctrl;
    tried = true;
    for (auto& loc : discover(3000)) {
        auto desc = luart::httpGet(loc);
        if (desc.status != 200) continue;
        static const std::regex svcRe("<service>([\\s\\S]*?)</service>");
        std::sregex_iterator it(desc.body.begin(), desc.body.end(), svcRe), end;
        for (; it != end; ++it) {
            std::string svc = (*it)[1];
            if (!std::regex_search(svc, std::regex("WANIPv6FirewallControl", std::regex::icase))) continue;
            std::string type = xmlTag(svc, "serviceType"), cu = xmlTag(svc, "controlURL");
            if (type.empty() || cu.empty()) continue;
            ctrl = { absUrl(loc, cu), type, "" };
            return &ctrl;
        }
    }
    return nullptr;
}

inline std::string getExternalIp() {
    Ctrl* c = resolveControl(); if (!c) return "";
    auto r = soap(c->controlURL, c->serviceType, "GetExternalIPAddress", "");
    return xmlTag(r.body, "NewExternalIPAddress");
}
inline bool mapPort(int port, const char* proto, const char* desc) {
    Ctrl* c = resolveControl(); if (!c) return false;
    std::string inner = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>" + proto +
        "</NewProtocol><NewInternalPort>" + std::to_string(port) + "</NewInternalPort><NewInternalClient>" + c->localIp +
        "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" + desc + "</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>";
    return soap(c->controlURL, c->serviceType, "AddPortMapping", inner).status == 200;
}
inline void unmapPort(int port, const char* proto) {
    Ctrl* c = resolveControl(); if (!c) return;
    soap(c->controlURL, c->serviceType, "DeletePortMapping",
        "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>" + std::string(proto) + "</NewProtocol>");
}
// Globale Quell-IPv6 (temporaere Privacy-Adresse - nur die kennt der Router!).
inline std::string publicIPv6() {
    SOCKET s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";
    sockaddr_in6 dst{}; dst.sin6_family = AF_INET6; dst.sin6_port = htons(53);
    inet_pton(AF_INET6, "2001:4860:4860::8888", &dst.sin6_addr);
    std::string ip;
    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in6 loc{}; int ll = sizeof(loc);
        if (getsockname(s, (sockaddr*)&loc, &ll) == 0) { char b[64] = {}; inet_ntop(AF_INET6, &loc.sin6_addr, b, sizeof(b)); ip = b; }
    }
    closesocket(s);
    return (!ip.empty() && (ip[0] == '2' || ip[0] == '3')) ? ip : "";   // nur global unicast (2000::/3)
}
inline std::string addPinhole(const std::string& v6, int port, const char* proto) {
    Ctrl* c = resolveV6Firewall(); if (!c || v6.empty()) return "";
    int pr = strcmp(proto, "TCP") == 0 ? 6 : 17;
    std::string inner = "<RemoteHost></RemoteHost><RemotePort>0</RemotePort><InternalClient>" + v6 +
        "</InternalClient><InternalPort>" + std::to_string(port) + "</InternalPort><Protocol>" + std::to_string(pr) + "</Protocol><LeaseTime>86400</LeaseTime>";
    auto r = soap(c->controlURL, c->serviceType, "AddPinhole", inner);
    return r.status == 200 ? xmlTag(r.body, "UniqueID") : "";
}
inline void deletePinhole(const std::string& id) {
    Ctrl* c = resolveV6Firewall(); if (!c || id.empty()) return;
    soap(c->controlURL, c->serviceType, "DeletePinhole", "<UniqueID>" + id + "</UniqueID>");
}

} // namespace luupnp
