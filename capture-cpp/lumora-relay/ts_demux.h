// Demuxer fuer GENAU den MPEG-TS-Dialekt unseres Capture-Helfers
// (lumora-capture/main.cpp, "MPEG-TS-Muxer (bewiesen gegen mediamtx)"):
//   PAT PID 0, PMT PID 0x1000, Video PID 0x100 (H.264, PES 0xE0, PTS, AUD vor jeder AU),
//   Audio PID 0x101 (Opus, PES 0xBD, Control-Header 0x7F 0xE0 + 255er-Laengenkette).
// Kein generischer TS-Parser: PIDs sind fest, PSI wird nur zur Plausibilitaet ueberflogen.
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

struct TsDemux {
    // Callbacks: Annex-B-Access-Unit (inkl. AUD) mit 90kHz-PTS / rohes Opus-Paket mit 90kHz-PTS
    std::function<void(const uint8_t* au, size_t len, uint64_t pts90k)> onVideoAU;
    std::function<void(const uint8_t* pkt, size_t len, uint64_t pts90k)> onAudioFrame;

    // Beliebig grosse Happen (UDP-Datagramme, 1316 = 7x188) einspeisen; Restbytes werden gepuffert.
    void feed(const uint8_t* data, size_t len);

    uint64_t videoAUs = 0, audioFrames = 0, tsPackets = 0, ccErrors = 0;

private:
    void packet(const uint8_t* p); // genau 188 Bytes
    void pesComplete(int pid);
    std::vector<uint8_t> sync_;          // Restbytes bis zum naechsten 188er-Raster
    std::vector<uint8_t> pesVideo_, pesAudio_;
    int ccVideo_ = -1, ccAudio_ = -1;
};
