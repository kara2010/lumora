// lumora-media-relay: eigener WHEP-Relay (mediamtx-Ersatz).
// UDP/MPEG-TS-Ingest (Capture-Helfer) -> WebRTC/WHEP an Zuschauer.
// Ports = heutige mediamtx-Defaults, damit die Shell unveraendert spawnen kann.
#include "relay_core.h"
#include "ts_demux.h"
#include "ingest.h"
#include "whep_server.h"
#include "ctrl_server.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // Shell liest zeilenweise vom Pipe
    uint16_t ingestPort = 8558, whepPort = 8889, ctrlPort = 9997, icePort = 8189;
    for (int i = 1; i < argc - 1; ++i) {
        if (!strcmp(argv[i], "--ingest-port")) ingestPort = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--whep-port")) whepPort = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ctrl-port")) ctrlPort = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ice-port")) icePort = (uint16_t)atoi(argv[++i]);
    }

    RelayConfig cfg; cfg.icePort = icePort;
    RelayCore core(cfg);

    TsDemux demux;
    demux.onVideoAU = [&](const uint8_t* au, size_t len, uint64_t pts) { core.pushVideoAU(au, len, pts); };
    demux.onAudioFrame = [&](const uint8_t* p, size_t len, uint64_t pts) { core.pushAudioFrame(p, len, pts); };

    Ingest ingest;
    if (!ingest.start(ingestPort, &demux)) { printf("FEHLER: Ingest-Port %u belegt\n", ingestPort); return 2; }
    WhepServer whep;
    if (!whep.start(whepPort, &core)) { printf("FEHLER: WHEP-Port %u belegt\n", whepPort); return 2; }
    // Control-API ZULETZT starten: ihr Lauschen ist das Readiness-Signal fuer die Shell
    CtrlServer ctrl;
    if (!ctrl.start(ctrlPort, &core)) { printf("FEHLER: Control-Port %u belegt\n", ctrlPort); return 2; }

    printf("relay bereit: ingest=%u whep=%u ctrl=%u ice=%u\n", ingestPort, whepPort, ctrlPort, icePort);

    // Status einmal pro Minute (landet via Shell-Pipe im Stream-Log)
    uint64_t lastAUs = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        uint64_t aus = demux.videoAUs;
        printf("relay: %llu AUs (+%llu/min), %llu Ton, %zu Zuschauer, ccErr=%llu\n",
               (unsigned long long)aus, (unsigned long long)(aus - lastAUs),
               (unsigned long long)demux.audioFrames, core.listSessions().size(),
               (unsigned long long)demux.ccErrors);
        lastAUs = aus;
    }
}
