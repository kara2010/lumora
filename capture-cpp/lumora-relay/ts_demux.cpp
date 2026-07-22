#include "ts_demux.h"
#include <cstring>

static const int PID_VIDEO = 0x0100, PID_AUDIO = 0x0101, PID_VIDEO_AV1 = 0x0102;

// PTS aus 5-Byte-Feld (33 Bit, Marker-Bits dazwischen)
static uint64_t parsePts(const uint8_t* d) {
    return ((uint64_t)((d[0] >> 1) & 0x07) << 30) | ((uint64_t)d[1] << 22) |
           ((uint64_t)((d[2] >> 1) & 0x7F) << 15) | ((uint64_t)d[3] << 7) | ((uint64_t)(d[4] >> 1) & 0x7F);
}

void TsDemux::feed(const uint8_t* data, size_t len) {
    // Datagramme kommen 188er-gerastert (Muxer sendet ganze TS-Pakete), aber wir puffern
    // trotzdem robust, falls UDP-Reassembly/Verluste das Raster verschieben.
    if (!sync_.empty()) {
        sync_.insert(sync_.end(), data, data + len);
        size_t off = 0;
        while (sync_.size() - off >= 188) {
            if (sync_[off] != 0x47) { ++off; continue; }   // Resync auf Sync-Byte
            packet(sync_.data() + off); off += 188;
        }
        sync_.erase(sync_.begin(), sync_.begin() + off);
        return;
    }
    size_t off = 0;
    while (len - off >= 188) {
        if (data[off] != 0x47) { ++off; continue; }
        packet(data + off); off += 188;
    }
    if (off < len) sync_.assign(data + off, data + len);
}

void TsDemux::packet(const uint8_t* p) {
    ++tsPackets;
    int pid = ((p[1] & 0x1F) << 8) | p[2];
    if (pid != PID_VIDEO && pid != PID_AUDIO && pid != PID_VIDEO_AV1) return;  // PAT/PMT/Null: uninteressant (Dialekt ist fest)
    bool pusi = (p[1] & 0x40) != 0;
    int afc = (p[3] >> 4) & 0x3, cc = p[3] & 0x0F;
    if (!(afc & 1)) return;                                // kein Payload
    int off = 4;
    if (afc & 2) off += 1 + p[4];                          // Adaptation Field ueberspringen
    if (off >= 188) return;

    bool video = (pid != PID_AUDIO);
    int& lastCc = (pid == PID_VIDEO) ? ccVideo_ : (pid == PID_VIDEO_AV1 ? ccVideoAv1_ : ccAudio_);
    if (lastCc >= 0 && cc != ((lastCc + 1) & 0x0F)) ++ccErrors;  // Zaehlen, nicht verwerfen (PES-Laenge/AUD begrenzen Schaden)
    lastCc = cc;

    std::vector<uint8_t>& pes = (pid == PID_VIDEO) ? pesVideo_ : (pid == PID_VIDEO_AV1 ? pesVideoAv1_ : pesAudio_);
    if (pusi) { pesComplete(pid); pes.clear(); }           // vorherige PES-Einheit abschliessen
    pes.insert(pes.end(), p + off, p + 188);

    // Audio-PES traegt PES_packet_length (Video: 0 = unbegrenzt) -> sobald vollstaendig, sofort ausliefern
    if (!video && pes.size() >= 6) {
        size_t plen = ((size_t)pes[4] << 8) | pes[5];
        if (plen > 0 && pes.size() >= 6 + plen) pesComplete(pid);
    }
}

void TsDemux::pesComplete(int pid) {
    bool video = (pid != PID_AUDIO);
    std::vector<uint8_t>& pes = (pid == PID_VIDEO) ? pesVideo_ : (pid == PID_VIDEO_AV1 ? pesVideoAv1_ : pesAudio_);
    if (pes.size() < 9) { pes.clear(); return; }
    if (pes[0] != 0 || pes[1] != 0 || pes[2] != 1) { pes.clear(); return; }
    uint8_t streamId = pes[3];
    int hdrLen = pes[8];
    size_t payloadOff = 9 + hdrLen;
    if (pes.size() <= payloadOff) { pes.clear(); return; }
    uint64_t pts = 0;
    if ((pes[7] & 0x80) && hdrLen >= 5) pts = parsePts(pes.data() + 9);

    const uint8_t* pl = pes.data() + payloadOff;
    size_t plen = pes.size() - payloadOff;
    if (video && streamId == 0xE0) {
        if (pid == PID_VIDEO_AV1) {
            // Payload = komplette AV1-Temporal-Unit (low-overhead OBU), direkt durchreichen
            ++videoAUsAv1;
            if (onVideoAuAv1) onVideoAuAv1(pl, plen, pts);
        } else {
            // Payload = AUD (00 00 00 01 09 F0) + Annex-B-AU, direkt durchreichen
            ++videoAUs;
            if (onVideoAU) onVideoAU(pl, plen, pts);
        }
    } else if (!video && streamId == 0xBD) {
        // PES_packet_length einhalten (writeTS stopft den Rest des 188er-Pakets mit 0xFF auf)
        size_t declared = ((size_t)pes[4] << 8) | pes[5];
        if (declared > 3) { size_t maxPl = declared - 3 - hdrLen; if (plen > maxPl) plen = maxPl; }
        // Opus-TS-Control-Header entfernen: 0x7F 0xE0 (11-Bit-Prefix 0x3FF, keine Trims) + 255er-Laengenkette
        if (plen >= 3 && pl[0] == 0x7F && (pl[1] & 0xE0) == 0xE0) {
            size_t i = 2, sz = 0;
            while (i < plen && pl[i] == 0xFF) { sz += 255; ++i; }
            if (i < plen) { sz += pl[i]; ++i; }
            if (i + sz <= plen) { ++audioFrames; if (onAudioFrame) onAudioFrame(pl + i, sz, pts); }
        }
    }
    pes.clear();
}
