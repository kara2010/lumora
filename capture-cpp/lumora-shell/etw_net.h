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
        p->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP;   // NUR Netz - kein CSwitch, kein Disk
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

    // Summen fuer eine PID-Menge (Spiel + Kindprozesse - Launcher starten den
    // eigentlichen Spielprozess oft als Kind).
    void bytesFor(const std::set<uint32_t>& pids, uint64_t& in, uint64_t& out) {
        in = 0; out = 0;
        std::lock_guard<std::mutex> lk(mx_);
        for (uint32_t pid : pids) {
            auto it = counts_.find(pid);
            if (it != counts_.end()) { in += it->second.in; out += it->second.out; }
        }
    }
    // Neue Sitzung: Zaehler leeren, damit "seit Spielstart" gilt und nicht "seit
    // Broker-Start" (der Broker ueberlebt mehrere Spielsitzungen).
    void reset() { std::lock_guard<std::mutex> lk(mx_); counts_.clear(); }

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
        const bool tcp = IsEqualGUID(h.ProviderId, GUID_TCPIP);
        if (!tcp && !IsEqualGUID(h.ProviderId, GUID_UDPIP)) return;
        // MOF-Opcodes: 10=Send v4, 11=Recv v4, 26=Send v6, 27=Recv v6.
        // TCP hat zusaetzlich 12..18 (Connect/Disconnect/Retransmit...) - die tragen
        // keine Nutzdaten und werden ignoriert.
        UCHAR op = h.EventDescriptor.Opcode;
        const bool send = (op == 10 || op == 26);
        const bool recv = (op == 11 || op == 27);
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
};

} // namespace luetw
