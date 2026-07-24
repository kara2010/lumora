#include "relay_core.h"
#include "rtc/rtc.hpp"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <random>
#include <atomic>
#include <cstdio>

using std::string;
using std::vector;
using std::shared_ptr;
using std::make_shared;

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static string randomId() {
    static const char* hex = "0123456789abcdef";
    std::random_device rd; string s; s.reserve(32);
    for (int i = 0; i < 32; ++i) s += hex[rd() % 16];
    return s;
}

// H.264-PT aus dem Offer waehlen: bevorzugt packetization-mode=1 (FU-A), sonst erster H264-Eintrag
static int pickH264Pt(const rtc::Description::Media* m) {
    int fallback = -1;
    for (int pt : m->payloadTypes()) {
        const auto* map = m->rtpMap(pt);
        if (!map || map->format != "H264") continue;
        if (fallback < 0) fallback = pt;
        for (const auto& f : map->fmtps)
            if (f.find("packetization-mode=1") != string::npos) return pt;
    }
    return fallback;
}
// AV1-PT aus dem Offer waehlen (Chrome/Firefox/Edge bieten AV1 an, Safari nicht)
static int pickAv1Pt(const rtc::Description::Media* m) {
    for (int pt : m->payloadTypes()) {
        const auto* map = m->rtpMap(pt);
        if (map && map->format == "AV1") return pt;
    }
    return -1;
}
static int pickOpusPt(const rtc::Description::Media* m) {
    for (int pt : m->payloadTypes()) {
        const auto* map = m->rtpMap(pt);
        if (map && map->format == "opus") return pt;
    }
    return -1;
}

struct RelayCore::Session {
    string id, userAgent, query;
    int64_t createdMs = 0;
    shared_ptr<rtc::PeerConnection> pc;
    shared_ptr<rtc::Track> video, audio;
    shared_ptr<rtc::RtpPacketizationConfig> vCfg, aCfg;
    bool vFirst = true;             // erst ab erstem Keyframe senden (H.264: SPS/PPS+IDR; AV1: SeqHdr+Key)
    bool vAv1 = false;              // dieser Zuschauer bekommt den AV1-Zweig (SDP-Offer konnte AV1)
    // RTP-Timestamps laufen relativ weiter: bei PTS-Sprung (Capture-Neustart = neue Zeitbasis)
    // wird mit einem Frame-Schritt fortgesetzt statt den Sprung durchzureichen.
    uint64_t vLastPts = 0, aLastPts = 0; bool vTsSet = false, aTsSet = false;
    uint32_t vTs = 0, aTs = 0;
    uint64_t bytesSent = 0;         // selbst gezaehlt (pc->bytesSent() zaehlt nur Data-Channels)
    // Diagnose je Zuschauer. Bisher war von aussen NICHT erkennbar, ob ein Zuschauer
    // ueberhaupt jemals sein erstes Keyframe bekommt (vFirst-Tor) oder ob Senden fehlschlaegt:
    // der Rueckgabewert von send() wurde nur fuer bytesSent benutzt, Exceptions verschluckte
    // ein leeres catch. Genau das kostete uns die Fehlersuche "Verbindung steht, kein Bild".
    uint64_t sendOk = 0, sendFail = 0, sendEx = 0;
    bool gateLogged = false;        // "erstes Keyframe durchgelassen" schon gemeldet?
    int64_t openedMs = 0;           // wann das Tor aufging (0 = nie)
};

// Verworfene Pakete SICHTBAR machen. libjuice meldet einen vollen Sendepuffer nur mit
// JLOG_INFO("Send failed, buffer is full") - auf Log-Level Warning war das unsichtbar,
// obwohl genau dort Pakete im EIGENEN Prozess verloren gehen (nicht blockierender
// Socket: was nicht in den Puffer passt, ist weg). Wir hoeren jetzt auf Info, geben
// aber nicht die volle Flut aus, sondern zaehlen nur diesen Fall und melden ihn 1x/s.
std::atomic<uint64_t> g_sendBufFull{ 0 };
static void relayLogFilter(rtc::LogLevel lev, std::string msg) {
    if (msg.find("buffer is full") != std::string::npos) {
        uint64_t n = ++g_sendBufFull;
        static std::atomic<uint64_t> lastReport{ 0 };
        uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t prev = lastReport.load();
        if (now != prev && lastReport.compare_exchange_strong(prev, now))
            printf("relay: SENDEPUFFER VOLL - %llu Pakete im eigenen Prozess verworfen\n",
                   (unsigned long long)n);
        return;
    }
    if (lev <= rtc::LogLevel::Warning) printf("relay: %s\n", msg.c_str());
}
RelayCore::RelayCore(const RelayConfig& cfg) : cfg_(cfg) {
    rtc::InitLogger(rtc::LogLevel::Info, relayLogFilter);
    sender_ = std::thread([this] { sendLoop(); });   // Sende-Thread (s. pushVideoAU im Header)
}
RelayCore::~RelayCore() {
    qRun_ = false; qcv_.notify_all();
    if (sender_.joinable()) sender_.join();
    std::lock_guard<std::mutex> lk(mx_);
    sessions_.clear();
}

// Ingest-seitige Enqueue-Stubs + Sende-Thread (Entkopplung, s. Header). Queue-Limit 240
// Eintraege (~2s Video + Ton bei 60fps): laeuft der Sende-Thread dauerhaft hinterher
// (langsamer Zuschauer), fliegt das AELTESTE raus - der Ingest blockiert NIE.
void RelayCore::pushVideoAU(const uint8_t* au, size_t len, uint64_t pts90k) {
    std::lock_guard<std::mutex> lk(qmx_);
    if (q_.size() >= 240) { q_.pop_front(); ++qDropped_; }
    q_.push_back({ 0, std::vector<uint8_t>(au, au + len), pts90k });
    qcv_.notify_one();
}
void RelayCore::pushVideoAuAv1(const uint8_t* tu, size_t len, uint64_t pts90k) {
    std::lock_guard<std::mutex> lk(qmx_);
    if (q_.size() >= 240) { q_.pop_front(); ++qDropped_; }
    q_.push_back({ 1, std::vector<uint8_t>(tu, tu + len), pts90k });
    qcv_.notify_one();
}
void RelayCore::pushAudioFrame(const uint8_t* pkt, size_t len, uint64_t pts90k) {
    std::lock_guard<std::mutex> lk(qmx_);
    if (q_.size() >= 240) { q_.pop_front(); ++qDropped_; }
    q_.push_back({ 2, std::vector<uint8_t>(pkt, pkt + len), pts90k });
    qcv_.notify_one();
}
void RelayCore::sendLoop() {
    for (;;) {
        QItem it;
        {
            std::unique_lock<std::mutex> lk(qmx_);
            qcv_.wait(lk, [this] { return !q_.empty() || !qRun_; });
            if (!qRun_ && q_.empty()) return;
            it = std::move(q_.front()); q_.pop_front();
        }
        if (it.kind == 0) sendVideoAU(it.data.data(), it.data.size(), it.pts);
        else if (it.kind == 1) sendVideoAuAv1(it.data.data(), it.data.size(), it.pts);
        else sendAudioFrame(it.data.data(), it.data.size(), it.pts);
    }
}

void RelayCore::setIceConfig(vector<string> hosts, vector<RelayIceServer> servers) {
    std::lock_guard<std::mutex> lk(mx_);
    cfg_.additionalHosts = std::move(hosts);
    cfg_.iceServers = std::move(servers);
}

void RelayCore::setCodecMode(const string& mode) {
    std::lock_guard<std::mutex> lk(mx_);
    codecMode_ = (mode == "h264" || mode == "av1") ? mode : "auto";
    printf("relay: codec-mode -> %s\n", codecMode_.c_str());
}

void RelayCore::codecDemand(bool& needH264, bool& needAv1) {
    needH264 = false; needAv1 = false;
    std::lock_guard<std::mutex> lk(mx_);
    dropClosed();
    // Ueber ALLE offenen Sessions inkl. lokaler Host-Vorschau: die Vorschau ist AV1-faehig
    // und traegt unter auto/av1 zum AV1-Bedarf bei - sie allein loest also KEINEN Doppel-Encode
    // aus (nur einen AV1-Encoder). Erst ein echter H.264-Zuschauer setzt needH264.
    for (auto& s : sessions_) {
        if (!s->video) continue;
        if (s->vAv1) needAv1 = true; else needH264 = true;
    }
}

string RelayCore::createSession(const string& offerSdp, const string& userAgent,
                                const string& query, string& outSessionId) {
    RelayConfig cfg; string codecMode;
    { std::lock_guard<std::mutex> lk(mx_); cfg = cfg_; codecMode = codecMode_; }
    bool allowAv1 = (codecMode != "h264");

    rtc::Configuration rc;
    rc.portRangeBegin = rc.portRangeEnd = cfg.icePort;
    rc.enableIceUdpMux = true;                 // alle Sessions teilen sich den einen UDP-Port
    for (const auto& s : cfg.iceServers) {
        rtc::IceServer is(s.url);
        if (!s.user.empty()) { is.username = s.user; is.password = s.pass; }
        rc.iceServers.push_back(std::move(is));
    }

    auto sess = make_shared<Session>();
    sess->id = randomId(); sess->userAgent = userAgent; sess->query = query;
    sess->createdMs = nowMs();
    sess->pc = make_shared<rtc::PeerConnection>(rc);

    // Tracks entstehen aus den m-Lines des Offers (Client bietet recvonly an -> wir senden)
    std::weak_ptr<Session> wsess = sess;
    sess->pc->onTrack([this, wsess, allowAv1](shared_ptr<rtc::Track> track) {
        auto s = wsess.lock(); if (!s) return;
        auto desc = track->description();
        const string type = desc.type();
        // MID-Header-Extension setzen: Chrome ordnet BUNDLE-Pakete ohne signalisierte SSRC
        // NUR ueber die MID-Extension der richtigen m-Line zu (sonst: stiller Discard).
        auto findMidId = [](rtc::Description::Media* m) -> uint8_t {
            for (int id : m->extIds()) {
                const auto* em = m->extMap(id);
                if (em && em->uri == "urn:ietf:params:rtp-hdrext:sdes:mid") return (uint8_t)id;
            }
            return 0;
        };
        if (type == "video") {
            // Codec pro Zuschauer: AV1, wenn der Modus es erlaubt (auto/av1) UND der Browser es
            // anbietet - NICHT an av1Active_ gekoppelt (das waere ein Bootstrapping-Deadlock:
            // kein AV1-Ingest -> H.264 gewaehlt -> kein AV1-Bedarf -> AV1 startet nie). Die
            // AV1-Frames kommen, sobald der Capture AV1 hochgefahren hat (~1-2s beim Erst-AV1-
            // Zuschauer). Sonst H.264-Fallback (Safari, aeltere Geraete).
            int av1Pt = allowAv1 ? pickAv1Pt(&desc) : -1;
            if (av1Pt >= 0) {
                s->vAv1 = true;
                s->vCfg = make_shared<rtc::RtpPacketizationConfig>(
                    (rtc::SSRC)0x4C4D5257, "lumora-video", (uint8_t)av1Pt, rtc::AV1RtpPacketizer::ClockRate);
                s->vCfg->midId = findMidId(&desc); s->vCfg->mid = desc.mid();
                // Sende-SSRC signalisieren: nur dann ordnet libdatachannel eingehendes
                // RTCP (NACK/PLI/RR) unserem Track zu - sonst wird es verworfen und es
                // findet KEINE Paketwiederholung statt (jeder Verlust bleibt sichtbar).
                desc.addSSRC((uint32_t)0x4C4D5257, "lumora-video");
                track->setDescription(desc);
                auto pk = make_shared<rtc::AV1RtpPacketizer>(rtc::AV1RtpPacketizer::Packetization::TemporalUnit, s->vCfg);
                pk->addToChain(make_shared<rtc::RtcpSrReporter>(s->vCfg));
                pk->addToChain(make_shared<rtc::RtcpNackResponder>());
                track->setMediaHandler(pk);
                track->onMessage([](rtc::binary) {}, nullptr);   // RTCP-Rueckkanal konsumieren
                s->video = track;
                return;
            }
            int pt = pickH264Pt(&desc); if (pt < 0) return;
            s->vCfg = make_shared<rtc::RtpPacketizationConfig>(
                (rtc::SSRC)0x4C4D5256, "lumora-video", (uint8_t)pt, rtc::H264RtpPacketizer::ClockRate);
            s->vCfg->midId = findMidId(&desc); s->vCfg->mid = desc.mid();
            desc.addSSRC((uint32_t)0x4C4D5256, "lumora-video");   // s.o. - ohne SSRC kein NACK
            track->setDescription(desc);
            auto pk = make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, s->vCfg);
            pk->addToChain(make_shared<rtc::RtcpSrReporter>(s->vCfg));
            pk->addToChain(make_shared<rtc::RtcpNackResponder>());
            track->setMediaHandler(pk);
            track->onMessage([](rtc::binary) {}, nullptr);   // RTCP-Rueckkanal konsumieren
            s->video = track;
        } else if (type == "audio") {
            int pt = pickOpusPt(&desc); if (pt < 0) return;
            s->aCfg = make_shared<rtc::RtpPacketizationConfig>(
                (rtc::SSRC)0x4C4D5241, "lumora-audio", (uint8_t)pt, rtc::OpusRtpPacketizer::DefaultClockRate);
            s->aCfg->midId = findMidId(&desc); s->aCfg->mid = desc.mid();
            auto pk = make_shared<rtc::OpusRtpPacketizer>(s->aCfg);
            pk->addToChain(make_shared<rtc::RtcpSrReporter>(s->aCfg));
            pk->addToChain(make_shared<rtc::RtcpNackResponder>());
            track->setMediaHandler(pk);
            track->onMessage([](rtc::binary) {}, nullptr);
            s->audio = track;
        }
    });

    // Gathering synchron abwarten (lokale Host-Kandidaten auf festem Port: schnell; TURN: bis ~3s)
    std::mutex gm; std::condition_variable gcv; bool done = false;
    sess->pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState st) {
        if (st == rtc::PeerConnection::GatheringState::Complete) {
            std::lock_guard<std::mutex> lk(gm); done = true; gcv.notify_all();
        }
    });

    try {
        // setRemoteDescription(Offer) erzeugt die Answer AUTOMATISCH (Auto-Negotiation).
        // KEIN zusaetzliches setLocalDescription() - das wuerde die fertige Answer durch
        // ein neues Offer ersetzen (a=setup:actpass -> Browser lehnt ab).
        sess->pc->setRemoteDescription(rtc::Description(offerSdp, "offer"));
    } catch (const std::exception&) { return ""; }


    {
        std::unique_lock<std::mutex> lk(gm);
        gcv.wait_for(lk, std::chrono::milliseconds(3000), [&] { return done; });
    }
    // Callback abmelden: er captured gm/gcv/done per Referenz (lokale Variablen dieser
    // Funktion) und darf nach dem Return nicht mehr feuern.
    sess->pc->onGatheringStateChange(nullptr);
    auto local = sess->pc->localDescription();
    if (!local) return "";
    string sdp = string(*local);

    // Oeffentliche Hosts (Router-IP/IPv6) als zusaetzliche Host-Kandidaten eintragen -
    // Ersatz fuer mediamtx' webrtcAdditionalHosts (libdatachannel kennt das Konzept nicht).
    if (!cfg.additionalHosts.empty()) {
        string extra;
        uint32_t prio = 1686052607u;   // knapp unter typischer srflx-Prioritaet
        int foundation = 900;
        for (const auto& h : cfg.additionalHosts) {
            extra += "a=candidate:" + std::to_string(foundation++) + " 1 UDP " +
                     std::to_string(prio--) + " " + h + " " + std::to_string(cfg.icePort) +
                     " typ host\r\n";
        }
        // vor "a=end-of-candidates" bzw. ans Ende der ersten Media-Section
        size_t pos = sdp.find("a=end-of-candidates");
        if (pos != string::npos) sdp.insert(pos, extra);
        else sdp += extra;
    }

    {
        std::lock_guard<std::mutex> lk(mx_);
        dropClosed();
        sessions_.push_back(sess);
    }
    // Codec-Zuteilung sichtbar machen (Beweis fuer die Reihenfolge Push vs. Connect im Log)
    printf("relay: %s Session neu -> %s (Modus %s)\n", sess->id.substr(0, 6).c_str(),
           sess->vAv1 ? "AV1" : (sess->video ? "H264" : "ohne Video"), codecMode.c_str());
    outSessionId = sess->id;
    return sdp;
}

bool RelayCore::deleteSession(const string& id) {
    std::lock_guard<std::mutex> lk(mx_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if ((*it)->id == id) {
            try { (*it)->pc->close(); } catch (...) {}
            sessions_.erase(it);
            return true;
        }
    }
    return false;
}

vector<RelaySessionInfo> RelayCore::listSessions() {
    std::lock_guard<std::mutex> lk(mx_);
    dropClosed();
    vector<RelaySessionInfo> out;
    for (auto& s : sessions_) {
        RelaySessionInfo i;
        i.id = s->id; i.userAgent = s->userAgent; i.query = s->query; i.createdMs = s->createdMs;
        i.bytesSent = s->bytesSent;
        rtc::Candidate loc, rem;
        if (s->pc->getSelectedCandidatePair(&loc, &rem)) {
            // Format wie mediamtx: "typ/proto/ip/port" (Shell parst IP als vorletztes Segment)
            const char* typ = "host";
            switch (rem.type()) {
                case rtc::Candidate::Type::ServerReflexive: typ = "srflx"; break;
                case rtc::Candidate::Type::PeerReflexive:   typ = "prflx"; break;
                case rtc::Candidate::Type::Relayed:         typ = "relay"; break;
                default: break;
            }
            string ip = rem.address().value_or("");
            int port = rem.port().value_or(0);
            i.remoteCandidate = string(typ) + "/udp/" + ip + "/" + std::to_string(port);
        }
        out.push_back(std::move(i));
    }
    return out;
}

void RelayCore::dropClosed() {   // mx_ muss gehalten sein
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        auto st = (*it)->pc->state();
        if (st == rtc::PeerConnection::State::Closed || st == rtc::PeerConnection::State::Failed)
            it = sessions_.erase(it);
        else ++it;
    }
}

// Annex-B nach NAL-Typen scannen (SPS=7, PPS=8, IDR=5)
static void scanAnnexB(const uint8_t* d, size_t n, bool& hasIdr, size_t& spsPpsEnd) {
    hasIdr = false; spsPpsEnd = 0;
    size_t i = 0;
    while (i + 4 < n) {
        size_t sc = 0;
        if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1) sc = 3;
        else if (d[i] == 0 && d[i+1] == 0 && d[i+2] == 0 && d[i+3] == 1) sc = 4;
        if (!sc) { ++i; continue; }
        size_t nalStart = i + sc;
        if (nalStart >= n) break;
        int type = d[nalStart] & 0x1F;
        if (type == 5) hasIdr = true;
        // Ende dieses NALs suchen (naechster Startcode)
        size_t j = nalStart;
        while (j + 3 < n && !(d[j] == 0 && d[j+1] == 0 && (d[j+2] == 1 || (d[j+2] == 0 && j + 4 < n && d[j+3] == 1)))) ++j;
        size_t nalEnd = (j + 3 < n) ? j : n;
        if (type == 7 || type == 8) spsPpsEnd = nalEnd;
        i = nalEnd;
    }
}

void RelayCore::sendVideoAU(const uint8_t* au, size_t len, uint64_t pts90k) {
    bool hasIdr; size_t spsPpsEnd;
    scanAnnexB(au, len, hasIdr, spsPpsEnd);

    std::lock_guard<std::mutex> lk(mx_);
    ++framesIn_; bytesIn_ += len; lastIngestMs_ = nowMs();
    if (spsPpsEnd > 0) lastSpsPps_.assign(au, au + spsPpsEnd);   // fuer spaeter (derzeit: Encoder sendet SPS/PPS vor jedem IDR)

    for (auto& s : sessions_) {
        if (!s->video || !s->video->isOpen() || !s->vCfg || s->vAv1) continue;
        if (s->vFirst) { if (!hasIdr) continue; s->vFirst = false; s->openedMs = nowMs(); }   // sauberer Einstieg am IDR
        if (!s->gateLogged) { s->gateLogged = true; printf("relay: %s H264 erstes IDR durchgelassen (%zu Bytes)\n", s->id.substr(0, 6).c_str(), len); }
        if (!s->vTsSet) { s->vTs = s->vCfg->startTimestamp; s->vTsSet = true; }
        else {
            int64_t d = (int64_t)pts90k - (int64_t)s->vLastPts;
            if (d < 0 || d > 90000 * 10) d = 3000;   // Diskontinuitaet (Capture-Neustart): 1 Frame-Schritt
            s->vTs += (uint32_t)d;
        }
        s->vLastPts = pts90k;
        s->vCfg->timestamp = s->vTs;
        try {
            if (s->video->send(reinterpret_cast<const std::byte*>(au), len)) { s->bytesSent += len; ++s->sendOk; }
            else ++s->sendFail;
        } catch (...) { ++s->sendEx; }
    }
}

// AV1-Temporal-Unit (low-overhead OBU-Format) nach OBU_SEQUENCE_HEADER (Typ 1) scannen:
// der Encoder wiederholt den Sequence Header bei jedem Keyframe (repeatSeqHdr bzw.
// KEY_FRAME_ALIGNED) -> sein Vorhandensein ist unser Keyframe-Signal fuer den Einstieg.
static bool av1HasSeqHdr(const uint8_t* d, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t hdr = d[i];
        if (hdr & 0x80) return false;                    // forbidden bit -> kein gueltiges OBU
        int type = (hdr >> 3) & 0x0F;
        bool ext = (hdr & 0x04) != 0, hasSize = (hdr & 0x02) != 0;
        if (type == 1) return true;                      // OBU_SEQUENCE_HEADER
        size_t j = i + 1 + (ext ? 1 : 0);
        if (!hasSize) return false;                      // low-overhead ohne Size: Rest = letztes OBU
        uint64_t sz = 0; int shift = 0;                  // leb128
        while (j < n) { uint8_t b = d[j++]; sz |= (uint64_t)(b & 0x7F) << shift; if (!(b & 0x80)) break; shift += 7; if (shift > 56) return false; }
        i = j + sz;
    }
    return false;
}

void RelayCore::sendVideoAuAv1(const uint8_t* tu, size_t len, uint64_t pts90k) {
    bool key = av1HasSeqHdr(tu, len);
    std::lock_guard<std::mutex> lk(mx_);
    av1Active_.store(true);
    bytesIn_ += len; lastIngestMs_ = nowMs();
    for (auto& s : sessions_) {
        if (!s->vAv1 || !s->video || !s->video->isOpen() || !s->vCfg) continue;
        if (s->vFirst) { if (!key) continue; s->vFirst = false; s->openedMs = nowMs(); }   // sauberer Einstieg am Keyframe
        if (!s->gateLogged) { s->gateLogged = true; printf("relay: %s AV1 erstes Keyframe durchgelassen (%zu Bytes)\n", s->id.substr(0, 6).c_str(), len); }
        if (!s->vTsSet) { s->vTs = s->vCfg->startTimestamp; s->vTsSet = true; }
        else {
            int64_t d = (int64_t)pts90k - (int64_t)s->vLastPts;
            if (d < 0 || d > 90000 * 10) d = 3000;   // Diskontinuitaet (Capture-Neustart): 1 Frame-Schritt
            s->vTs += (uint32_t)d;
        }
        s->vLastPts = pts90k;
        s->vCfg->timestamp = s->vTs;
        try {
            if (s->video->send(reinterpret_cast<const std::byte*>(tu), len)) { s->bytesSent += len; ++s->sendOk; }
            else ++s->sendFail;
        } catch (...) { ++s->sendEx; }
    }
}

// Je Zuschauer eine Zeile. "wartet-auf-Keyframe" ist der entscheidende Zustand: steht ein
// Zuschauer dauerhaft darauf, bekommt er nie ein Bild, obwohl die Verbindung offen ist.
void RelayCore::logSessionDiag() {
    std::lock_guard<std::mutex> lk(mx_);
    for (auto& s : sessions_) {
        const char* st = !s->video ? "kein-Video" : (s->vFirst ? "WARTET-AUF-KEYFRAME" : "laeuft");
        printf("relay: %s %s %s gesendet=%lluKB ok=%llu fehlgeschlagen=%llu ausnahmen=%llu %s\n",
               s->id.substr(0, 6).c_str(), s->vAv1 ? "AV1" : "H264", st,
               (unsigned long long)(s->bytesSent / 1024), (unsigned long long)s->sendOk,
               (unsigned long long)s->sendFail, (unsigned long long)s->sendEx,
               s->pc && s->pc->remoteAddress() ? s->pc->remoteAddress()->c_str() : "?");
    }
}

void RelayCore::sendAudioFrame(const uint8_t* pkt, size_t len, uint64_t pts90k) {
    std::lock_guard<std::mutex> lk(mx_);
    bytesIn_ += len; lastIngestMs_ = nowMs();
    for (auto& s : sessions_) {
        if (!s->audio || !s->audio->isOpen() || !s->aCfg) continue;
        if (s->vFirst && s->video) continue;                 // Ton erst, wenn Video laeuft (A/V-Start sync)
        if (!s->aTsSet) { s->aTs = s->aCfg->startTimestamp; s->aTsSet = true; }
        else {
            int64_t d = (int64_t)pts90k - (int64_t)s->aLastPts;
            if (d < 0 || d > 90000 * 10) d = 1800;           // Diskontinuitaet: 1 Opus-Frame (20ms @90k)
            // 90kHz -> 48kHz: *8/15 (ganzzahlig exakt, da Opus-Frames auf 20ms-Raster liegen)
            s->aTs += (uint32_t)((d * 8) / 15);
        }
        s->aLastPts = pts90k;
        s->aCfg->timestamp = s->aTs;
        try {
            if (s->audio->send(reinterpret_cast<const std::byte*>(pkt), len)) s->bytesSent += len;
        } catch (...) {}
    }
}

bool RelayCore::ingestAlive() const {
    std::lock_guard<std::mutex> lk(mx_);
    return lastIngestMs_ != 0 && (nowMs() - lastIngestMs_) < 3000;
}

uint64_t RelayCore::bytesIn() const {
    std::lock_guard<std::mutex> lk(mx_);
    return bytesIn_;
}
