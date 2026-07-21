// Minimaler blockierender HTTP/1.1-Server (WinSock, nur 127.0.0.1) fuer WHEP- und
// Control-Endpunkte. Ein Accept-Thread + ein Thread pro Verbindung (Verbindungen sind
// kurzlebig: WHEP-POST/DELETE, API-Polls). Kein TLS - alles hinter der Shell (8787).
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

struct HttpReq {
    std::string method, path, query, body;
    std::map<std::string, std::string> headers;   // Namen lowercase
};
struct HttpResp {
    int status = 200;
    std::string contentType = "application/json";
    std::map<std::string, std::string> headers;
    std::string body;
};

class HttpMini {
public:
    using Handler = std::function<void(const HttpReq&, HttpResp&)>;

    // localOnly: an 127.0.0.1 binden (Standard - Shell proxied von aussen)
    bool start(uint16_t port, Handler handler, bool localOnly = true) {
        handler_ = std::move(handler);
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) return false;
        BOOL yes = TRUE; setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
        a.sin_addr.s_addr = localOnly ? htonl(INADDR_LOOPBACK) : INADDR_ANY;
        if (bind(sock_, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(sock_); sock_ = INVALID_SOCKET; return false; }
        if (listen(sock_, 16) != 0) { closesocket(sock_); sock_ = INVALID_SOCKET; return false; }
        run_ = true;
        acceptThread_ = std::thread([this] { acceptLoop(); });
        return true;
    }
    void stop() {
        run_ = false;
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
        if (acceptThread_.joinable()) acceptThread_.join();
    }
    ~HttpMini() { stop(); }

private:
    void acceptLoop() {
        while (run_) {
            SOCKET c = accept(sock_, nullptr, nullptr);
            if (c == INVALID_SOCKET) { if (!run_) break; continue; }
            std::thread([this, c] { handleConn(c); }).detach();
        }
    }
    void handleConn(SOCKET c) {
        DWORD to = 10000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
        std::string buf;
        // Header lesen
        for (;;) {
            char tmp[4096];
            int n = recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) { closesocket(c); return; }
            buf.append(tmp, n);
            if (buf.find("\r\n\r\n") != std::string::npos) break;
            if (buf.size() > 1 << 20) { closesocket(c); return; }
        }
        size_t hdrEnd = buf.find("\r\n\r\n");
        HttpReq req;
        {   // Request-Line + Header parsen
            size_t lineEnd = buf.find("\r\n");
            std::string line = buf.substr(0, lineEnd);
            size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) { closesocket(c); return; }
            req.method = line.substr(0, sp1);
            std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            size_t q = target.find('?');
            req.path = q == std::string::npos ? target : target.substr(0, q);
            req.query = q == std::string::npos ? "" : target.substr(q + 1);
            size_t pos = lineEnd + 2;
            while (pos < hdrEnd) {
                size_t e = buf.find("\r\n", pos);
                std::string h = buf.substr(pos, e - pos);
                size_t col = h.find(':');
                if (col != std::string::npos) {
                    std::string k = h.substr(0, col);
                    for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
                    size_t v = col + 1; while (v < h.size() && h[v] == ' ') ++v;
                    req.headers[k] = h.substr(v);
                }
                pos = e + 2;
            }
        }
        size_t clen = 0;
        auto it = req.headers.find("content-length");
        if (it != req.headers.end()) clen = (size_t)atoll(it->second.c_str());
        if (clen > 4u << 20) { closesocket(c); return; }
        req.body = buf.substr(hdrEnd + 4);
        while (req.body.size() < clen) {
            char tmp[4096];
            int n = recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            req.body.append(tmp, n);
        }

        HttpResp resp;
        try { handler_(req, resp); } catch (...) { resp = HttpResp{}; resp.status = 500; resp.body = "{}"; }

        const char* st = resp.status == 200 ? "OK" : resp.status == 201 ? "Created" :
                         resp.status == 204 ? "No Content" : resp.status == 404 ? "Not Found" :
                         resp.status == 400 ? "Bad Request" : resp.status == 405 ? "Method Not Allowed" : "Error";
        std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + st + "\r\n";
        // CORS wie mediamtx (Player laeuft normalerweise same-origin hinter der Shell,
        // aber Vorschau/Tools duerfen direkt zugreifen)
        out += "Access-Control-Allow-Origin: *\r\n";
        out += "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n";
        out += "Access-Control-Allow-Headers: Content-Type, If-Match\r\n";
        out += "Access-Control-Expose-Headers: Location, Link\r\n";
        out += "Content-Type: " + resp.contentType + "\r\n";
        out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
        out += "Connection: close\r\n";
        for (auto& [k, v] : resp.headers) out += k + ": " + v + "\r\n";
        out += "\r\n" + resp.body;
        send(c, out.data(), (int)out.size(), 0);
        shutdown(c, SD_SEND); closesocket(c);
    }
    SOCKET sock_ = INVALID_SOCKET;
    std::thread acceptThread_;
    std::atomic<bool> run_{ false };
    Handler handler_;
};
