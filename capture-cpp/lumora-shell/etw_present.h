// Eigener ETW-Present-Consumer - ersetzt PresentMon.exe (Datenquelle des FPS-Brokers).
// Realtime-ETW-Session auf die User-Mode-Provider Microsoft-Windows-DXGI und
// Microsoft-Windows-D3D9: deren Present_Start-Events (ein Event pro Present()-Aufruf
// des Spiels) liefern PID + QPC-Zeitstempel - exakt die Basis von PresentMons
// "msBetweenPresents" (v1). Braucht Adminrechte (der FPS-Broker laeuft ohnehin
// elevated in der geplanten Aufgabe LumoraOSD-FPS).
#pragma once
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace luetw {

// Provider-GUIDs (oeffentlich dokumentiert, identisch zu PresentMons Quellen)
static const GUID GUID_DXGI = { 0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9} };
static const GUID GUID_D3D9 = { 0x783ACA0A, 0x790E, 0x4D7F, {0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9} };
// Event-IDs der Present-Start-Events (PresentMon: DXGI Present_Start=42, PresentMulti_Start=55; D3D9 Present_Start=1)
static const USHORT DXGI_PRESENT_START = 42, DXGI_PRESENTMULTI_START = 55, D3D9_PRESENT_START = 1;

class PresentTrace {
public:
    // cb(pid, tSeconds): ein Present()-Aufruf von Prozess pid zum Zeitpunkt t (Sekunden, QPC-basiert).
    using Callback = std::function<void(uint32_t pid, double tSeconds)>;

    bool start(Callback cb) {
        cb_ = std::move(cb);
        QueryPerformanceFrequency((LARGE_INTEGER*)&qpf_);

        // Evtl. verwaiste Session vom letzten Lauf stoppen (wie PresentMons --stop_existing_session)
        stopSessionByName();

        // Realtime-Session anlegen. ClientContext=1 -> Event-Timestamps sind QPC.
        size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(SESSION_NAME) + 1) * sizeof(wchar_t);
        propsBuf_.assign(sz, 0);
        auto* p = (EVENT_TRACE_PROPERTIES*)propsBuf_.data();
        p->Wnode.BufferSize = (ULONG)sz;
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1;                      // QPC
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        if (StartTraceW(&session_, SESSION_NAME, p) != ERROR_SUCCESS) return false;

        // Beide Present-Provider aktivieren (alle Keywords; gefiltert wird im Callback per Event-ID)
        EnableTraceEx2(session_, &GUID_DXGI, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                       TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
        EnableTraceEx2(session_, &GUID_D3D9, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                       TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);

        EVENT_TRACE_LOGFILEW lf{};
        lf.LoggerName = (LPWSTR)SESSION_NAME;
        lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        lf.EventRecordCallback = &PresentTrace::onEventStatic;
        lf.Context = this;
        consumer_ = OpenTraceW(&lf);
        if (consumer_ == INVALID_PROCESSTRACE_HANDLE) { stop(); return false; }

        // ProcessTrace blockiert -> eigener Thread; kehrt zurueck, wenn die Session stoppt.
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
    ~PresentTrace() { stop(); }

private:
    static constexpr const wchar_t* SESSION_NAME = L"LumoraOSDTrace";

    void stopSessionByName() {
        size_t sz = sizeof(EVENT_TRACE_PROPERTIES) + 1024;
        std::vector<char> buf(sz, 0);
        auto* p = (EVENT_TRACE_PROPERTIES*)buf.data();
        p->Wnode.BufferSize = (ULONG)sz;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(0, SESSION_NAME, p, EVENT_TRACE_CONTROL_STOP);
    }

    static void WINAPI onEventStatic(EVENT_RECORD* er) {
        ((PresentTrace*)er->UserContext)->onEvent(er);
    }
    void onEvent(EVENT_RECORD* er) {
        const auto& h = er->EventHeader;
        USHORT id = h.EventDescriptor.Id;
        bool present =
            (IsEqualGUID(h.ProviderId, GUID_DXGI) && (id == DXGI_PRESENT_START || id == DXGI_PRESENTMULTI_START)) ||
            (IsEqualGUID(h.ProviderId, GUID_D3D9) && id == D3D9_PRESENT_START);
        if (!present) return;
        if (!t0_) t0_ = h.TimeStamp.QuadPart;
        double t = (double)(h.TimeStamp.QuadPart - t0_) / (double)qpf_;
        cb_(h.ProcessId, t);
    }

    Callback cb_;
    TRACEHANDLE session_ = 0;
    TRACEHANDLE consumer_ = INVALID_PROCESSTRACE_HANDLE;
    std::vector<char> propsBuf_;
    std::thread thread_;
    int64_t qpf_ = 1, t0_ = 0;
};

// PID -> Exe-Name (lowercase, ohne Pfad) mit kleinem Cache - fuer den pmIgnore-Filter.
inline std::string pidExeName(uint32_t pid) {
    static std::map<uint32_t, std::string> cache;
    static std::mutex mx;
    {
        std::lock_guard<std::mutex> lk(mx);
        auto it = cache.find(pid);
        if (it != cache.end()) return it->second;
    }
    std::string name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            std::wstring w(buf, n);
            size_t sl = w.find_last_of(L"\\/");
            if (sl != std::wstring::npos) w = w.substr(sl + 1);
            for (wchar_t c : w) name += (char)tolower((int)c);
        }
        CloseHandle(h);
    }
    std::lock_guard<std::mutex> lk(mx);
    if (cache.size() > 512) cache.clear();
    cache[pid] = name;
    return name;
}

} // namespace luetw
