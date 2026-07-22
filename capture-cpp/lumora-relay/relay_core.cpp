#include "relay_core.h"
#include "rtc/rtc.hpp"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <random>

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
    bool vFirst = true;             // erst ab erstem IDR senden (Decoder braucht SPS/PPS+IDR)
    // RTP-Timestamps laufen relativ weiter: bei PTS-Sprung (Capture-Neustart = neue Zeitbasis)
    // wird mit einem Frame-Schritt fortgesetzt statt den Sprung durchzureichen.
    uint64_t vLastPts = 0, aLastPts = 0; bool vTsSet = false, aTsSet = false;
    uint32_t vTs = 0, aTs = 0;
    uint64_t bytesSent = 0;         // selbst gezaehlt (pc->bytesSent() zaehlt nur Data-Channels)
};

RelayCore::RelayCore(const RelayConfig& cfg) : cfg_(cfg) {
    rtc::InitLogger(rtc::LogLevel::Warning);
}
RelayCore::~RelayCore() {
    std::lock_guard<std::mutex> lk(mx_);
    sessions_.clear();
}

void RelayCore::setIceConfig(vector<string> hosts, vector<RelayIceServer> servers) {
    std::lock_guard<std::mutex> lk(mx_);
    cfg_.additionalHosts = std::move(hosts);
    cfg_.iceServers = std::move(servers);
}

string RelayCore::createSession(const string& offerSdp, const string& userAgent,
                                const string& query, string& outSessionId) {
    RelayConfig cfg;
    { std::lock_guard<std::mutex> lk(mx_); cfg = cfg_; }

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
    sess->pc->onTrack([this, wsess](shared_ptr<rtc::Track> track) {
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
            int pt = pickH264Pt(&desc); if (pt < 0) return;
            s->vCfg = make_shared<rtc::RtpPacketizationConfig>(
                (rtc::SSRC)0x4C4D5256, "lumora-video", (uint8_t)pt, rtc::H264RtpPacketizer::ClockRate);
            s->vCfg->midId = findMidId(&desc); s->vCfg->mid = desc.mid();
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

void RelayCore::pushVideoAU(const uint8_t* au, size_t len, uint64_t pts90k) {
    bool hasIdr; size_t spsPpsEnd;
    scanAnnexB(au, len, hasIdr, spsPpsEnd);

    std::lock_guard<std::mutex> lk(mx_);
    ++framesIn_; lastIngestMs_ = nowMs();
    if (spsPpsEnd > 0) lastSpsPps_.assign(au, au + spsPpsEnd);   // fuer spaeter (derzeit: Encoder sendet SPS/PPS vor jedem IDR)

    for (auto& s : sessions_) {
        if (!s->video || !s->video->isOpen() || !s->vCfg) continue;
        if (s->vFirst) { if (!hasIdr) continue; s->vFirst = false; }   // sauberer Einstieg am IDR
        if (!s->vTsSet) { s->vTs = s->vCfg->startTimestamp; s->vTsSet = true; }
        else {
            int64_t d = (int64_t)pts90k - (int64_t)s->vLastPts;
            if (d < 0 || d > 90000 * 10) d = 3000;   // Diskontinuitaet (Capture-Neustart): 1 Frame-Schritt
            s->vTs += (uint32_t)d;
        }
        s->vLastPts = pts90k;
        s->vCfg->timestamp = s->vTs;
        try {
            if (s->video->send(reinterpret_cast<const std::byte*>(au), len)) s->bytesSent += len;
        } catch (...) {}
    }
}

void RelayCore::pushAudioFrame(const uint8_t* pkt, size_t len, uint64_t pts90k) {
    std::lock_guard<std::mutex> lk(mx_);
    lastIngestMs_ = nowMs();
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
