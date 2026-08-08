// Netzwerk-Statistik pro Prozess - eigener Kernel-ETW-Consumer (Quelle des FPS-Brokers).
// System-Logger-Session (wie etw_kernel.h) mit NUR dem TCP/UDP-EnableFlag: die
// klassischen TcpIp-/UdpIp-MOF-Events tragen PID + Bytezahl je Send/Recv, gezaehlt
// direkt am Netzwerk-Stack. Das ist die einzige Quelle, die pro PROZESS stimmt -
// Adapterzaehler mischen alle Programme, IO_COUNTERS mischen Platte und Netz.
// Braucht Adminrechte (der FPS-Broker laeuft ohnehin elevated in LumoraOSD-FPS).
#pragma once
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef EVENT_TRACE_SYSTEM_LOGGER_MODE
#define EVENT_TRACE_SYSTEM_LOGGER_MODE 0x02000000
#endif

namespace luetw {

// MOF-Gruppen-GUIDs (oeffentlich dokumentiert, stabil seit Win2000)
static const GUID GUID_TCPIP = { 0x9a280ac0, 0xc8e0, 0x11d1, {0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2} };
static const GUID GUID_UDPIP = { 0xbf3a50c5, 0xa9c9, 0x4988, {0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80} };
static const GUID GUID_PROC_NET = { 0x3d6fa8d0, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c} };   // Process (Kind-Erkennung)

class NetTrace {
public:
    bool start() {
        stopSessionByName();   // verwaiste Session vom letzten Lauf (Crash) wegputzen
        size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + (name_.size() + 1) * sizeof(wchar_t);
        propsBuf_.assign(sz, 0);
        auto* p = (EVENT_TRACE_PROPERTIES*)propsBuf_.data();
        p->Wnode.BufferSize = (ULONG)sz;
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1;
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        // Netz + Prozess-Starts: Kindprozesse EVENTGETRIEBEN in die Familie aufnehmen.
        // Der 1-s-Toolhelp-Schnappschuss allein verpasste kurzlebige Kinder komplett -
        // gemessen: von 5 nacheinander gestarteten Downloads wurde nur EINER gezaehlt,
        // die anderen vier Prozesse lebten kuerzer als das Schnappschuss-Raster.
        p->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP | EVENT_TRACE_FLAG_PROCESS;
        p->BufferSize = 64; p->MinimumBuffers = 4; p->MaximumBuffers = 16;
        if (StartTraceW(&session_, name_.c_str(), p) != ERROR_SUCCESS) return false;

        EVENT_TRACE_LOGFILEW lf{};
        lf.LoggerName = (LPWSTR)name_.c_str();
        lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        lf.EventRecordCallback = &NetTrace::onEventStatic;
        lf.Context = this;
        consumer_ = OpenTraceW(&lf);
        if (consumer_ == INVALID_PROCESSTRACE_HANDLE) { stop(); return false; }
        thread_ = std::thread([this]() { ProcessTrace(&consumer_, 1, nullptr, nullptr); });
        return true;
    }

    void stop() {
        if (session_) {
            auto* p = (EVENT_TRACE_PROPERTIES*)propsBuf_.data();
            ControlTraceW(session_, nullptr, p, EVENT_TRACE_CONTROL_STOP);
            session_ = 0;
        }
        if (consumer_ != INVALID_PROCESSTRACE_HANDLE) { CloseTrace(consumer_); consumer_ = INVALID_PROCESSTRACE_HANDLE; }
        if (thread_.joinable()) thread_.join();
    }
    ~NetTrace() { stop(); }

    // Neue Sitzung: Ziel setzen + Zaehler leeren ("seit Spielstart", nicht "seit
    // Broker-Start"). Kinder des Ziels sammelt ab jetzt der Prozess-Start-Event-Pfad;
    // Kinder, die es VOR dem Trace-Start schon gab, reicht der Broker per addFamily nach.
    void setTarget(uint32_t pid) {
        std::lock_guard<std::mutex> lk(mx_);
        fam_.clear(); if (pid) fam_.insert(pid);
        counts_.clear();
    }
    void addFamily(uint32_t pid) { std::lock_guard<std::mutex> lk(mx_); if (pid) fam_.insert(pid); }
    std::set<uint32_t> family() { std::lock_guard<std::mutex> lk(mx_); return fam_; }
    // Summen der PID-Familie (Ziel + alle je gesehenen Nachkommen - auch beendete:
    // deren Bytes gehoeren weiter zur Sitzung).
    void bytesFam(uint64_t& in, uint64_t& out) {
        in = 0; out = 0;
        std::lock_guard<std::mutex> lk(mx_);
        for (uint32_t pid : fam_) {
            auto it = counts_.find(pid);
            if (it != counts_.end()) { in += it->second.in; out += it->second.out; }
        }
    }

    // Diagnose: was kommt WIRKLICH an? Je Opcode Ereignisse+Bytes (ueber alle PIDs)
    // plus die Top-PIDs nach Volumen. Ohne diesen Blick bleibt bei "es zaehlt fast
    // nichts" nur Raten - genau der Fall, der real aufgetreten ist.
    std::string diag() {
        std::lock_guard<std::mutex> lk(mx_);
        std::string s = "opcodes:";
        for (auto& [op, st] : opstats_) {
            char b[64]; sprintf_s(b, " %d=%llux/%lluB", op, (unsigned long long)st.first, (unsigned long long)st.second);
            s += b;
        }
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (auto& [pid, c] : counts_) top.push_back({ c.in + c.out, pid });
        std::sort(top.rbegin(), top.rend());
        s += " | top-pids:";
        for (size_t i = 0; i < top.size() && i < 8; ++i) {
            auto& c = counts_[top[i].second];
            char b[96]; sprintf_s(b, " %u(in=%llu,out=%llu)", top[i].second, (unsigned long long)c.in, (unsigned long long)c.out);
            s += b;
        }
        auto topOf = [](std::map<uint32_t, uint64_t>& m) {
            std::vector<std::pair<uint64_t, uint32_t>> t;
            for (auto& [pid, b] : m) t.push_back({ b, pid });
            std::sort(t.rbegin(), t.rend());
            std::string r;
            for (size_t i = 0; i < t.size() && i < 5; ++i) {
                char b[48]; sprintf_s(b, " %u=%lluB", t[i].second, (unsigned long long)t[i].first); r += b;
            }
            return r;
        };
        s += " | copy-nutzdaten:" + topOf(diagPay_) + " | copy-header:" + topOf(diagHdr_);
        return s;
    }

private:
    struct Cnt { uint64_t in = 0, out = 0; };

    void stopSessionByName() {
        size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + 1024;
        std::vector<char> buf(sz, 0);
        auto* p = (EVENT_TRACE_PROPERTIES*)buf.data();
        p->Wnode.BufferSize = (ULONG)sz;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(0, name_.c_str(), p, EVENT_TRACE_CONTROL_STOP);
    }

    static void WINAPI onEventStatic(EVENT_RECORD* er) {
        ((NetTrace*)er->UserContext)->onEvent(er);
    }
    void onEvent(EVENT_RECORD* er) {
        const auto& h = er->EventHeader;
        // Prozess-Start (Process_TypeGroup1, 64-bit: UniqueProcessKey ptr @0,
        // ProcessId u32 @8, ParentId u32 @12): Kind eines Familienmitglieds -> dazu.
        if (IsEqualGUID(h.ProviderId, GUID_PROC_NET)) {
            if (h.EventDescriptor.Opcode == 1 && er->UserDataLength >= 16) {
                const uint8_t* d = (const uint8_t*)er->UserData;
                uint32_t pid = *(const uint32_t*)(d + 8), eltern = *(const uint32_t*)(d + 12);
                std::lock_guard<std::mutex> lk(mx_);
                if (fam_.count(eltern)) fam_.insert(pid);
            }
            return;
        }
        const bool tcp = IsEqualGUID(h.ProviderId, GUID_TCPIP);
        if (!tcp && !IsEqualGUID(h.ProviderId, GUID_UDPIP)) return;
        // MOF-Opcodes: 10=Send v4, 11=Recv v4, 26=Send v6, 27=Recv v6.
        // TCP hat zusaetzlich 12..18 (Connect/Disconnect/Retransmit...) - die tragen
        // keine Nutzdaten und werden ignoriert.
        UCHAR op = h.EventDescriptor.Opcode;
        {   // Diagnose: JEDES Provider-Event zaehlen (auch nicht behandelte Opcodes)
            std::lock_guard<std::mutex> lk(mx_);
            auto& st = opstats_[(int)op + (tcp ? 0 : 100)];   // UDP als 1xx ablegen
            st.first++;
            if (er->UserDataLength >= 8) st.second += ((const uint32_t*)er->UserData)[1];
            // Zuordnungs-Frage bei den Copy-Events: Nutzdaten-PID vs. Header-PID?
            // Beide parallel mitschreiben - entschieden wird nach Messung, nicht Meinung.
            if (tcp && (op == 18 || op == 34) && er->UserDataLength >= 8) {
                uint32_t sz = ((const uint32_t*)er->UserData)[1];
                diagPay_[((const uint32_t*)er->UserData)[0]] += sz;
                diagHdr_[h.ProcessId] += sz;
            }
        }
        // GEMESSEN (net-diagnose): die TCP-Recv-Events (11/27) buchen ihre Bytes auf
        // PID 4 (System) - der Empfang laeuft im Kernel-Kontext, nicht im Programm.
        // Die Zuordnung zum ECHTEN Empfaenger traegt erst das Copy-Event (18, v6: 34),
        // wenn die Daten in den Puffer der Anwendung wandern. Darum zaehlt TCP-Empfang
        // ueber 18/34; 11/27 zu nehmen hiesse doppelt zaehlen UND falsch zuordnen.
        // UDP hat keine Copy-Events - dort ist Recv (11/27) korrekt im Prozesskontext.
        const bool send = (op == 10 || op == 26);
        const bool recv = tcp ? (op == 18 || op == 34) : (op == 11 || op == 27);
        if (!send && !recv) return;
        // Payload beginnt mit PID (u32) + size (u32) - bei v4 wie v6 identisch.
        // WICHTIG: h.ProcessId ist hier oft -1 (Kernel-Kontext), die echte PID steht
        // in den Nutzdaten.
        if (er->UserDataLength < 8) return;
        const uint32_t* d = (const uint32_t*)er->UserData;
        uint32_t pid = d[0], size = d[1];
        if (!pid || pid == 0xFFFFFFFF) return;
        std::lock_guard<std::mutex> lk(mx_);
        Cnt& c = counts_[pid];
        if (send) c.out += size; else c.in += size;
        // Backstop gegen unbegrenztes Wachstum (viele kurzlebige PIDs): grob begrenzen.
        if (counts_.size() > 2048) counts_.clear();
    }

    std::wstring name_ = L"LumoraNetTrace";
    TRACEHANDLE session_ = 0;
    TRACEHANDLE consumer_ = INVALID_PROCESSTRACE_HANDLE;
    std::vector<char> propsBuf_;
    std::thread thread_;
    std::mutex mx_;
    std::map<uint32_t, Cnt> counts_;
    std::set<uint32_t> fam_;                                 // Ziel + alle je gesehenen Nachkommen
    std::map<int, std::pair<uint64_t, uint64_t>> opstats_;   // Opcode -> (Ereignisse, Bytes); UDP als 1xx
    std::map<uint32_t, uint64_t> diagPay_, diagHdr_;         // Copy-Events: Bytes je Nutzdaten- bzw. Header-PID
};

} // namespace luetw
