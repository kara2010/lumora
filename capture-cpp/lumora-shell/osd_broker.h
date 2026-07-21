// OSD-Broker-Modi der Shell (1:1 aus main.js runFpsBroker/runSenseBroker).
// Die geplanten Aufgaben LumoraOSD-FPS / LumoraOSD-Sensors starten
// "lumora_shell.exe --fps-broker" bzw. "--sensor-broker" elevated; der Broker
// sammelt die Werte und schreibt sie ins Shared Memory, das die UI-Instanz liest.
// Getrennte Schreibbereiche: Broker @0 (magic..pid), App @24 (appTick,wanted).
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <intrin.h>
#include <string>
#include <map>
#include <vector>
#include <set>
#include <algorithm>
#include <cmath>
#include "etw_present.h"

namespace lubroker {

static const uint32_t FPS_MAGIC = 0x4C4F5344;   // 'LOSD'
// PresentMon-Prozesse, die kein Spiel sind (1:1 aus main.js PM_IGNORE)
static bool pmIgnore(const std::string& app) {
    static const std::set<std::string> ig = { "dwm.exe","explorer.exe","lumora.exe","lumora_shell.exe","presentmon.exe",
        "searchhost.exe","textinputhost.exe","shellexperiencehost.exe","startmenuexperiencehost.exe","applicationframehost.exe" };
    std::string a = app; for (auto& c : a) c = (char)tolower((unsigned char)c);
    return ig.count(a) > 0;
}

// Ein einzelner FPS-Broker-Lauf: eigener ETW-Present-Consumer (etw_present.h,
// ersetzt PresentMon.exe), FPS des aktivsten Praesentierers ins SHM.
inline int runFpsBroker(const std::wstring& binDir) {
    (void)binDir;   // frueher: PresentMon.exe-Suche
    HANDLE shm = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, "Local\\LumoraOSDFps");
    if (!shm) return 1;
    uint32_t* mem = (uint32_t*)MapViewOfFile(shm, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!mem) { CloseHandle(shm); return 1; }
    // Laeuft schon ein Broker (frischer brokerTick)? Dann sofort raus (nur EINER darf leben).
    if (mem[0] == FPS_MAGIC && mem[1] && (uint32_t)(GetTickCount() - mem[1]) < 2500) { UnmapViewOfFile(mem); CloseHandle(shm); return 0; }

    // ETW-Events -> Frames je PID (times/frametimes, max 400) - gleiche Struktur wie
    // vorher aus PresentMons CSV; Frametime = Abstand zweier Present-Starts derselben PID.
    struct Frames { std::vector<double> times, ft; };
    static std::map<std::string, Frames> frames;   // pid -> Frames
    static std::mutex mx;
    luetw::PresentTrace trace;
    bool ok = trace.start([&](uint32_t pid, double t) {
        if (!pid || pmIgnore(luetw::pidExeName(pid))) return;
        std::lock_guard<std::mutex> lk(mx);
        auto& e = frames[std::to_string(pid)];
        double ft = e.times.empty() ? 0.0 : (t - e.times.back()) * 1000.0;
        e.times.push_back(t); e.ft.push_back(ft);
        if (e.times.size() > 400) { e.times.erase(e.times.begin()); e.ft.erase(e.ft.begin()); }
    });
    if (!ok) { UnmapViewOfFile(mem); CloseHandle(shm); return 1; }   // keine Adminrechte / Session belegt

    // 60-Hz-Schreibschleife: aktivsten Praesentierer der letzten 0,5 s -> FPS ins SHM.
    uint32_t startTick = GetTickCount(), lastFreshApp = GetTickCount();
    for (;;) {
        Sleep(16);
        uint32_t now = GetTickCount();
        uint32_t appTick = mem[6];   // App-Heartbeat @24
        if (appTick && (uint32_t)(now - appTick) < 3000) lastFreshApp = now;
        if ((uint32_t)(now - startTick) > 8000 && (uint32_t)(now - lastFreshApp) > 5000) break;   // App zu / OSD aus
        double tmax = 0; int outFps = 0, outFtX100 = 0; uint32_t bestPid = 0;
        {
            std::lock_guard<std::mutex> lk(mx);
            for (auto& [pid, e] : frames) if (!e.times.empty()) tmax = (std::max)(tmax, e.times.back());
            int bestC = 0;
            for (auto& [pid, e] : frames) {
                int c = 0;
                for (int i = (int)e.times.size() - 1; i >= 0 && e.times[i] >= tmax - 0.5; --i) c++;
                if (c > bestC) { bestC = c; outFps = (int)(c / 0.5 + 0.5); outFtX100 = (int)(e.ft.back() * 100 + 0.5); bestPid = (uint32_t)atoi(pid.c_str()); }
            }
            if (bestC < 2) { outFps = 0; outFtX100 = 0; }
        }
        mem[0] = FPS_MAGIC; mem[1] = now; mem[2] = (uint32_t)outFps; mem[3] = (uint32_t)outFtX100; mem[4] = 0; mem[5] = bestPid;
    }
    trace.stop();
    UnmapViewOfFile(mem); CloseHandle(shm);
    return 0;
}

// === CPU-Sensor-Broker (PawnIO) ==============================================
// CPU-Temp/-Verbrauch OHNE Afterburner: PawnIO (signierter WinRing0-Nachfolger,
// pawnio.eu) liefert MSR/SMN-Lesezugriff ueber signierte Module. pawnio_open
// braucht Adminrechte -> laeuft (wie der FPS-Broker) elevated in der geplanten
// Aufgabe LumoraOSD-Sensors; Werte wandern per Shared Memory zur UI. 1:1 aus
// main.js runSensorBroker portiert. Sense-SHM-Layout (pack(1), wie main.cpp):
// magic@0, brokerTick@4, tempX10@8 (i32), powerX10@12 (i32), pid@16, _r@20,
// appTick@24, wanted@28. Broker schreibt @0..20, App @24..28.
static const uint32_t SENSE_MAGIC = 0x4C4F5345;   // 'LOSE'

// PawnIO-Modul (.bin) fuer diese CPU suchen: neben der Shell, sonst hoeher / stage.
inline std::wstring findSensorModule(const std::wstring& binDir, const std::wstring& name) {
    for (const std::wstring& d : { binDir, binDir + L"\\..", binDir + L"\\..\\..\\..\\..", binDir + L"\\stage" }) {
        std::wstring p = d + L"\\" + name;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return binDir + L"\\" + name;
}
// Passendes (gebuendeltes, LGPL) PawnIO-Modul: Zen -> AMDFamily17.bin, Intel -> IntelMSR.bin.
inline const wchar_t* cpuSensorModule() {
    int r[4] = { 0 }; __cpuid(r, 0);
    char v[13] = { 0 }; memcpy(v, &r[1], 4); memcpy(v + 4, &r[3], 4); memcpy(v + 8, &r[2], 4);
    if (strcmp(v, "AuthenticAMD") == 0) return L"AMDFamily17.bin";
    if (strcmp(v, "GenuineIntel") == 0) return L"IntelMSR.bin";
    return nullptr;
}

// Ein Sensor-Broker-Lauf: PawnIO oeffnen, Modul laden, 2x/s CPU-Temp/-Power ins
// SHM schreiben. Beendet sich selbst, sobald der App-Heartbeat ausbleibt.
inline int runSensorBroker(const std::wstring& binDir) {
    HANDLE shm = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, "Local\\LumoraOSDSense");
    if (!shm) return 1;
    uint32_t* mem = (uint32_t*)MapViewOfFile(shm, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!mem) { CloseHandle(shm); return 1; }
    // Laeuft schon ein Sensor-Broker (frischer brokerTick)? Dann sofort raus.
    if (mem[0] == SENSE_MAGIC && mem[1] && (uint32_t)(GetTickCount() - mem[1]) < 3500) { UnmapViewOfFile(mem); CloseHandle(shm); return 0; }

    const wchar_t* modName = cpuSensorModule();
    if (!modName) { UnmapViewOfFile(mem); CloseHandle(shm); return 0; }   // CPU nicht unterstuetzt

    // PawnIOLib.dll aus dem Installationsordner (C:\Program Files\PawnIO).
    wchar_t pf[MAX_PATH]; if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) wcscpy_s(pf, L"C:\\Program Files");
    std::wstring dllPath = std::wstring(pf) + L"\\PawnIO\\PawnIOLib.dll";
    HMODULE lib = LoadLibraryW(dllPath.c_str());
    if (!lib) { UnmapViewOfFile(mem); CloseHandle(shm); return 0; }   // Treiber noch nicht installiert
    typedef long (*pio_open_t)(void**);
    typedef long (*pio_load_t)(void*, void*, size_t);
    typedef long (*pio_exec_t)(void*, const char*, const void*, size_t, void*, size_t, size_t*);
    typedef long (*pio_close_t)(void*);
    auto pioOpen  = (pio_open_t)GetProcAddress(lib, "pawnio_open");
    auto pioLoad  = (pio_load_t)GetProcAddress(lib, "pawnio_load");
    auto pioExec  = (pio_exec_t)GetProcAddress(lib, "pawnio_execute");
    auto pioClose = (pio_close_t)GetProcAddress(lib, "pawnio_close");
    if (!pioOpen || !pioLoad || !pioExec || !pioClose) { UnmapViewOfFile(mem); CloseHandle(shm); return 0; }

    void* h = nullptr;
    if (pioOpen(&h) != 0 || !h) { UnmapViewOfFile(mem); CloseHandle(shm); return 0; }   // Adminrechte fehlen

    // Modul-Blob laden.
    std::wstring modPath = findSensorModule(binDir, modName);
    std::vector<unsigned char> blob;
    HANDLE f = CreateFileW(modPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { pioClose(h); UnmapViewOfFile(mem); CloseHandle(shm); return 0; }
    DWORD sz = GetFileSize(f, nullptr); blob.resize(sz); DWORD got = 0; ReadFile(f, blob.data(), sz, &got, nullptr); CloseHandle(f);
    if (got != sz || sz == 0) { pioClose(h); UnmapViewOfFile(mem); CloseHandle(shm); return 0; }
    if (pioLoad(h, blob.data(), blob.size()) != 0) { pioClose(h); UnmapViewOfFile(mem); CloseHandle(shm); return 0; }

    bool amd = wcscmp(modName, L"AMDFamily17.bin") == 0;
    // rd(name, addr): 1 uint64 rein, 1 uint64 raus. true + Wert bei Erfolg.
    auto rd = [&](const char* name, uint64_t addr, uint64_t& out) -> bool {
        uint64_t in = addr, o = 0; size_t retC = 0;
        if (pioExec(h, name, &in, 1, &o, 1, &retC) != 0) return false;
        out = o; return true;
    };

    // Intel: TjMax einmalig (MSR_TEMPERATURE_TARGET 0x1A2, Bits 23:16); AMD braucht keins.
    int tjMax = 100;
    if (!amd) { uint64_t t; if (rd("ioctl_read_msr", 0x1A2, t)) { int v = (int)((t >> 16) & 0xff); if (v > 40 && v < 130) tjMax = v; } }
    // RAPL-Energie-Einheit (AMD 0xC0010299 / Intel 0x606, ESU in Bits 12:8).
    uint32_t energyMsr = amd ? 0xC001029B : 0x611;
    int esu = 16; { uint64_t u; if (rd("ioctl_read_msr", amd ? 0xC0010299 : 0x606, u)) esu = (int)((u >> 8) & 0x1f); }
    double jPerTick = 1.0 / pow(2.0, esu);

    bool haveLastE = false; uint32_t lastE = 0; ULONGLONG lastT = 0; double watts = -1;
    uint32_t startTick = GetTickCount(), lastFreshApp = GetTickCount();
    for (;;) {
        Sleep(500);   // 2 Hz: Temp/Power reagieren fluessig; Power-Delta bleibt genau genug
        uint32_t now = GetTickCount();
        uint32_t appTick = mem[6];   // App-Heartbeat @24
        if (appTick && (uint32_t)(now - appTick) < 3000) lastFreshApp = now;
        if ((uint32_t)(now - startTick) > 8000 && (uint32_t)(now - lastFreshApp) > 5000) break;   // App zu / OSD aus

        // Temperatur: AMD Tctl via SMN THM_TCON_CUR_TMP; Intel via IA32_THERM_STATUS (TjMax-DTS).
        double temp = 0; bool haveTemp = false; uint64_t r;
        if (amd) {
            if (rd("ioctl_read_smn", 0x00059800, r)) {
                uint32_t raw = (uint32_t)r;
                double t = (raw >> 21) * 0.125;
                if (raw & 0x80000) t -= 49;
                if (t > -20 && t < 150) { temp = t; haveTemp = true; }
            }
        } else {
            if (rd("ioctl_read_msr", 0x19C, r)) {
                uint32_t raw = (uint32_t)r;
                if (raw & 0x80000000) { temp = tjMax - (double)((raw >> 16) & 0x7f); haveTemp = true; }   // Reading-Valid-Bit
            }
        }
        // Package-Power: kumulativer Energiezaehler -> Watt = Delta-Energie / Delta-Zeit.
        uint64_t e; ULONGLONG tNow = GetTickCount64();
        if (rd("ioctl_read_msr", energyMsr, e)) {
            uint32_t cur = (uint32_t)e;
            if (haveLastE && tNow > lastT) {
                int64_t dE = (int64_t)cur - (int64_t)lastE; if (dE < 0) dE += 0x100000000LL;   // 32-bit-Wrap
                double w = dE * jPerTick / ((tNow - lastT) / 1000.0);
                if (w >= 0 && w < 1000) watts = w;
            }
            lastE = cur; lastT = tNow; haveLastE = true;
        }
        // Broker-Section @0 schreiben (magic, brokerTick, tempX10, powerX10, pid, _r).
        mem[0] = SENSE_MAGIC; mem[1] = now;
        ((int32_t*)mem)[2] = haveTemp ? (int32_t)lround(temp * 10) : -1;
        ((int32_t*)mem)[3] = watts >= 0 ? (int32_t)lround(watts * 10) : -1;
        mem[4] = GetCurrentProcessId(); mem[5] = 0;
    }
    pioClose(h);
    UnmapViewOfFile(mem); CloseHandle(shm);
    return 0;
}

} // namespace lubroker
