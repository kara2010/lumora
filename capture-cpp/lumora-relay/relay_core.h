// Kern des eigenen WHEP-Relays (mediamtx-Ersatz): Session-Verwaltung, WebRTC-Aufbau,
// H.264/Opus-RTP-Packetizing, Fan-out vom Ingest auf alle Zuschauer-Sessions.
// Bewusst als eigenstaendige Klasse (statische Lib denkbar) - Frontends sind
// UDP/TS-Ingest (ingest.cpp) und HTTP (whep_server/ctrl_server).
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rtc { class PeerConnection; class Track; struct RtpPacketizationConfig; }

struct RelayIceServer { std::string url, user, pass; };

struct RelayConfig {
    uint16_t icePort = 8189;                    // fester ICE-UDP-Port (Mux fuer alle Sessions)
    std::vector<std::string> additionalHosts;   // oeffentliche IPv4/IPv6 fuer Kandidaten im Answer-SDP
    std::vector<RelayIceServer> iceServers;     // TURN (an Clients signalisiert UND lokal genutzt)
};

struct RelaySessionInfo {
    std::string id, remoteCandidate, userAgent, query; // remoteCandidate: "typ/proto/ip/port" (bcExtractIp-Format)
    uint64_t bytesSent = 0;
    int64_t createdMs = 0;
};

class RelayCore {
public:
    explicit RelayCore(const RelayConfig& cfg);
    ~RelayCore();

    // WHEP: Offer-SDP rein, Answer-SDP raus (blockiert bis ICE-Gathering fertig, max ~3s).
    // Liefert leeren String bei Fehler. userAgent/query nur fuer die Roster-Anzeige.
    std::string createSession(const std::string& offerSdp, const std::string& userAgent,
                              const std::string& query, std::string& outSessionId);
    bool deleteSession(const std::string& id);           // WHEP DELETE / Kick
    std::vector<RelaySessionInfo> listSessions();

    // ICE-Konfiguration live ersetzen - wirkt nur auf NEUE Sessions (bestehende bleiben verbunden).
    void setIceConfig(std::vector<std::string> additionalHosts, std::vector<RelayIceServer> iceServers);

    // Ingest (vom TS-Demuxer): Annex-B-AU / rohes Opus-Paket mit 90kHz-PTS an alle Sessions.
    void pushVideoAU(const uint8_t* au, size_t len, uint64_t pts90k);
    void pushAudioFrame(const uint8_t* pkt, size_t len, uint64_t pts90k);

    bool ingestAlive() const;                            // Ingest in den letzten 3s gesehen?
    uint64_t framesIn() const { return framesIn_; }

private:
    struct Session;
    void dropClosed();                                   // getrennte/geschlossene Sessions ausraeumen
    RelayConfig cfg_;
    mutable std::mutex mx_;
    std::vector<std::shared_ptr<Session>> sessions_;
    uint64_t framesIn_ = 0;
    int64_t lastIngestMs_ = 0;
    // SPS/PPS-Cache: neuen Sessions fehlt sonst bis zum naechsten IDR (max 1s GOP) der Parametersatz
    std::vector<uint8_t> lastSpsPps_;
};
