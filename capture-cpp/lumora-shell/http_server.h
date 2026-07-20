// Streaming-HTTP-Server (Architektur-Entscheidung "A", 2026-07-20): 1:1-Ersatz des
// Node-http-Servers aus main.js auf Port 8787. Eigener schlanker Winsock-HTTP/1.1-
// Server (http.sys braeuchte Admin-URL-Reservierungen; Node lauscht ebenfalls roh).
// Ein Thread je Verbindung (Zuschauerzahlen klein), Connection: close je Request.
// SSE-Verbindungen (switch-events) uebernimmt der SseHub dauerhaft (8s-Heartbeat
// gegen NAT-Idle-Abriss - Log-belegte 5-min-Falle aus dem Electron-Betrieb).
// Der WHEP-Proxy laeuft ueber WinHTTP (Location-Header-Rewrite wie das Original).
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <mutex>
#include <thread>
#include <functional>
#pragma comment(lib, "ws2_32.lib")

namespace lusrv {

struct Request {
    std::string method, path, query;           // /whep?name=x -> path=/whep query=name=x
    std::map<std::string, std::string> headers;   // Schluessel lowercase
    std::string body, clientIp;
};
struct Response {
    int status = 404;
    std::map<std::string, std::string> headers;   // zusaetzlich zu CORS/Standard
    std::string body;
    bool takeoverSse = false;                  // Route uebernimmt den Socket als SSE-Client
};
using Handler = std::function<Response(const Request&)>;

// ---- SSE-Hub: langlebige Zuschauer-Sockets + Broadcast + Heartbeat ----
class SseHub {
public:
    void add(SOCKET s) { std::lock_guard<std::mutex> lk(mx_); clients_.insert(s); }
    void broadcast(const std::string& line) { sendAll(line); }
    void start() {
        if (hb_.joinable()) return;
        run_ = true;
        hb_ = std::thread([this]() { while (run_) { for (int i = 0; i < 80 && run_; ++i) Sleep(100); if (run_) sendAll(": hb\n\n"); } });
    }
    void stop() {
        run_ = false; if (hb_.joinable()) hb_.join();
        std::lock_guard<std::mutex> lk(mx_);
        for (SOCKET s : clients_) closesocket(s);
        clients_.clear();
    }
private:
    void sendAll(const std::string& line) {
        std::lock_guard<std::mutex> lk(mx_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (send(*it, line.c_str(), (int)line.size(), 0) == SOCKET_ERROR) { closesocket(*it); it = clients_.erase(it); }
            else ++it;
        }
    }
    std::set<SOCKET> clients_; std::mutex mx_; std::thread hb_; volatile bool run_ = false;
};

// ---- WHEP-Proxy-Transport: beliebige Methode + Body an 127.0.0.1:<port>, liefert
// Status/Body/wichtige Antwort-Header (Location/Content-Type/ETag) zurueck. ----
struct ProxyResult { int status = 0; std::string body; std::map<std::string, std::string> headers; };
inline ProxyResult proxyLocal(int port, const std::string& method, const std::string& pathQuery,
                              const std::string& contentType, const std::string& userAgent, const std::string& body) {
    ProxyResult r;
    HINTERNET ses = WinHttpOpen(L"Lumora/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return r;
    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", (INTERNET_PORT)port, 0);
    std::wstring wpath(pathQuery.begin(), pathQuery.end());
    std::wstring wmethod(method.begin(), method.end());
    HINTERNET req = con ? WinHttpOpenRequest(con, wmethod.c_str(), wpath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    if (req) {
        std::wstring hdrs;
        if (!contentType.empty()) hdrs += L"Content-Type: " + std::wstring(contentType.begin(), contentType.end()) + L"\r\n";
        if (!userAgent.empty()) hdrs += L"User-Agent: " + std::wstring(userAgent.begin(), userAgent.end()) + L"\r\n";
        if (WinHttpSendRequest(req, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(), hdrs.empty() ? 0 : (DWORD)-1,
                (LPVOID)(body.empty() ? nullptr : body.data()), (DWORD)body.size(), (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD st = 0, sz = sizeof(st);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
            r.status = (int)st;
            auto qh = [&](const wchar_t* name, const char* out) {
                wchar_t buf[2048] = {}; DWORD bsz = sizeof(buf);
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, buf, &bsz, WINHTTP_NO_HEADER_INDEX)) {
                    std::wstring w = buf; r.headers[out] = std::string(w.begin(), w.end());
                }
            };
            qh(L"Location", "Location"); qh(L"Content-Type", "Content-Type"); qh(L"ETag", "ETag"); qh(L"Link", "Link");
            for (;;) { DWORD avail = 0; if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
                size_t off = r.body.size(); r.body.resize(off + avail); DWORD got = 0;
                if (!WinHttpReadData(req, &r.body[off], avail, &got)) { r.body.resize(off); break; }
                r.body.resize(off + got); if (!got) break; }
        }
    }
    if (req) WinHttpCloseHandle(req); if (con) WinHttpCloseHandle(con); if (ses) WinHttpCloseHandle(ses);
    return r;
}

// ---- Der Server selbst ----
class HttpServer {
public:
    SseHub sse;
    bool start(int port, Handler handler) {
        handler_ = handler;
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
        lsock_ = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);   // Dual-Stack (IPv6 + IPv4 wie Node)
        DWORD off = 0; setsockopt(lsock_, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof(off));
        sockaddr_in6 a{}; a.sin6_family = AF_INET6; a.sin6_addr = in6addr_any; a.sin6_port = htons((u_short)port);
        if (bind(lsock_, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR || listen(lsock_, 32) == SOCKET_ERROR) { closesocket(lsock_); lsock_ = INVALID_SOCKET; return false; }
        run_ = true;
        sse.start();
        accept_ = std::thread([this]() {
            while (run_) {
                sockaddr_in6 ca{}; int cl = sizeof(ca);
                SOCKET c = accept(lsock_, (sockaddr*)&ca, &cl);
                if (c == INVALID_SOCKET) { if (run_) continue; break; }
                char ipb[64] = {}; InetNtopA(AF_INET6, &ca.sin6_addr, ipb, sizeof(ipb));
                std::string ip = ipb;
                if (ip.rfind("::ffff:", 0) == 0) ip = ip.substr(7);   // IPv4-mapped normalisieren
                std::thread(&HttpServer::serve, this, c, ip).detach();
            }
        });
        return true;
    }
    void stop() {
        run_ = false;
        if (lsock_ != INVALID_SOCKET) { closesocket(lsock_); lsock_ = INVALID_SOCKET; }
        if (accept_.joinable()) accept_.join();
        sse.stop();
    }
private:
    void serve(SOCKET c, std::string ip) {
        DWORD to = 15000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
        std::string buf; char tmp[8192];
        size_t hdrEnd;
        for (;;) {   // bis Header-Ende lesen
            hdrEnd = buf.find("\r\n\r\n");
            if (hdrEnd != std::string::npos) break;
            int n = recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0 || buf.size() > 64 * 1024) { closesocket(c); return; }
            buf.append(tmp, n);
        }
        Request rq; rq.clientIp = ip;
        {   // Request-Zeile + Header parsen
            size_t lineEnd = buf.find("\r\n");
            std::string line = buf.substr(0, lineEnd);
            size_t s1 = line.find(' '), s2 = line.rfind(' ');
            if (s1 == std::string::npos || s2 <= s1) { closesocket(c); return; }
            rq.method = line.substr(0, s1);
            std::string url = line.substr(s1 + 1, s2 - s1 - 1);
            size_t q = url.find('?');
            rq.path = q == std::string::npos ? url : url.substr(0, q);
            rq.query = q == std::string::npos ? "" : url.substr(q + 1);
            size_t p = lineEnd + 2;
            while (p < hdrEnd) {
                size_t e = buf.find("\r\n", p); if (e == std::string::npos || e > hdrEnd) break;
                std::string h = buf.substr(p, e - p); p = e + 2;
                size_t col = h.find(':'); if (col == std::string::npos) continue;
                std::string k = h.substr(0, col); for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
                size_t vs = h.find_first_not_of(' ', col + 1);
                rq.headers[k] = vs == std::string::npos ? "" : h.substr(vs);
            }
        }
        // Body (Content-Length, Limit 300 KB wie das Original)
        size_t clen = 0; auto it = rq.headers.find("content-length");
        if (it != rq.headers.end()) clen = (size_t)atoll(it->second.c_str());
        if (clen > 300000) { closesocket(c); return; }
        rq.body = buf.substr(hdrEnd + 4);
        while (rq.body.size() < clen) {
            int n = recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) { closesocket(c); return; }
            rq.body.append(tmp, n);
        }
        // CORS-Preflight direkt hier (wie das Original ganz oben)
        Response rs;
        if (rq.method == "OPTIONS") rs.status = 204;
        else { try { rs = handler_(rq); } catch (...) { rs.status = 500; rs.body = "error"; } }
        std::string out = "HTTP/1.1 " + std::to_string(rs.status) + " X\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n";
        if (rs.takeoverSse) {
            out += "Content-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n\r\n:ok\n\n";
            send(c, out.c_str(), (int)out.size(), 0);
            sse.add(c);                                    // Hub uebernimmt (Heartbeat/Broadcast/Close)
            return;
        }
        for (auto& [k, v] : rs.headers) out += k + ": " + v + "\r\n";
        out += "Content-Length: " + std::to_string(rs.body.size()) + "\r\nConnection: close\r\n\r\n";
        out += rs.body;
        send(c, out.c_str(), (int)out.size(), 0);
        closesocket(c);
    }
    Handler handler_; SOCKET lsock_ = INVALID_SOCKET; std::thread accept_; volatile bool run_ = false;
};

} // namespace lusrv
