#include "ingest.h"
#include "ts_demux.h"
#include <winsock2.h>
#include <ws2tcpip.h>

bool Ingest::start(uint16_t port, TsDemux* demux) {
    demux_ = demux;
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    // grosser Empfangspuffer wie bei mediamtx (udpReadBufferSize 25MB): Burst-Schutz bei hohen Bitraten
    int rcvbuf = 26214400;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return false; }
    sock_ = (uintptr_t)s;
    run_ = true;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void Ingest::stop() {
    run_ = false;
    if (sock_ != ~(uintptr_t)0) { closesocket((SOCKET)sock_); sock_ = ~(uintptr_t)0; }
    if (thread_.joinable()) thread_.join();
}

void Ingest::loop() {
    // Muxer sendet 1316-Byte-Happen (7x188); Puffer grosszuegig fuer Fremd-Sender
    char buf[65536];
    while (run_) {
        int n = recv((SOCKET)sock_, buf, sizeof(buf), 0);
        if (n <= 0) { if (!run_) break; continue; }
        ++datagrams;
        demux_->feed((const uint8_t*)buf, (size_t)n);
    }
}
