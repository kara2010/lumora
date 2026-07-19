// Transport-PoC: FFmpeg-Ersatz. Liest eine H.264-Annex-B-Datei, muxt sie selbst zu
// MPEG-TS (PAT/PMT/PES/PCR, CRC32) und sendet sie per UDP an mediamtx
// (source: udp+mpegts://...). Beweist, dass wir OHNE FFmpeg zu mediamtx publishen
// koennen. Aufruf: poc_mpegts <datei.h264> [ip] [port] [loops]
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <thread>
#include <chrono>
#pragma comment(lib, "ws2_32.lib")

static const int PID_PMT = 0x1000, PID_VIDEO = 0x0100;
static uint8_t g_ccVideo = 0, g_ccPat = 0, g_ccPmt = 0;

static uint32_t crc32_mpeg(const uint8_t* d, int len) {
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; ++i) {
        crc ^= (uint32_t)d[i] << 24;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
    }
    return crc;
}

// Ein 188-Byte-TS-Paket bauen und in out anhaengen.
static void writeTS(std::vector<uint8_t>& out, int pid, bool pusi, uint8_t& cc,
    const uint8_t* payload, int payloadLen, bool wantPCR, uint64_t pcr) {
    uint8_t pkt[188]; memset(pkt, 0xFF, 188);
    pkt[0] = 0x47;
    pkt[1] = (pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F);
    pkt[2] = pid & 0xFF;
    int afLen = 0; bool haveAF = wantPCR;
    // Adaptation-Field nur wenn PCR gebraucht ODER Payload nicht ganz fuellt (Stuffing).
    int header = 4;
    int maxPayload = 188 - header;
    if (haveAF) { afLen = wantPCR ? 1 + 6 : 1; maxPayload = 188 - header - 1 - afLen; }
    int take = payloadLen < maxPayload ? payloadLen : maxPayload;
    int stuffing = 0;
    if (take < payloadLen) { /* volles Paket, kein Stuffing */ }
    else { // Payload passt: evtl. Adaptation-Field zum Auffuellen noetig
        int need = maxPayload - take;
        if (need > 0) { haveAF = true; if (afLen == 0) afLen = 1; stuffing = need; }
    }
    pkt[3] = (haveAF ? 0x30 : 0x10) | (cc & 0x0F); cc = (cc + 1) & 0x0F;
    int p = 4;
    if (haveAF) {
        int afTotal = afLen + stuffing;
        pkt[p++] = (uint8_t)afTotal;
        uint8_t flags = wantPCR ? 0x10 : 0x00;
        pkt[p++] = flags;
        if (wantPCR) {
            uint64_t base = pcr / 300; uint32_t ext = (uint32_t)(pcr % 300);
            pkt[p++] = (uint8_t)(base >> 25);
            pkt[p++] = (uint8_t)(base >> 17);
            pkt[p++] = (uint8_t)(base >> 9);
            pkt[p++] = (uint8_t)(base >> 1);
            pkt[p++] = (uint8_t)(((base & 1) << 7) | 0x7E | ((ext >> 8) & 1));
            pkt[p++] = (uint8_t)(ext & 0xFF);
        }
        for (int i = 0; i < stuffing; ++i) pkt[p++] = 0xFF;
    }
    memcpy(pkt + p, payload, take);
    out.insert(out.end(), pkt, pkt + 188);
    // Rest der Payload in Folge-Pakete (ohne PUSI, ohne PCR)
    int off = take;
    while (off < payloadLen) {
        uint8_t pk2[188]; memset(pk2, 0xFF, 188);
        pk2[0] = 0x47; pk2[1] = (pid >> 8) & 0x1F; pk2[2] = pid & 0xFF;
        int rem = payloadLen - off; int mp = 188 - 4; int stf = 0;
        if (rem < mp) { stf = mp - rem; pk2[3] = 0x30 | (cc & 0x0F); }
        else pk2[3] = 0x10 | (cc & 0x0F);
        cc = (cc + 1) & 0x0F;
        int q = 4;
        if (stf > 0) { pk2[q++] = (uint8_t)(stf - 1); if (stf >= 2) pk2[q++] = 0x00; for (int i = 0; i < stf - 2; ++i) pk2[q++] = 0xFF; }
        int t2 = rem < (188 - q) ? rem : (188 - q);
        memcpy(pk2 + q, payload + off, t2); off += t2;
        out.insert(out.end(), pk2, pk2 + 188);
    }
}

// PSI-Paket (PAT/PMT): pointer + section + 0xFF-Payload-Padding, KEIN Adaptation-Field.
// Das AF-Stuffing war der astits-Ablehnungsgrund ("max recorded size exceeded").
static void writePSI(std::vector<uint8_t>& out, int pid, uint8_t& cc, const uint8_t* sec, int secLen) {
    uint8_t pkt[188]; memset(pkt, 0xFF, 188);
    pkt[0] = 0x47; pkt[1] = 0x40 | ((pid >> 8) & 0x1F); pkt[2] = pid & 0xFF;
    pkt[3] = 0x10 | (cc & 0x0F); cc = (cc + 1) & 0x0F;
    pkt[4] = 0x00; memcpy(pkt + 5, sec, secLen);
    out.insert(out.end(), pkt, pkt + 188);
}

static void buildPAT(std::vector<uint8_t>& out) {
    uint8_t sec[16]; int n = 0;
    sec[n++] = 0x00; // table_id
    sec[n++] = 0xB0; sec[n++] = 0x0D; // section_syntax=1 + length=13
    sec[n++] = 0x00; sec[n++] = 0x01; // transport_stream_id
    sec[n++] = 0xC1; // version 0, current_next 1
    sec[n++] = 0x00; sec[n++] = 0x00; // section/last
    sec[n++] = 0x00; sec[n++] = 0x01; // program_number 1
    sec[n++] = 0xE0 | ((PID_PMT >> 8) & 0x1F); sec[n++] = PID_PMT & 0xFF;
    uint32_t crc = crc32_mpeg(sec, n);
    sec[n++] = crc >> 24; sec[n++] = crc >> 16; sec[n++] = crc >> 8; sec[n++] = crc;
    writePSI(out, 0x0000, g_ccPat, sec, n);
}

static void buildPMT(std::vector<uint8_t>& out) {
    uint8_t sec[32]; int n = 0;
    sec[n++] = 0x02; // table_id PMT
    sec[n++] = 0xB0; sec[n++] = 0x12; // length=18
    sec[n++] = 0x00; sec[n++] = 0x01; // program_number
    sec[n++] = 0xC1; sec[n++] = 0x00; sec[n++] = 0x00;
    sec[n++] = 0xE0 | ((PID_VIDEO >> 8) & 0x1F); sec[n++] = PID_VIDEO & 0xFF; // PCR_PID
    sec[n++] = 0xF0; sec[n++] = 0x00; // program_info_length 0
    sec[n++] = 0x1B; // stream_type H.264
    sec[n++] = 0xE0 | ((PID_VIDEO >> 8) & 0x1F); sec[n++] = PID_VIDEO & 0xFF;
    sec[n++] = 0xF0; sec[n++] = 0x00; // ES_info_length 0
    uint32_t crc = crc32_mpeg(sec, n);
    sec[n++] = crc >> 24; sec[n++] = crc >> 16; sec[n++] = crc >> 8; sec[n++] = crc;
    writePSI(out, PID_PMT, g_ccPmt, sec, n);
}

static void writePTS(std::vector<uint8_t>& v, int guard, uint64_t ts) {
    v.push_back((uint8_t)((guard << 4) | (((ts >> 30) & 0x07) << 1) | 1));
    v.push_back((uint8_t)((ts >> 22) & 0xFF));
    v.push_back((uint8_t)((((ts >> 15) & 0x7F) << 1) | 1));
    v.push_back((uint8_t)((ts >> 7) & 0xFF));
    v.push_back((uint8_t)(((ts & 0x7F) << 1) | 1));
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { printf("Aufruf: poc_mpegts <datei.h264> [ip] [port] [loops]\n"); return 1; }
    const char* ip = argc > 2 ? argv[2] : "127.0.0.1";
    int port = argc > 3 ? atoi(argv[3]) : 9998;
    int loops = argc > 4 ? atoi(argv[4]) : 6;

    FILE* f = nullptr; fopen_s(&f, argv[1], "rb");
    if (!f) { printf("FEHLER: %s nicht lesbar\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz); fread(buf.data(), 1, sz, f); fclose(f);
    printf("H.264 geladen: %ld Bytes\n", sz);

    // NALs (Annex-B) finden -> Access-Units bilden (Grenze = VCL-NAL)
    std::vector<std::pair<int, int>> nals; // offset, len (inkl. Startcode)
    for (int i = 0; i + 3 < sz; ) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            int start = i; i += 3;
            while (i + 2 < sz && !(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)) i++;
            nals.push_back({ start, (i + 2 < sz ? i : (int)sz) - start });
        } else i++;
    }
    printf("%zu NAL-Units gefunden.\n", nals.size());
    // AUs: sammle NALs, schliesse bei VCL (Typ 1-5) ab
    std::vector<std::pair<int, int>> aus; int auStart = -1; int auEnd = -1;
    for (auto& nl : nals) {
        int hdrOff = nl.first + (buf[nl.first + 2] == 1 ? 3 : 4);
        int type = buf[hdrOff] & 0x1F;
        if (auStart < 0) auStart = nl.first;
        auEnd = nl.first + nl.second;
        if (type >= 1 && type <= 5) { aus.push_back({ auStart, auEnd - auStart }); auStart = -1; }
    }
    printf("%zu Access-Units (Frames).\n", aus.size());
    if (aus.empty()) { printf("FEHLER: keine Frames\n"); return 1; }

    // Winsock + UDP
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &dst.sin_addr);
    printf("Sende MPEG-TS an %s:%d (%d Durchlaeufe)...\n", ip, port, loops);
    FILE* tsf = nullptr; fopen_s(&tsf, "out.ts", "wb");   // Debug: TS zusaetzlich in Datei -> mit ffmpeg pruefbar

    auto sendBuf = [&](std::vector<uint8_t>& ts) {
        if (tsf) fwrite(ts.data(), 1, ts.size(), tsf);
        for (size_t o = 0; o < ts.size(); o += 1316) { // 7 TS-Pakete pro UDP-Datagramm
            int len = (int)((ts.size() - o) < 1316 ? (ts.size() - o) : 1316);
            sendto(s, (const char*)ts.data() + o, len, 0, (sockaddr*)&dst, sizeof(dst));
        }
    };

    uint64_t frame = 0;
    for (int lp = 0; lp < loops; ++lp) {
        for (size_t i = 0; i < aus.size(); ++i) {
            uint64_t pts = 90000 + frame * 1500; // 90kHz, 60fps
            std::vector<uint8_t> ts;
            if (frame % 30 == 0) { buildPAT(ts); buildPMT(ts); } // PSI periodisch
            // PES bauen
            std::vector<uint8_t> pes;
            int auSize = aus[i].second;
            static const uint8_t AUD[6] = { 0,0,0,1, 0x09, 0xF0 }; // Access Unit Delimiter (wie FFmpeg) - astits erkennt daran die AU-Grenze
            pes.push_back(0); pes.push_back(0); pes.push_back(1); pes.push_back(0xE0);
            pes.push_back(0); pes.push_back(0);                     // PES_length = 0 (unbounded, wie FFmpeg)
            pes.push_back(0x80); pes.push_back(0x80); pes.push_back(5); // PTS only
            writePTS(pes, 0x2, pts);
            pes.insert(pes.end(), AUD, AUD + 6);
            pes.insert(pes.end(), buf.data() + aus[i].first, buf.data() + aus[i].first + auSize);
            uint64_t pcr = pts * 300;
            writeTS(ts, PID_VIDEO, true, g_ccVideo, pes.data(), (int)pes.size(), true, pcr);
            sendBuf(ts);
            frame++;
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 fps live
        }
    }
    if (tsf) fclose(tsf);
    printf("OK: %llu Frames als MPEG-TS gesendet.\n", (unsigned long long)frame);
    closesocket(s); WSACleanup();
    return 0;
}
