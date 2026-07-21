// LAN-Beacon fuer Gruppen (1:1 aus main.js groupLanBeacon*): Lumora-Instanzen im
// selben LAN finden sich per UDP-Broadcast (Port 8788) selbst, ohne Vermittlungs-
// server. Jede Instanz mit aktiver Gruppe sendet alle 4 s {lumora,group,name,id};
// alle Instanzen lauschen und melden fremde Gruppen an die UI (lan-groups).
// Ein gemeinsamer Socket macht Empfang + Senden; recvfrom mit 1s-Timeout treibt
// zugleich den 4s-Beacon. Der bind(8788) kann EINMALIG die Firewall-Nachfrage
// ausloesen. Eigenstaendiges Modul (nur ws2_32 + iphlpapi).
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include "json.hpp"
#pragma comment(lib, "iphlpapi.lib")

namespace lulan {
using nlohmann::json;
static const int LAN_PORT = 8788;
static SOCKET g_sock = INVALID_SOCKET;
static std::atomic<bool> g_run{ false };
static std::mutex g_mx;                                             // schuetzt g_groups
static std::map<std::string, std::pair<std::string, ULONGLONG>> g_groups;   // code -> {name, ts}
static std::mutex g_selfMx;                                         // schuetzt g_code/g_name/g_id
static std::string g_code, g_name, g_id;

// Gueltiger Raumcode: 6 Zeichen A-Z/2-9 (wie main.js).
inline bool validCode(const std::string& c) {
    if (c.size() != 6) return false;
    for (char ch : c) if (!((ch >= 'A' && ch <= 'Z') || (ch >= '2' && ch <= '9'))) return false;
    return true;
}
// Ziel-Adressen: globaler Broadcast + je IPv4-Interface der Subnetz-Broadcast
// (manche Netze/Treiber filtern 255.255.255.255, der gerichtete kommt durch).
inline std::vector<std::string> broadcastAddrs() {
    std::vector<std::string> out = { "255.255.255.255" };
    ULONG sz = 0; ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &sz) != ERROR_BUFFER_OVERFLOW || !sz) return out;
    std::vector<uint8_t> buf(sz);
    auto* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &sz) != NO_ERROR) return out;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sa = (sockaddr_in*)u->Address.lpSockaddr;
            if (!sa || sa->sin_family != AF_INET) continue;
            int prefix = u->OnLinkPrefixLength;
            if (prefix <= 0 || prefix > 32) continue;
            ULONG ip = ntohl(sa->sin_addr.s_addr);
            ULONG mask = (prefix == 32) ? 0xFFFFFFFFul : ~((1ul << (32 - prefix)) - 1);
            ULONG bc = ip | ~mask;
            in_addr ia; ia.s_addr = htonl(bc);
            char b[64] = {}; inet_ntop(AF_INET, &ia, b, sizeof(b));
            if (b[0]) out.push_back(b);
        }
    }
    return out;
}

// Empfangs- + Beacon-Thread: recvfrom (1s-Timeout) fuellt g_groups; alle 4 s
// wird der eigene Beacon an alle Broadcast-Adressen gesendet (wenn Gruppe aktiv).
inline void threadFn() {
    ULONGLONG lastSend = 0;
    while (g_run) {
        char buf[600]; sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
        if (n > 0) {
            std::string self;
            { std::lock_guard<std::mutex> lk(g_selfMx); self = g_id; }
            json d = json::parse(std::string(buf, n), nullptr, false);
            if (d.is_object() && d.value("lumora", 0) == 1 && d.contains("group")) {
                std::string id = d.value("id", "");
                std::string code = d.value("group", "");
                for (auto& ch : code) ch = (char)toupper((unsigned char)ch);
                if (id != self && validCode(code)) {
                    std::string name = d.value("name", std::string("PC"));
                    if (name.size() > 24) name.resize(24);
                    if (name.empty()) name = "PC";
                    std::lock_guard<std::mutex> lk(g_mx);
                    g_groups[code] = { name, GetTickCount64() };
                }
            }
        }
        ULONGLONG now = GetTickCount64();
        if (now - lastSend >= 4000) {
            lastSend = now;
            std::string code, name, id;
            { std::lock_guard<std::mutex> lk(g_selfMx); code = g_code; name = g_name; id = g_id; }
            if (!code.empty()) {
                json m = { {"lumora", 1}, {"group", code}, {"name", name}, {"id", id} };
                std::string s = m.dump();
                for (auto& addr : broadcastAddrs()) {
                    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons(LAN_PORT);
                    inet_pton(AF_INET, addr.c_str(), &dst.sin_addr);
                    sendto(g_sock, s.c_str(), (int)s.size(), 0, (sockaddr*)&dst, sizeof(dst));
                }
            }
        }
    }
}

// Empfangs-Socket + Thread einmalig starten (beim App-Start; loest ggf. die
// Firewall-Nachfrage aus). Idempotent.
inline void listenStart() {
    if (g_sock != INVALID_SOCKET) return;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&yes, sizeof(yes));
    DWORD tmo = 1000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tmo, sizeof(tmo));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(LAN_PORT);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return; }
    g_sock = s; g_run = true;
    std::thread(threadFn).detach();
}
// Eigene Gruppe bewerben (bei group-start/join). Aktiviert zugleich den Empfang.
inline void beaconStart(const std::string& code, const std::string& name, const std::string& id) {
    { std::lock_guard<std::mutex> lk(g_selfMx); g_code = code; g_name = name; g_id = id; }
    listenStart();
}
// Beacon einstellen (bei group-leave): nur nicht mehr senden, weiter lauschen.
inline void beaconStop() { std::lock_guard<std::mutex> lk(g_selfMx); g_code.clear(); }

// Im LAN sichtbare fremde Gruppen (veraltete >12 s raus), {code,name}.
inline std::vector<std::pair<std::string, std::string>> groups() {
    std::vector<std::pair<std::string, std::string>> out;
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto it = g_groups.begin(); it != g_groups.end(); ) {
        if (now - it->second.second > 12000) it = g_groups.erase(it);
        else { out.push_back({ it->first, it->second.first }); ++it; }
    }
    return out;
}

} // namespace lulan
