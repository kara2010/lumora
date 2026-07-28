// Kernel-ETW-Konsument fuer die Ruckler-Blackbox (Design: capture-cpp/BOTTLENECK-PLAN.md).
// EIGENE System-Logger-Session (EVENT_TRACE_SYSTEM_LOGGER_MODE, Win10 2004+/Build 19041):
// liefert die klassischen Kernel-Events (DPC/ISR mit Treiber-Routine, Context-Switches,
// Disk-I/O, Prozess-Start/Ende) OHNE den Legacy "NT Kernel Logger" zu belegen - der
// existiert nur 1x systemweit und wuerde mit LatencyMon/xperf kollidieren; System-Logger-
// Sessions gibt es bis zu 8 parallel. Aktivierung ueber die klassischen EnableFlags
// (dokumentierter Weg fuer System-Logger-Sessions) -> Events kommen mit den seit
// Jahrzehnten stabilen MOF-Layouts der Kernel-Gruppen (PerfInfo/Thread/DiskIo/Process),
// keine fragilen Keyword-Konstanten der neuen Einzel-Provider noetig.
// Braucht Adminrechte -> laeuft ausschliesslich im elevated Analyze-Broker
// (geplante Aufgabe LumoraOSD-Analyze, gleiches Muster wie LumoraOSD-FPS).
//
// WICHTIG (Overhead-Budget <1%, s. BOTTLENECK-PLAN Abschnitt 4): Die Callbacks laufen
// auf dem ProcessTrace-Thread. Der CSwitch-Callback feuert unter Last zehntausende
// Male pro Sekunde - der Konsument (StutterAnalyzer) darf dort NUR Integer-Arithmetik
// ohne Allokation/Strings/Logs machen. Diese Datei selbst haelt sich daran: die
// TID->PID-Tabelle und die Treiber-Tabelle sind feste, vorallokierte Strukturen.
#pragma once
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <psapi.h>
#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace luetw {

// Aeltere SDKs definieren den Modus nicht - Wert ist oeffentlich dokumentiert.
#ifndef EVENT_TRACE_SYSTEM_LOGGER_MODE
#define EVENT_TRACE_SYSTEM_LOGGER_MODE 0x02000000
#endif

// MOF-Gruppen-GUIDs der klassischen Kernel-Events (oeffentlich dokumentiert, stabil)
static const GUID GUID_PERFINFO = { 0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc} };
static const GUID GUID_THREAD   = { 0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c} };
static const GUID GUID_DISKIO   = { 0x3d6fa8d4, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c} };
static const GUID GUID_PROCESS  = { 0x3d6fa8d0, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c} };
// System-Provider (Win10 2004+): DPC/ISR kommen NICHT ueber die klassischen EnableFlags
// (gemessen 2026-07-28: CSwitch/Thread/Disk/Process ja, PerfInfo-DPC/ISR nein) - sie
// muessen zusaetzlich per EnableTraceEx2 mit diesem Provider + Keywords angefordert werden.
static const GUID GUID_SYS_INTERRUPT = { 0xd4bbee17, 0xb545, 0x4888, {0x85, 0x8b, 0x74, 0x41, 0x69, 0x01, 0x5b, 0x25} };
static const ULONGLONG SYSINT_KW_GENERAL = 0x1, SYSINT_KW_DPC = 0x4, SYSINT_KW_WDF_DPC = 0x10, SYSINT_KW_WDF_INTERRUPT = 0x20;
// Opcodes (MOF: PerfInfo ISR=67, DPC=68, TimerDPC=69, ThreadDPC=66; Thread CSwitch=36,
// Start/End/DCStart/DCEnd=1/2/3/4; DiskIo Read=10 Write=11; Process Start/End=1/2)
static const UCHAR OP_ISR = 67, OP_DPC = 68, OP_TIMERDPC = 69, OP_THREADDPC = 66;
static const UCHAR OP_CSWITCH = 36, OP_THR_START = 1, OP_THR_END = 2, OP_THR_DCSTART = 3;
static const UCHAR OP_DISK_READ = 10, OP_DISK_WRITE = 11;
static const UCHAR OP_PROC_START = 1, OP_PROC_END = 2;

// Geladene Kernel-Treiber als sortierte Basisadress-Tabelle: Routine-Adresse aus
// DPC/ISR-Events -> Treibername per Binaersuche (groesste Basis <= Adresse).
// EnumDeviceDrivers liefert keine Groessen - die naechste Basis begrenzt nach oben.
class DriverTable {
public:
    void build() {
        bases_.clear(); names_.clear();
        LPVOID img[2048]; DWORD need = 0;
        if (!EnumDeviceDrivers(img, sizeof(img), &need)) return;
        int n = (int)(need / sizeof(LPVOID)); if (n > 2048) n = 2048;
        std::vector<std::pair<uint64_t, std::string>> v;
        v.reserve(n);
        for (int i = 0; i < n; ++i) {
            wchar_t nm[64] = {};
            if (!K32GetDeviceDriverBaseNameW(img[i], nm, 63)) continue;
            std::string s; for (wchar_t c : nm) { if (!c) break; s += (char)tolower((int)c); }
            v.push_back({ (uint64_t)img[i], std::move(s) });
        }
        std::sort(v.begin(), v.end());
        bases_.reserve(v.size()); names_.reserve(v.size());
        for (auto& e : v) { bases_.push_back(e.first); names_.push_back(std::move(e.second)); }
    }
    // Index in die Tabelle (fuer den heissen Pfad: KEINE Strings) - -1 = unbekannt
    int indexOf(uint64_t routine) const {
        if (bases_.empty() || routine < bases_[0]) return -1;
        auto it = std::upper_bound(bases_.begin(), bases_.end(), routine);
        return (int)(it - bases_.begin()) - 1;
    }
    const std::string& name(int idx) const { static std::string leer = "(unbekannt)"; return (idx >= 0 && idx < (int)names_.size()) ? names_[idx] : leer; }
    size_t size() const { return names_.size(); }
private:
    std::vector<uint64_t> bases_;
    std::vector<std::string> names_;
};

class KernelTrace {
public:
    // Alle Callbacks laufen auf dem ProcessTrace-Thread. t in Sekunden auf der t0-Basis.
    struct Sinks {
        // DPC oder ISR eines Treibers: durUs = Dauer der Routine (Event-Ende minus InitialTime)
        std::function<void(double t, double durUs, int driverIdx, bool isr)> dpcIsr;
        // Context-Switch auf Kern cpu: newTid uebernimmt (0 = Idle). pid via pidOfTid().
        std::function<void(double t, uint32_t cpu, uint32_t oldTid, uint32_t newTid)> cswitch;
        // Disk-Transfer abgeschlossen: latMs = HighResResponseTime
        std::function<void(double t, uint32_t bytes, double latMs)> diskIo;
        // Prozess gestartet/beendet
        std::function<void(double t, uint32_t pid, bool start)> proc;
    };

    // t0Shared: gemeinsame QPC-Basis mit anderen Quellen (0 = eigenes t0 beim ersten Event).
    bool start(Sinks sinks, int64_t t0Shared = 0) {
        sinks_ = std::move(sinks);
        QueryPerformanceFrequency((LARGE_INTEGER*)&qpf_);
        t0_ = t0Shared;
        drivers_.build();
        tidPid_.assign(TIDMAP_SLOTS * 2, 0);

        stopSessionByName();   // verwaiste Session vom letzten (abgestuerzten) Lauf raeumen

        size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(SESSION_NAME) + 1) * sizeof(wchar_t);
        propsBuf_.assign(sz, 0);
        auto* p = (EVENT_TRACE_PROPERTIES*)propsBuf_.data();
        p->Wnode.BufferSize = (ULONG)sz;
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1;   // QPC-Zeitstempel (wie die Present-Session)
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        // Puffer klein halten: 64-KB-Puffer, moderat viele -> geringe Latenz, wenig RAM
        p->BufferSize = 64; p->MinimumBuffers = 16; p->MaximumBuffers = 64;
        p->FlushTimer = 1;
        // THREAD zwingend mit: CSwitch traegt nur TIDs; die Thread-Events (inkl. DCStart-
        // Rundown aller existierenden Threads beim Sessionstart) liefern TID->PID.
        p->EnableFlags = EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_DPC | EVENT_TRACE_FLAG_INTERRUPT
                       | EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_THREAD | EVENT_TRACE_FLAG_PROCESS;
        ULONG st = StartTraceW(&session_, SESSION_NAME, p);
        if (st != ERROR_SUCCESS) { lastError_ = st; return false; }
        // DPC/ISR nachtraeglich ueber den System-Interrupt-Provider anfordern (s.o.).
        // Fehlschlag ist NICHT fatal: ohne DPC/ISR bleibt die Analyse per CSwitch/Disk
        // brauchbar (Prozess-Verdaechtige), nur Treiber-Verdaechtige entfallen.
        enableIntrErr_ = EnableTraceEx2(session_, &GUID_SYS_INTERRUPT, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                        TRACE_LEVEL_INFORMATION,
                                        SYSINT_KW_GENERAL | SYSINT_KW_DPC | SYSINT_KW_WDF_DPC | SYSINT_KW_WDF_INTERRUPT,
                                        0, 0, nullptr);

        EVENT_TRACE_LOGFILEW lf{};
        lf.LoggerName = (LPWSTR)SESSION_NAME;
        lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        lf.EventRecordCallback = &KernelTrace::onEventStatic;
        lf.Context = this;
        consumer_ = OpenTraceW(&lf);
        if (consumer_ == INVALID_PROCESSTRACE_HANDLE) { lastError_ = GetLastError(); stop(); return false; }

        thread_ = std::thread([this]() {
            // Konsument bewusst niederprior: ETW puffert kernel-seitig, wir duerfen hinterherlesen
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            ProcessTrace(&consumer_, 1, nullptr, nullptr);
        });
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
    ~KernelTrace() { stop(); }

    // CSwitch-Flut-Notbremse (Selbst-CPU-Watchdog im Broker): nur den teuersten Flag abschalten,
    // DPC/ISR/Disk bleiben aktiv. THREAD bleibt an (billig, PID-Aufloesung weiter noetig).
    void dropCSwitch() {
        if (!session_) return;
        auto* p = (EVENT_TRACE_PROPERTIES*)propsBuf_.data();
        p->EnableFlags &= ~EVENT_TRACE_FLAG_CSWITCH;
        ControlTraceW(session_, nullptr, p, EVENT_TRACE_CONTROL_UPDATE);
    }

    // TID -> PID aus der Thread-Event-Tabelle (heisser Pfad: offene Adressierung, keine Locks -
    // Schreiber und Leser sind derselbe ProcessTrace-Thread). 0 = Idle/unbekannt.
    uint32_t pidOfTid(uint32_t tid) const {
        if (!tid) return 0;
        uint32_t h = (tid * 2654435761u) & (TIDMAP_SLOTS - 1);
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t s = (h + i) & (TIDMAP_SLOTS - 1);
            uint32_t k = tidPid_[s * 2];
            if (k == tid) return tidPid_[s * 2 + 1];
            if (k == 0) return 0;
        }
        return 0;
    }
    const DriverTable& drivers() const { return drivers_; }
    ULONG lastError() const { return lastError_; }
    ULONG enableIntrError() const { return enableIntrErr_; }
    // Diagnose (nur fuer --analyze-dump): Zaehler je Provider-GUID+Opcode einschalten.
    void setGuidStats(std::map<std::string, uint64_t>* m) { guidStats_ = m; }
    int64_t t0() const { return t0_; }
    int64_t qpf() const { return qpf_; }
    uint64_t eventCount() const { return eventCount_; }

    static void stopOrphan() {   // beim App-/Broker-Start: Session-Leiche eines Absturzes raeumen
        KernelTrace k; k.stopSessionByName();
    }

private:
    static constexpr const wchar_t* SESSION_NAME = L"LumoraAnalyzeTrace";
    static constexpr uint32_t TIDMAP_SLOTS = 16384;   // 2er-Potenz (Maske)

    void stopSessionByName() {
        size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + 1024;
        std::vector<char> buf(sz, 0);
        auto* p = (EVENT_TRACE_PROPERTIES*)buf.data();
        p->Wnode.BufferSize = (ULONG)sz;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(0, SESSION_NAME, p, EVENT_TRACE_CONTROL_STOP);
    }

    void tidPidSet(uint32_t tid, uint32_t pid) {
        if (!tid) return;
        uint32_t h = (tid * 2654435761u) & (TIDMAP_SLOTS - 1);
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t s = (h + i) & (TIDMAP_SLOTS - 1);
            uint32_t k = tidPid_[s * 2];
            if (k == 0 || k == tid) { tidPid_[s * 2] = tid; tidPid_[s * 2 + 1] = pid; return; }
        }
        // Tabelle in dieser Region voll: Eintrag verfaellt (naechster CSwitch dieses TIDs
        // liefert dann pid 0 -> zaehlt als "unbekannt", verfaelscht keine Verdaechtigen).
    }

    static void WINAPI onEventStatic(EVENT_RECORD* er) {
        ((KernelTrace*)er->UserContext)->onEvent(er);
    }

    double toT(int64_t qpc) {
        if (!t0_) t0_ = qpc;
        return (double)(qpc - t0_) / (double)qpf_;
    }

    void onEvent(EVENT_RECORD* er) {
        const auto& h = er->EventHeader;
        UCHAR op = h.EventDescriptor.Opcode;
        const uint8_t* d = (const uint8_t*)er->UserData;
        USHORT len = er->UserDataLength;
        ++eventCount_;
        if (guidStats_) {   // Diagnosemodus: welche GUID/Opcode-Kombis kommen wirklich an?
            char k[80]; const GUID& g = h.ProviderId;
            sprintf_s(k, "%08lX-%04X-%04X-%02X%02X op=%u id=%u len=%u", g.Data1, g.Data2, g.Data3,
                      g.Data4[0], g.Data4[1], (unsigned)op, (unsigned)h.EventDescriptor.Id, (unsigned)len);
            (*guidStats_)[k]++;
        }

        if (IsEqualGUID(h.ProviderId, GUID_PERFINFO)) {
            // DPC/ISR (64-bit-Layout): InitialTime u64 @0 (QPC, Routinen-EINTRITT; das Event
            // selbst wird beim AUSTRITT geloggt -> Dauer = EventTs - InitialTime, xperf-Methode),
            // Routine ptr @8.
            if ((op == OP_DPC || op == OP_TIMERDPC || op == OP_THREADDPC || op == OP_ISR) && len >= 16) {
                int64_t init = *(const int64_t*)(d);
                uint64_t routine = *(const uint64_t*)(d + 8);
                // GEMESSEN 2026-07-28 (Dump v3): InitialTime und EventHeader.TimeStamp laufen auf
                // System-Logger-Sessions in VERSCHIEDENEN Epochen (Differenz konstant ~1.34e17
                // Ticks = FILETIME- gegen QPC-Basis), aber mit IDENTISCHER Tickrate (beide 10 MHz).
                // Beweis: die Differenz streute ueber 5 s nur um 242 us - das sind die echten
                // Routinen-Dauern auf einem konstanten Sockel. Also den Sockel selbst
                // herauskalibrieren statt Epochen zu raten: kleinste beobachtete Differenz =
                // kuerzeste Routine (~0 us). Kalibrierfenster 30 s gegen Uhren-Drift (NTP).
                int64_t rawDelta = h.TimeStamp.QuadPart - init;
                if (rawDelta < winMin_) winMin_ = rawDelta;
                if (epochOff_ == INT64_MAX) { epochOff_ = winMin_; winStart_ = h.TimeStamp.QuadPart; }
                if (h.TimeStamp.QuadPart - winStart_ > qpf_ * 30) {
                    epochOff_ = winMin_; winMin_ = INT64_MAX; winStart_ = h.TimeStamp.QuadPart;
                }
                double durUs = (double)(rawDelta - epochOff_) * 1e6 / (double)qpf_;
                if (durUs < 0) durUs = 0;
                if (sinks_.dpcIsr) sinks_.dpcIsr(toT(h.TimeStamp.QuadPart), durUs, drivers_.indexOf(routine), op == OP_ISR);
            }
            return;
        }
        if (IsEqualGUID(h.ProviderId, GUID_THREAD)) {
            if (op == OP_CSWITCH && len >= 8) {
                // CSwitch: NewThreadId u32 @0, OldThreadId u32 @4. Kern-Nummer aus dem
                // Puffer-Kontext (PROCESSOR_INDEX-Variante fuer >64-Kern-Systeme beachten).
                uint32_t newTid = *(const uint32_t*)(d);
                uint32_t oldTid = *(const uint32_t*)(d + 4);
                uint32_t cpu = (h.Flags & EVENT_HEADER_FLAG_PROCESSOR_INDEX)
                    ? er->BufferContext.ProcessorIndex : er->BufferContext.ProcessorNumber;
                if (sinks_.cswitch) sinks_.cswitch(toT(h.TimeStamp.QuadPart), cpu, oldTid, newTid);
            } else if ((op == OP_THR_START || op == OP_THR_DCSTART || op == 4 /*DCEnd traegt auch PID/TID*/) && len >= 8) {
                // Thread_TypeGroup1: ProcessId u32 @0, TThreadId u32 @4. DCStart = Rundown
                // aller BEIM SESSIONSTART existierenden Threads -> Tabelle ist sofort komplett.
                uint32_t pid = *(const uint32_t*)(d);
                uint32_t tid = *(const uint32_t*)(d + 4);
                tidPidSet(tid, pid);
            }
            return;
        }
        if (IsEqualGUID(h.ProviderId, GUID_DISKIO)) {
            if ((op == OP_DISK_READ || op == OP_DISK_WRITE) && len >= 48) {
                // DiskIo_TypeGroup1 (64-bit): TransferSize u32 @8, HighResResponseTime u64 @40 (QPC-Ticks)
                uint32_t bytes = *(const uint32_t*)(d + 8);
                uint64_t resp = *(const uint64_t*)(d + 40);
                double latMs = (double)resp * 1000.0 / (double)qpf_;
                if (sinks_.diskIo) sinks_.diskIo(toT(h.TimeStamp.QuadPart), bytes, latMs);
            }
            return;
        }
        if (IsEqualGUID(h.ProviderId, GUID_PROCESS)) {
            if ((op == OP_PROC_START || op == OP_PROC_END) && len >= 12) {
                // Process_TypeGroup1 (64-bit): UniqueProcessKey ptr @0, ProcessId u32 @8.
                // Den Exe-Namen NICHT aus dem Payload parsen (variables Layout) - der
                // Konsument loest ihn bei Bedarf ueber pidExeName() auf (Prozess lebt gerade).
                uint32_t pid = *(const uint32_t*)(d + 8);
                if (sinks_.proc) sinks_.proc(toT(h.TimeStamp.QuadPart), pid, op == OP_PROC_START);
            }
            return;
        }
    }

    Sinks sinks_;
    DriverTable drivers_;
    std::vector<uint32_t> tidPid_;   // [slot*2]=tid, [slot*2+1]=pid
    TRACEHANDLE session_ = 0;
    TRACEHANDLE consumer_ = INVALID_PROCESSTRACE_HANDLE;
    std::vector<char> propsBuf_;
    std::thread thread_;
    int64_t qpf_ = 1, t0_ = 0;
    int64_t epochOff_ = INT64_MAX, winMin_ = INT64_MAX, winStart_ = 0;   // Epochen-Sockel der DPC/ISR-Dauern
    ULONG lastError_ = 0, enableIntrErr_ = 0;
    uint64_t eventCount_ = 0;
    std::map<std::string, uint64_t>* guidStats_ = nullptr;   // nur im Diagnosemodus gesetzt
};

} // namespace luetw
