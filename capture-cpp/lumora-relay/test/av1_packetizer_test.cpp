// Bau-Sperre fuer den AV1-RTP-Packetizer (libdatachannel + Lumora-Patch, deps/patches).
//
// Hintergrund (Schwarzbild mit neuen NVIDIA-Treibern, 2026-10): libwebrtc (Chrome, Edge,
// WebView2, Firefox) erkennt Bildanfang und -ende AUSSCHLIESSLICH an den Z-/Y-Bits des
// AV1-Aggregationskopfes. Ein Packetizer, der jedes OBU einer Temporal Unit als eigene
// Paketkette schickt, funktioniert nur zufaellig - naemlich solange der Encoder ein
// einziges OBU_FRAME je Bild liefert. Liefert er FRAME_HEADER + TILE_GROUP getrennt,
// sieht der Browser zwei unbrauchbare "Bilder" und zeigt Schwarz.
//
// Dieser Test schickt synthetische Temporal Units BEIDER Formen durch den echten
// Packetizer (ueber die oeffentliche outgoing()-Schnittstelle, wie im Relay) und prueft:
//   1. genau EINE Paketkette je TU: Z=1 auf jedem Paket ausser dem ersten, Y=1 auf jedem
//      ausser dem letzten, RTP-Marker nur auf dem letzten
//   2. N-Bit (neue Videosequenz) genau dann auf Paket 1, wenn die TU mit dem Sequence
//      Header beginnt; nie zusammen mit Z
//   3. das Zusammensetzen nach av1-rtp-spec liefert exakt die eingegebenen OBUs
//      (ohne obu_size-Feld, has_size-Bit geloescht; Temporal Delimiter/Padding entfernt)
//   4. Grenzfall: ein OBU endet exakt an der Paketgrenze - die Kette darf nicht reissen
// Exit-Code 0 = alles gruen, sonst 1 mit Klartext-Befund.
#include "rtc/rtc.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using rtc::byte;
static int g_fehler = 0;
#define PRUEFE(bed, ...) do { if (!(bed)) { ++g_fehler; printf("  FEHLER: " __VA_ARGS__); printf("\n"); } } while (0)

// ---- Testdaten: OBUs im low-overhead-Format (mit obu_size), wie sie der Encoder liefert ----
static void leb128(rtc::binary& o, size_t v) {
    do { uint8_t b = uint8_t(v & 0x7F); v >>= 7; if (v) b |= 0x80; o.push_back(byte(b)); } while (v);
}
static rtc::binary obu(int type, size_t payloadLen, uint8_t seed) {
    rtc::binary o; o.push_back(byte((type << 3) | 0x02)); leb128(o, payloadLen);
    for (size_t i = 0; i < payloadLen; ++i) o.push_back(byte(uint8_t(seed + i * 7)));
    return o;
}
// Erwartetes Element nach RTP-Transport: Header ohne has_size + Nutzlast (kein Groessenfeld)
static rtc::binary erwartet(const rtc::binary& in) {
    rtc::binary o; o.push_back(in[0] & ~byte(0x02));
    size_t p = 1; while ((uint8_t)in[p] & 0x80) ++p; ++p;   // leb128 ueberspringen
    o.insert(o.end(), in.begin() + p, in.end());
    return o;
}
static rtc::binary tu(const std::vector<rtc::binary>& obus) {
    rtc::binary t = { byte(0x12), byte(0x00) };   // Temporal Delimiter
    for (auto& o : obus) t.insert(t.end(), o.begin(), o.end());
    return t;
}
enum { TD = 2, SEQ = 1, FH = 3, TG = 4, FRAME = 6, PAD = 15 };

// ---- Packetizer treiben + RTP-Pakete zerlegen ----
struct Pkt { bool z, y, n, marker; int w; rtc::binary payload; };
static std::vector<Pkt> packetisiere(const rtc::binary& tuData, size_t maxFrag) {
    auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(0x1234u, "test", 45, rtc::AV1RtpPacketizer::ClockRate);
    rtc::AV1RtpPacketizer p(rtc::AV1RtpPacketizer::Packetization::TemporalUnit, cfg, maxFrag);
    rtc::message_vector msgs; rtc::binary kopie = tuData;
    msgs.push_back(rtc::make_message(std::move(kopie)));
    p.outgoing(msgs, nullptr);
    std::vector<Pkt> out;
    for (auto& m : msgs) {
        PRUEFE(m->size() >= 13, "RTP-Paket zu kurz (%zu Bytes)", m->size());
        if (m->size() < 13) continue;
        uint8_t b1 = (uint8_t)(*m)[1], agg = (uint8_t)(*m)[12];
        Pkt k; k.marker = (b1 & 0x80) != 0; k.z = agg & 0x80; k.y = agg & 0x40; k.w = (agg >> 4) & 3; k.n = agg & 0x08;
        k.payload.assign(m->begin() + 13, m->end());
        out.push_back(std::move(k));
    }
    return out;
}
// Zusammensetzen nach av1-rtp-spec §4.4 (wie libwebrtc): Elemente je Paket, W/Laengenfelder,
// Z/Y-Fragmente ueber Paketgrenzen zusammenfuegen.
static std::vector<rtc::binary> setzeZusammen(const std::vector<Pkt>& pk) {
    std::vector<rtc::binary> obus; rtc::binary offen; bool offenAktiv = false;
    for (size_t i = 0; i < pk.size(); ++i) {
        const auto& p = pk[i]; size_t pos = 0; std::vector<rtc::binary> elems;
        size_t anzahl = p.w; size_t idx = 0;
        while (pos < p.payload.size()) {
            size_t len;
            bool letztesOhneLaenge = (p.w != 0 && idx + 1 == anzahl);
            if (letztesOhneLaenge) len = p.payload.size() - pos;
            else { len = 0; int sh = 0; for (;;) { uint8_t b = (uint8_t)p.payload[pos++]; len |= size_t(b & 0x7F) << sh; if (!(b & 0x80)) break; sh += 7; } }
            PRUEFE(pos + len <= p.payload.size(), "Paket %zu: Laengenfeld %zu ueberschreitet Nutzlast", i, len);
            if (pos + len > p.payload.size()) return obus;
            elems.emplace_back(p.payload.begin() + pos, p.payload.begin() + pos + len);
            pos += len; ++idx;
        }
        if (p.w) PRUEFE(elems.size() == anzahl, "Paket %zu: W=%d aber %zu Elemente", i, p.w, elems.size());
        for (size_t e = 0; e < elems.size(); ++e) {
            bool fortsetzung = (e == 0 && p.z), bleibtOffen = (e + 1 == elems.size() && p.y);
            if (fortsetzung) { PRUEFE(offenAktiv, "Paket %zu: Z gesetzt, aber kein offenes Fragment", i); offen.insert(offen.end(), elems[e].begin(), elems[e].end()); }
            else { PRUEFE(!offenAktiv, "Paket %zu: neues Element, obwohl ein Fragment offen war", i); offen = elems[e]; offenAktiv = true; }
            if (!bleibtOffen) { obus.push_back(offen); offen.clear(); offenAktiv = false; }
        }
    }
    PRUEFE(!offenAktiv, "TU endet mit offenem Fragment");
    return obus;
}
static void pruefeKette(const char* name, const std::vector<Pkt>& pk, bool erwarteN) {
    PRUEFE(!pk.empty(), "%s: keine Pakete", name);
    if (pk.empty()) return;
    for (size_t i = 0; i < pk.size(); ++i) {
        PRUEFE(pk[i].z == (i > 0), "%s: Paket %zu Z=%d (erwartet %d) - Kette gerissen", name, i, (int)pk[i].z, (int)(i > 0));
        PRUEFE(pk[i].y == (i + 1 < pk.size()), "%s: Paket %zu Y=%d (erwartet %d) - Kette gerissen", name, i, (int)pk[i].y, (int)(i + 1 < pk.size()));
        PRUEFE(pk[i].marker == (i + 1 == pk.size()), "%s: Paket %zu Marker=%d", name, i, (int)pk[i].marker);
        PRUEFE(pk[i].n == (i == 0 && erwarteN), "%s: Paket %zu N=%d (erwartet %d)", name, i, (int)pk[i].n, (int)(i == 0 && erwarteN));
        PRUEFE(!(pk[i].n && pk[i].z), "%s: Paket %zu N und Z gleichzeitig (libwebrtc verwirft)", name, i);
    }
}
static void pruefeInhalt(const char* name, const std::vector<Pkt>& pk, const std::vector<rtc::binary>& eingabe) {
    auto obus = setzeZusammen(pk);
    PRUEFE(obus.size() == eingabe.size(), "%s: %zu OBUs zusammengesetzt, %zu erwartet", name, obus.size(), eingabe.size());
    for (size_t i = 0; i < obus.size() && i < eingabe.size(); ++i)
        PRUEFE(obus[i] == erwartet(eingabe[i]), "%s: OBU %zu unterscheidet sich (%zu vs %zu Bytes)", name, i, obus[i].size(), erwartet(eingabe[i]).size());
}
static void fall(const char* name, const std::vector<rtc::binary>& obus, bool erwarteN, size_t maxFrag = 1200, size_t minPakete = 1) {
    auto pk = packetisiere(tu(obus), maxFrag);
    printf("%-58s %zu Pakete\n", name, pk.size());
    PRUEFE(pk.size() >= minPakete, "%s: nur %zu Pakete (mind. %zu erwartet)", name, pk.size(), minPakete);
    pruefeKette(name, pk, erwarteN);
    pruefeInhalt(name, pk, obus);
}

int main() {
    printf("AV1-RTP-Packetizer-Test\n");
    // Form A (AMD, aeltere NVIDIA-Treiber): ein OBU_FRAME je Bild
    fall("Keyframe, ein OBU_FRAME (SeqHdr+FRAME, 6 KB)", { obu(SEQ, 12, 1), obu(FRAME, 6000, 2) }, true, 1200, 5);
    fall("Deltaframe, ein OBU_FRAME (900 B)", { obu(FRAME, 900, 3) }, false);
    // Form B (neuere NVIDIA-Treiber): FRAME_HEADER + TILE_GROUP getrennt
    fall("Keyframe, FRAME_HEADER+TILE_GROUP getrennt (5 KB)", { obu(SEQ, 12, 1), obu(FH, 20, 4), obu(TG, 5000, 5) }, true, 1200, 4);
    fall("Deltaframe, FRAME_HEADER+TILE_GROUP getrennt (900 B)", { obu(FH, 18, 6), obu(TG, 900, 7) }, false);
    fall("Deltaframe, Header+TileGroup klein (ein Paket)", { obu(FH, 18, 6), obu(TG, 300, 7) }, false);
    // Viele kleine OBUs (W=0-Pfad mit Laengenfeld fuer jedes Element) + Metadata
    fall("Keyframe mit 5 Metadata-OBUs + Tile Group", { obu(SEQ, 12, 1), obu(5, 9, 8), obu(5, 9, 9), obu(5, 9, 10), obu(5, 9, 11), obu(5, 9, 12), obu(FH, 20, 4), obu(TG, 3000, 5) }, true);
    // Grenzfall: FRAME_HEADER fuellt Paket 1 EXAKT (1 Aggregationskopf + 1 Laengenbyte + 198)
    fall("Grenzfall: OBU endet exakt an Paketgrenze (maxFrag 200)", { obu(FH, 198, 13), obu(TG, 1500, 14) }, false, 200, 2);
    fall("Grenzfall: OBU endet 1 Byte vor Paketgrenze (maxFrag 200)", { obu(FH, 197, 13), obu(TG, 1500, 14) }, false, 200, 2);
    fall("Grenzfall: SeqHdr+FrameHeader exakt ein Paket (maxFrag 64)", { obu(SEQ, 12, 1), obu(FH, 46, 15), obu(TG, 500, 16) }, true, 64, 2);
    // Padding/TD innerhalb der TU muessen verschwinden, Rest unveraendert
    {
        std::vector<rtc::binary> mitPad = { obu(SEQ, 12, 1), obu(PAD, 40, 0), obu(FH, 20, 4), obu(TG, 2000, 5) };
        std::vector<rtc::binary> ohnePad = { obu(SEQ, 12, 1), obu(FH, 20, 4), obu(TG, 2000, 5) };
        auto pk = packetisiere(tu(mitPad), 1200);
        printf("%-58s %zu Pakete\n", "Padding-OBU wird entfernt", pk.size());
        pruefeKette("Padding", pk, true); pruefeInhalt("Padding", pk, ohnePad);
    }
    // Leere/kaputte Eingaben: keine Pakete, kein Absturz
    { auto pk = packetisiere({ byte(0x12), byte(0x00) }, 1200); PRUEFE(pk.empty(), "TD allein: %zu Pakete statt 0", pk.size()); }
    { auto pk = packetisiere({}, 1200); PRUEFE(pk.empty(), "leer: %zu Pakete statt 0", pk.size()); }
    { auto kurz = tu({ obu(FH, 20, 4), obu(TG, 2000, 5) }); kurz.resize(kurz.size() - 500); auto pk = packetisiere(kurz, 1200);
      PRUEFE(pk.empty() || !pk.back().y, "abgeschnittene TU: letztes Paket haelt die Kette offen"); printf("%-58s %zu Pakete\n", "abgeschnittene TU (nichts Halbes senden)", pk.size()); }

    if (g_fehler) { printf("AV1-RTP-Packetizer-Test: %d FEHLER - Relay-Build abgebrochen\n", g_fehler); return 1; }
    printf("AV1-RTP-Packetizer-Test: ok\n");
    return 0;
}
