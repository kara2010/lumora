// OSD-Broker-Modi der Shell (1:1 aus main.js runFpsBroker/runSenseBroker).
// Die geplanten Aufgaben LumoraOSD-FPS / LumoraOSD-Sensors starten
// "lumora_shell.exe --fps-broker" bzw. "--sensor-broker" elevated; der Broker
// sammelt die Werte und schreibt sie ins Shared Memory, das die UI-Instanz liest.
// Getrennte Schreibbereiche: Broker @0 (magic..pid), App @24 (appTick,wanted).
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <pdh.h>
#include <intrin.h>
#include <shlobj.h>      // SHGetKnownFolderPath (analyzeDir: %APPDATA% im elevated Broker)
#include <knownfolders.h>
#include <mutex>
#include <string>
#include <map>
#include <vector>
#include <set>
#include <algorithm>
#include <cmath>
#include <tlhelp32.h>
#include "etw_present.h"
#include "etw_kernel.h"
#include "etw_net.h"
#include "stutter_analyzer.h"

namespace lubroker {

// === Ruckler-Blackbox: Mess-Broker (--analyze-broker) ==============================
// Explizite Mess-Session: Shell setzt wanted=1 im Kontroll-SHM und startet die
// geplante Aufgabe LumoraOSD-Analyze; dieser Broker faehrt Kernel-ETW + eigene
// Present-Session + NVML-Probe hoch, fuettert den StutterAnalyzer, schreibt Befunde
// live nach findings.jsonl und beim Stop den fertigen Bericht (JSON, mit Kontext-
// Metadaten aus context.json der Shell). SHM-Layout (pack via u32-Indizes, 256 B):
//   Broker: [0]='LOSA' [1]=brokerTick [2]=state(1 messen/2 Fehler) [3]=spikeCount
//           [4]=lastFindingId [5]=medianFtX100 [6]=avgFpsX10 [7]=cswitchPerSec
//           [8]=selfCpuPermille [9]=errCode [10]=modeEcho [11]=targetPid [12]=sessionSec
//   App:    [16]=appTick [17]=wanted [18]=appTargetPid [19]=modeWunsch(bit0=presentOnly, bits4-5=Empfindlichkeit 0/1/2)
static const uint32_t ANALYZE_MAGIC = 0x4C4F5341;   // 'LOSA'
static bool pmIgnore(const std::string& app);       // (unten definiert; Ziel-PID-Wahl nutzt sie)

inline std::wstring analyzeDir() {   // %APPDATA%\lumora\analyze (elevated: KnownFolder, nicht Env)
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) { d = p; CoTaskMemFree(p); }
    d += L"\\lumora\\analyze";
    CreateDirectoryW((d.substr(0, d.find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
inline std::string readSmallFile(const std::wstring& p) {
    FILE* f = nullptr; _wfopen_s(&f, p.c_str(), L"rb"); if (!f) return "";
    std::string s; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f); return s;
}

inline int runAnalyzeBroker() {
    HANDLE shm = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 256, "Local\\LumoraOSDAnalyze");
    if (!shm) return 1;
    uint32_t* mem = (uint32_t*)MapViewOfFile(shm, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!mem) { CloseHandle(shm); return 1; }
    if (mem[0] == ANALYZE_MAGIC && mem[1] && (uint32_t)(GetTickCount() - mem[1]) < 2500) { UnmapViewOfFile(mem); CloseHandle(shm); return 0; }   // Singleton
    mem[0] = ANALYZE_MAGIC; mem[1] = GetTickCount(); mem[2] = 0; mem[9] = 0;

    std::wstring dir = analyzeDir();
    std::wstring curDir = dir + L"\\current";
    CreateDirectoryW(curDir.c_str(), nullptr);
    DeleteFileW((curDir + L"\\findings.jsonl").c_str());
    bool presentOnly = (mem[19] & 1) != 0;   // Selbsttest-Modus A: ohne Kernel-Provider

    // --- NVML lokal laden (eigener Prozess; Klon des Shell-Musters main.cpp L2753) ---
    struct { void* dev = nullptr; char name[96] = {}; char drvVer[64] = {};
             int (*clock)(void*, int, unsigned int*) = nullptr;
             int (*temp)(void*, int, unsigned int*) = nullptr;
             int (*power)(void*, unsigned int*) = nullptr;
             int (*memInfo)(void*, void*) = nullptr;
             int (*throttle)(void*, unsigned long long*) = nullptr;
             int (*util)(void*, void*) = nullptr; } nv;
    {
        HMODULE m = LoadLibraryW(L"nvml.dll");
        if (!m) m = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        if (m) {
            auto init = (int(*)())GetProcAddress(m, "nvmlInit_v2");
            auto byIdx = (int(*)(unsigned int, void**))GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
            auto gname = (int(*)(void*, char*, unsigned int))GetProcAddress(m, "nvmlDeviceGetName");
            auto sysDrv = (int(*)(char*, unsigned int))GetProcAddress(m, "nvmlSystemGetDriverVersion");
            nv.clock = (int(*)(void*, int, unsigned int*))GetProcAddress(m, "nvmlDeviceGetClockInfo");
            nv.temp = (int(*)(void*, int, unsigned int*))GetProcAddress(m, "nvmlDeviceGetTemperature");
            nv.power = (int(*)(void*, unsigned int*))GetProcAddress(m, "nvmlDeviceGetPowerUsage");
            nv.memInfo = (int(*)(void*, void*))GetProcAddress(m, "nvmlDeviceGetMemoryInfo");
            nv.throttle = (int(*)(void*, unsigned long long*))GetProcAddress(m, "nvmlDeviceGetCurrentClocksThrottleReasons");
            nv.util = (int(*)(void*, void*))GetProcAddress(m, "nvmlDeviceGetUtilizationRates");   // GPU-Auslastung (Flaschenhals-Erkennung)
            if (init && byIdx && init() == 0) {
                byIdx(0, &nv.dev);
                if (nv.dev && gname) gname(nv.dev, nv.name, sizeof(nv.name));
                if (sysDrv) sysDrv(nv.drvVer, sizeof(nv.drvVer));
            }
        }
        // AMD/Intel: keine NVML -> Sensor-Ring bleibt duenn (nur RAM); die Analyse
        // degradiert sauber (GPU-Verdaechtige entfallen, Rest bleibt voll nutzbar).
    }

    // --- Analyzer + Quellen ---
    // HEAP statt Stapel: die festen Ringe machen den Analyzer ~650KB gross - auf dem
    // 1-MB-Standardstapel blieb weniger Luft als noetig (s. selfCheck-Befund 0xC00000FD).
    auto anaPtr = std::make_unique<luana::StutterAnalyzer>();
    luana::StutterAnalyzer& ana = *anaPtr;
    std::atomic<uint32_t> spikeCount{ 0 }, lastFindingId{ 0 };
    FILE* jsonl = nullptr;
    _wfopen_s(&jsonl, (curDir + L"\\findings.jsonl").c_str(), L"ab");
    luetw::KernelTrace kt;
    luana::StutterAnalyzer::Config cfg;
    cfg.targetPid = mem[18];   // 0 = automatisch waehlen
    cfg.pidName = [](uint32_t pid) { return luetw::pidExeName(pid); };
    cfg.driverName = [&kt](int idx) { return kt.drivers().name(idx); };
    cfg.onFinding = [&](const luana::Finding& f) {
        if (jsonl) { std::string line = luana::findingJson(f) + "\n"; fwrite(line.data(), 1, line.size(), jsonl); fflush(jsonl); }
        spikeCount = (uint32_t)f.id; lastFindingId = f.id;
    };
    ana.start(cfg);

    // Present-Session (eigener Name, parallel zum FPS-Broker moeglich) + Ziel-Wahl:
    // aktivster Praesentierer der letzten Sekunde (ohne Shell/DWM & Co.), sofern die
    // App keinen festen Ziel-PID vorgibt.
    std::mutex electMx; std::map<uint32_t, uint32_t> presentsByPid;
    luetw::PresentTrace pres(L"LumoraAnalyzePresent");
    bool presOk = pres.start([&](uint32_t pid, double t) {
        ana.pushFrame(pid, t);
        if (!pid) return;
        std::lock_guard<std::mutex> lk(electMx);
        presentsByPid[pid]++;
    });
    if (!presOk) { mem[2] = 2; mem[9] = 100; UnmapViewOfFile(mem); CloseHandle(shm); if (jsonl) fclose(jsonl); return 1; }

    std::atomic<uint32_t> cswPerSecCnt{ 0 };
    bool kernelOk = false;
    if (!presentOnly) {
        luetw::KernelTrace::Sinks sinks;
        sinks.dpcIsr = [&](double t, double durUs, int drv, bool isr) { ana.pushDpcIsr(t, durUs, drv, isr); };
        sinks.cswitch = [&](double t, uint32_t cpu, uint32_t oldTid, uint32_t newTid) {
            cswPerSecCnt.fetch_add(1, std::memory_order_relaxed);
            ana.onCSwitch(t, cpu, oldTid, newTid, kt.pidOfTid(newTid));
        };
        sinks.diskIo = [&](double t, uint32_t bytes, double latMs) { ana.pushDisk(t, bytes, latMs); };
        sinks.proc = [&](double t, uint32_t pid, bool start) { ana.pushProc(t, pid, start); };
        kernelOk = kt.start(std::move(sinks), pres.t0());
        if (!kernelOk) { mem[9] = kt.lastError(); }   // nicht fatal: Etappe-1-Analyse laeuft weiter
    }

    // Sensor-Probe-Thread (10 Hz): NVML + RAM in den Sensor-Ring
    std::atomic<bool> run{ true };
    std::thread sensorThr([&]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        LARGE_INTEGER qpf, qpc; QueryPerformanceFrequency(&qpf);
        // PDH-3D-Engine als herstellerunabhaengige GPU-Auslastung (AMD/Intel, und
        // NVIDIA-Fallback falls NVML fehlt) - gleiche Zaehler wie das Gaming-OSD.
        PDH_HQUERY pq = nullptr; PDH_HCOUNTER pc = nullptr;
        if (!nv.util) {
            if (PdhOpenQueryW(nullptr, 0, &pq) == ERROR_SUCCESS)
                if (PdhAddEnglishCounterW(pq, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &pc) != ERROR_SUCCESS) pc = nullptr;
            if (pq) PdhCollectQueryData(pq);   // Basis-Sample
        }
        while (run.load()) {
            luana::SensorSample s{};
            QueryPerformanceCounter(&qpc);
            int64_t base = pres.t0() ? pres.t0() : qpc.QuadPart;
            s.t = (double)(qpc.QuadPart - base) / (double)qpf.QuadPart;
            if (nv.dev) {
                unsigned int v = 0;
                if (nv.clock && nv.clock(nv.dev, 0, &v) == 0) s.gpuClockMHz = (int)v;
                if (nv.temp && nv.temp(nv.dev, 0, &v) == 0) s.gpuTempC = (int)v;
                if (nv.power && nv.power(nv.dev, &v) == 0) s.gpuPowerW = v / 1000.0f;
                struct { unsigned long long total, freeB, used; } mi{};
                if (nv.memInfo && nv.memInfo(nv.dev, &mi) == 0) {
                    s.vramMB = (int64_t)(mi.used / 1048576);
                    s.vramTotalMB = (int64_t)(mi.total / 1048576);   // fuer die VRAM-Limit-Erkennung
                }
                unsigned long long tr = 0;
                if (nv.throttle && nv.throttle(nv.dev, &tr) == 0) { s.throttleMask = tr; s.throttleKnown = 1; }
                struct { unsigned int gpu, mem; } ut{};
                if (nv.util && nv.util(nv.dev, &ut) == 0) s.gpuLoadPct = (int)(ut.gpu > 100 ? 100 : ut.gpu);
            }
            if (s.gpuLoadPct < 0 && pc) {   // Fallback: Summe aller 3D-Engine-Instanzen (AMD/Intel)
                PdhCollectQueryData(pq);
                DWORD bs = 0, cnt = 0;
                PdhGetFormattedCounterArrayW(pc, PDH_FMT_DOUBLE, &bs, &cnt, nullptr);
                if (bs) {
                    std::vector<uint8_t> buf(bs);
                    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
                    if (PdhGetFormattedCounterArrayW(pc, PDH_FMT_DOUBLE, &bs, &cnt, items) == ERROR_SUCCESS) {
                        double sum = 0; for (DWORD i = 0; i < cnt; ++i) sum += items[i].FmtValue.doubleValue;
                        s.gpuLoadPct = (int)(sum > 100 ? 100 : sum + 0.5);
                    }
                }
            }
            MEMORYSTATUSEX ms{ sizeof(ms) };
            if (GlobalMemoryStatusEx(&ms)) s.ramMB = (int64_t)((ms.ullTotalPhys - ms.ullAvailPhys) / 1048576);
            ana.pushSensor(s);
            Sleep(100);
        }
    });

    // --- Hauptschleife: SHM-Status, Ziel-Wahl, Selbst-CPU-Watchdog, Ende-Erkennung ---
    uint32_t startTick = GetTickCount(), lastSec = GetTickCount();
    FILETIME cA, cB, kA, uA, kB, uB; GetProcessTimes(GetCurrentProcess(), &cA, &cB, &kA, &uA);
    uint64_t lastCpu100 = ((uint64_t)kA.dwHighDateTime << 32 | kA.dwLowDateTime) + ((uint64_t)uA.dwHighDateTime << 32 | uA.dwLowDateTime);
    bool cswDropped = false;
    for (;;) {
        Sleep(100);
        uint32_t now = GetTickCount();
        // Ziel-PID-Wahl 1x/s (falls App keinen vorgibt)
        if (now - lastSec >= 1000) {
            std::map<uint32_t, uint32_t> counts;
            { std::lock_guard<std::mutex> lk(electMx); counts.swap(presentsByPid); }
            if (!mem[18]) {
                uint32_t best = 0, bestN = 0;
                for (auto& [pid, n] : counts)
                    if (n > bestN && !pmIgnore(luetw::pidExeName(pid))) { best = pid; bestN = n; }
                if (best && bestN >= 20 && best != ana.targetPid()) ana.setTargetPid(best);   // >=20 fps: echtes Spiel/Anwendung
            } else if (ana.targetPid() != mem[18]) ana.setTargetPid(mem[18]);
            // Selbst-CPU (Permille eines Kerns) + CSwitch-Rate + Watchdog
            GetProcessTimes(GetCurrentProcess(), &cA, &cB, &kB, &uB);
            uint64_t cpu100 = ((uint64_t)kB.dwHighDateTime << 32 | kB.dwLowDateTime) + ((uint64_t)uB.dwHighDateTime << 32 | uB.dwLowDateTime);
            uint32_t permille = (uint32_t)((cpu100 - lastCpu100) / 10000);   // 100ns-Einheiten -> Promille bei 1s Wand-Zeit
            lastCpu100 = cpu100;
            mem[8] = permille;
            mem[7] = cswPerSecCnt.exchange(0);
            ana.sampleLimit();   // Flaschenhals-Klassifikation des letzten Sekundenfensters
            mem[13] = (uint32_t)ana.lastLimit();
            if (!cswDropped && permille > 10 && kernelOk) { kt.dropCSwitch(); cswDropped = true; mem[9] = 200; }   // Notbremse (>1% eines Kerns)
            lastSec = now;
        }
        // Empfindlichkeit live aus dem App-Wunsch uebernehmen (UI-Regler wirkt sofort)
        { static int lastSense = -1; int sense = (mem[19] >> 4) & 3;
          if (sense != lastSense) { lastSense = sense; ana.setSensitivity(sense); } }
        // SHM-Status. live() statt stats(): stats() kopiert die WACHSENDEN Report-Vektoren,
        // waehrend der Present-Thread per push_back realloziert - Use-after-free, exakt die
        // Klasse des behobenen recentFt-Absturzes (c2758b6). live() liest nur den festen Ring.
        auto lv = ana.live();
        mem[1] = now;
        mem[2] = 1;
        mem[3] = spikeCount.load(); mem[4] = lastFindingId.load();
        mem[5] = (uint32_t)(lv.medianFtMs * 100); mem[6] = (uint32_t)(lv.avgFps * 10);
        mem[10] = presentOnly ? 1 : 0; mem[11] = ana.targetPid(); mem[12] = (now - startTick) / 1000;
        {   // Live-Frametimes fuers Analyse-OSD (echter Schrieb statt 1-Hz-Median):
            // mem[20] = Frame-Zaehler, mem[21..60] = letzte 40 Frametimes x100 (aeltester zuerst)
            float fts[40]; uint32_t n = ana.recentFt(fts, 40);
            for (uint32_t i = 0; i < 40; ++i) mem[21 + i] = i < n ? (uint32_t)(fts[i] * 100.0f + 0.5f) : 0;
            mem[20] = (uint32_t)ana.frameCount();
        }
        // Ende: App will stoppen (wanted=0) oder App-Heartbeat weg (>5s, nach 8s Anlauf)
        bool appAlive = (mem[16] && (uint32_t)(now - mem[16]) < 5000) || (uint32_t)(now - startTick) < 8000;
        if (!mem[17] || !appAlive) break;
    }

    // --- Finalisieren: Quellen stoppen, Bericht schreiben ---
    run = false;
    pres.stop(); if (kernelOk) kt.stop(); ana.stop();
    if (sensorThr.joinable()) sensorThr.join();
    if (jsonl) fclose(jsonl);

    auto st = ana.stats();
    double durS = (GetTickCount() - startTick) / 1000.0;
    // Aggregat: Verdaechtige nach (kind,name) buendeln
    struct Agg { std::string kind, name; uint32_t hits = 0; };
    std::vector<Agg> agg;
    for (auto& f : ana.findings()) {
        // je Befund nur den TOP-Verdaechtigen zaehlen (sonst dominieren Beifaenge)
        if (f.suspects.empty()) continue;
        const auto& s = f.suspects[0];
        bool found = false;
        for (auto& a : agg) if (a.kind == s.kind && a.name == s.name) { a.hits++; found = true; break; }
        if (!found) agg.push_back({ s.kind, s.name, 1 });
    }
    std::sort(agg.begin(), agg.end(), [](const Agg& a, const Agg& b) { return a.hits > b.hits; });
    std::string verdictKey = "clean", verdictName; uint32_t verdictHits = 0;
    if (!ana.findings().empty() && !agg.empty()) { verdictKey = agg[0].kind; verdictName = agg[0].name; verdictHits = agg[0].hits; }

    // Bericht zusammensetzen (manuelles JSON, BOM-frei; Kontext der Shell einbetten)
    std::string ctx = readSmallFile(curDir + L"\\context.json");
    if (ctx.empty() || ctx[0] != '{') ctx = "{}";
    SYSTEMTIME lt; GetLocalTime(&lt);
    char ts[32]; sprintf_s(ts, "%04d%02d%02d-%02d%02d%02d", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
    char head[1024];
    sprintf_s(head,
        "{\"version\":2,\"wall\":\"%04d-%02d-%02dT%02d:%02d:%02d\",\"durS\":%.0f,\"pid\":%u,"
        "\"frames\":%llu,\"avgFps\":%.1f,\"p1LowFps\":%.1f,\"medianFtMs\":%.2f,\"p99FtMs\":%.2f,"
        "\"spikes\":%u,\"spikesPerMin\":%.2f,\"presentOnly\":%s,\"kernelOk\":%s,\"errCode\":%u,"
        "\"verdictKey\":\"%s\",\"verdictHits\":%u,",
        lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, durS, ana.targetPid(),
        (unsigned long long)st.frames, st.avgFps, st.p1LowFps, st.medianFtMs, st.p99FtMs,
        (uint32_t)ana.findings().size(), durS > 0 ? ana.findings().size() * 60.0 / durS : 0,
        presentOnly ? "true" : "false", kernelOk ? "true" : "false", mem[9],
        verdictKey.c_str(), verdictHits);
    std::string rep = head;
    rep += "\"verdictName\":\"" + luana::jsonEsc(verdictName) + "\",";
    rep += "\"game\":\"" + luana::jsonEsc(luetw::pidExeName(ana.targetPid())) + "\",";
    rep += "\"gpu\":\"" + luana::jsonEsc(nv.name) + "\",\"gpuDriver\":\"" + luana::jsonEsc(nv.drvVer) + "\",";
    rep += "\"context\":" + ctx + ",\"note\":\"\",";
    rep += "\"ftSeries\":[";
    { std::vector<std::pair<double, float>> ser; ana.ftSeries(2000, ser);
      char b2[48];
      for (size_t i = 0; i < ser.size(); ++i) { sprintf_s(b2, "%s[%.2f,%.2f]", i ? "," : "", ser[i].first, ser[i].second); rep += b2; } }
    rep += "],\"findings\":[";
    { const auto& fs = ana.findings();
      for (size_t i = 0; i < fs.size(); ++i) { if (i) rep += ","; rep += luana::findingJson(fs[i]); } }
    rep += "],\"aggregate\":[";
    for (size_t i = 0; i < agg.size() && i < 8; ++i) {
        if (i) rep += ",";
        rep += "{\"kind\":\"" + luana::jsonEsc(agg[i].kind) + "\",\"name\":\"" + luana::jsonEsc(agg[i].name)
             + "\",\"hits\":" + std::to_string(agg[i].hits) + "}";
    }
    rep += "],\"limit\":{";
    {   // Flaschenhals-Verteilung ueber die Session (Sekundenproben) + dominante Ursache
        const luana::Limit all[] = { luana::Limit::Gpu, luana::Limit::Cpu, luana::Limit::CpuCore,
                                     luana::Limit::Framecap, luana::Limit::Throttle, luana::Limit::Vram,
                                     luana::Limit::Unknown };
        uint32_t total = ana.limitTotal(); bool first = true;
        std::string top = "unknown"; uint32_t topN = 0;
        for (auto l : all) {
            uint32_t n = ana.limitCount(l);
            if (!first) rep += ","; first = false;
            rep += "\"" + std::string(luana::limitKey(l)) + "\":" + std::to_string(total ? n * 100 / total : 0);
            // "unknown" nie als Hauptaussage kueren - sonst stuende im Bericht eine Nicht-Aussage
            if (l != luana::Limit::Unknown && n > topN) { topN = n; top = luana::limitKey(l); }
        }
        rep += ",\"samples\":" + std::to_string(total) + ",\"top\":\"" + top + "\"";
        rep += ",\"topPct\":" + std::to_string(total ? topN * 100 / total : 0);
    }
    rep += "}";
    // ETW-Verluste als Ehrlichkeits-Kennzahl in den Bericht: verworfene Events/Puffer
    // weichen jede Aussage auf - ein Bericht ohne diese Zahl saehe immer "sauber" aus.
    rep += ",\"eventsLost\":" + std::to_string(kernelOk ? kt.eventsLost() : 0)
         + ",\"buffersLost\":" + std::to_string(kernelOk ? kt.buffersLost() : 0);
    // Datenmodell v2 (ANALYSE-WERKBANK-PLAN.md): volle ft-Serie + Session-Spuren +
    // Namens-Tabellen fuer die Werkbank-Timeline. ftSeries oben bleibt absichtlich
    // bestehen - Reiter-Bericht und Website lesen weiter das v1-Subset.
    rep += "," + ana.v2Json();
    rep += "}";
    // Bericht ATOMAR schreiben (erst .neu, dann drueberschieben) und die Rohdaten NUR
    // bei Erfolg loeschen. Vorher: direktes wb + bedingungsloses DeleteFile - schlug der
    // Schreibvorgang fehl (Platte voll, Virenscanner), waren Bericht UND findings.jsonl
    // (die Blackbox!) zugleich verloren. Gleiche Fehlerklasse wie writeFile in main.cpp.
    bool repOk = false;
    {
        std::wstring repPath = dir + L"\\report-" + std::wstring(ts, ts + strlen(ts)) + L".json";
        std::wstring tmpPath = repPath + L".neu";
        FILE* f = nullptr; _wfopen_s(&f, tmpPath.c_str(), L"wb");
        if (f) {
            repOk = fwrite(rep.data(), 1, rep.size(), f) == rep.size();
            fclose(f);
            if (repOk) repOk = MoveFileExW(tmpPath.c_str(), repPath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            if (!repOk) DeleteFileW(tmpPath.c_str());
        }
    }
    if (repOk) {
        DeleteFileW((curDir + L"\\findings.jsonl").c_str());
        DeleteFileW((curDir + L"\\context.json").c_str());
    } else mem[9] = 210;   // Bericht nicht geschrieben - findings.jsonl bleibt als Blackbox liegen
    mem[2] = 0; mem[1] = GetTickCount();
    UnmapViewOfFile(mem); CloseHandle(shm);
    return 0;
}

// --- Ruckler-Blackbox: Diagnose-/Prototypmodus ------------------------------------
// "lumora-shell.exe --analyze-dump" (elevated Konsole): 5 s Kernel-ETW mitschneiden,
// Aggregate (DPC je Treiber, CSwitch-/Disk-Raten, Top-CPU-Prozesse) auf Konsole UND
// nach %TEMP%\lumora-analyze-dump.txt schreiben. Beweist die komplette Kette
// (System-Logger-Session, MOF-Parsing, Treibernamen, TID->PID) VOR dem Feature-Bau.
inline int runAnalyzeDump() {
    // GUI-Subsystem: an die aufrufende Konsole anhaengen, sonst verpufft printf
    if (AttachConsole(ATTACH_PARENT_PROCESS)) { FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); }
    auto out = [](const std::string& s) {
        printf("%s\n", s.c_str());
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        FILE* f = nullptr; _wfopen_s(&f, (std::wstring(tmp) + L"\\lumora-analyze-dump.txt").c_str(), L"ab");
        if (f) { fwrite(s.data(), 1, s.size(), f); fwrite("\n", 1, 1, f); fclose(f); }
    };
    {   // alte Dump-Datei leeren
        wchar_t tmp[MAX_PATH] = {}; GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
        DeleteFileW((std::wstring(tmp) + L"\\lumora-analyze-dump.txt").c_str());
    }
    {   // Analyzer-Selbstcheck mit synthetischen Daten (braucht KEINE Elevation)
        std::string msg;
        bool ok = luana::StutterAnalyzer::selfCheck(msg);
        out((ok ? "OK  " : "FEHLER  ") + msg);
    }
    // Aggregate (nur dieser Thread schreibt via Callbacks, Hauptthread liest NACH stop())
    struct DrvAgg { uint64_t count = 0; double sumUs = 0, maxUs = 0; };
    std::map<int, DrvAgg> dpcByDrv, isrByDrv;
    double durMin = 1e18, durMax = -1e18; uint64_t durNeg = 0;   // Zeitbasis-Diagnose InitialTime
    uint64_t csw = 0, disk = 0; double diskBytes = 0;
    std::map<uint32_t, double> cpuByPid;               // pid -> geschaetzte Laufzeit (s)
    struct CoreRun { uint32_t tid = 0; double since = 0; } cores[128];

    std::map<std::string, uint64_t> guidStats;
    luetw::KernelTrace kt;
    kt.setGuidStats(&guidStats);
    luetw::KernelTrace::Sinks sinks;
    sinks.dpcIsr = [&](double, double durUs, int drv, bool isr) {
        if (durUs < durMin) durMin = durUs; if (durUs > durMax) durMax = durUs; if (durUs < 0) ++durNeg;
        auto& a = (isr ? isrByDrv : dpcByDrv)[drv];
        a.count++; a.sumUs += durUs; if (durUs > a.maxUs) a.maxUs = durUs;
    };
    sinks.cswitch = [&](double t, uint32_t cpu, uint32_t, uint32_t newTid) {
        ++csw;
        if (cpu < 128) {
            auto& c = cores[cpu];
            if (c.tid) { uint32_t pid = kt.pidOfTid(c.tid); if (pid) cpuByPid[pid] += t - c.since; }
            c.tid = newTid; c.since = t;
        }
    };
    sinks.diskIo = [&](double, uint32_t bytes, double) { ++disk; diskBytes += bytes; };
    sinks.proc = [&](double, uint32_t, bool) {};
    if (!kt.start(std::move(sinks))) {
        out("FEHLER: KernelTrace.start fehlgeschlagen, Code " + std::to_string(kt.lastError()) +
            (kt.lastError() == 5 ? " (Zugriff verweigert - elevated Konsole noetig!)" : ""));
        return 1;
    }
    out("Kernel-ETW-Session laeuft (System-Logger). Sammle 5 Sekunden...");
    if (kt.enableIntrError()) out("HINWEIS: EnableTraceEx2(System-Interrupt) Code " + std::to_string(kt.enableIntrError()));
    Sleep(5000);
    kt.stop();

    char b[256];
    out("--- Event-Mix (GUID/Opcode, Top 20) ---");
    { std::vector<std::pair<uint64_t, std::string>> gs;
      for (auto& [k, n] : guidStats) gs.push_back({ n, k });
      std::sort(gs.rbegin(), gs.rend());
      for (size_t i = 0; i < gs.size() && i < 20; ++i) { sprintf_s(b, "  %-52s n=%llu", gs[i].second.c_str(), (unsigned long long)gs[i].first); out(b); } }
    sprintf_s(b, "Events gesamt: %llu | Treiber in Tabelle: %zu", (unsigned long long)kt.eventCount(), kt.drivers().size()); out(b);
    sprintf_s(b, "CSwitch: %llu (%.0f/s) | Disk-IOs: %llu (%.1f MB)", (unsigned long long)csw, csw / 5.0, (unsigned long long)disk, diskBytes / 1048576.0); out(b);
    sprintf_s(b, "InitialTime-Diagnose: durUs min=%.1f max=%.1f negativ=%llu (QPF=%lld)", durMin, durMax, (unsigned long long)durNeg, (long long)kt.qpf()); out(b);
    out("--- DPC je Treiber (Top 10 nach Summe) ---");
    std::vector<std::pair<double, int>> top;
    for (auto& [drv, a] : dpcByDrv) top.push_back({ a.sumUs, drv });
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 10; ++i) {
        auto& a = dpcByDrv[top[i].second];
        sprintf_s(b, "  %-24s n=%-7llu sum=%9.0fus max=%7.1fus", kt.drivers().name(top[i].second).c_str(),
                  (unsigned long long)a.count, a.sumUs, a.maxUs); out(b);
    }
    out("--- ISR je Treiber (Top 5) ---");
    top.clear(); for (auto& [drv, a] : isrByDrv) top.push_back({ a.sumUs, drv });
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 5; ++i) {
        auto& a = isrByDrv[top[i].second];
        sprintf_s(b, "  %-24s n=%-7llu sum=%9.0fus max=%7.1fus", kt.drivers().name(top[i].second).c_str(),
                  (unsigned long long)a.count, a.sumUs, a.maxUs); out(b);
    }
    out("--- CPU-Zeit je Prozess aus CSwitch (Top 10) ---");
    std::vector<std::pair<double, uint32_t>> topCpu;
    for (auto& [pid, s] : cpuByPid) topCpu.push_back({ s, pid });
    std::sort(topCpu.rbegin(), topCpu.rend());
    for (size_t i = 0; i < topCpu.size() && i < 10; ++i) {
        sprintf_s(b, "  pid %-7u %-28s %6.2fs", topCpu[i].second,
                  luetw::pidExeName(topCpu[i].second).c_str(), topCpu[i].first); out(b);
    }
    out("FERTIG.");
    return 0;
}

static const uint32_t FPS_MAGIC = 0x4C4F5344;   // 'LOSD'
// PresentMon-Prozesse, die kein Spiel sind (1:1 aus main.js PM_IGNORE)
static bool pmIgnore(const std::string& app) {
    static const std::set<std::string> ig = { "dwm.exe","explorer.exe","lumora.exe","lumora_shell.exe","presentmon.exe",
        "searchhost.exe","textinputhost.exe","shellexperiencehost.exe","startmenuexperiencehost.exe","applicationframehost.exe",
        // WebView2-Renderer: praesentiert fuer UNSERE Overlays (Analyse-/Gaming-OSD) - ohne
        // diesen Eintrag koennte die Ziel-PID-Wahl der Ruckler-Blackbox das eigene OSD
        // als "Spiel" waehlen (Selbstmessungs-Falle).
        "msedgewebview2.exe" };
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

    // Netzwerk-Statistik (zuschaltbar): App schreibt die Ziel-PID des laufenden Spiels
    // nach [8]; solange sie != 0 ist, laeuft eine ZWEITE, reine Netz-ETW-Session und
    // der Broker meldet die Bytes der PID-Familie (Spiel + Kinder) nach [9..12].
    // [13] = bestaetigte Ziel-PID (0 = Zaehlung aus/Trace-Start fehlgeschlagen).
    luetw::NetTrace net;
    bool netAn = false;
    uint32_t netPid = 0, netFamTick = 0, diagTick = 0;
    int diagZeilen = 0;
    std::set<uint32_t> netFam;

    // 60-Hz-Schreibschleife: aktivsten Praesentierer der letzten 0,5 s -> FPS ins SHM.
    uint32_t startTick = GetTickCount(), lastFreshApp = GetTickCount();
    for (;;) {
        Sleep(16);
        uint32_t now = GetTickCount();
        uint32_t appTick = mem[6];   // App-Heartbeat @24
        if (appTick && (uint32_t)(now - appTick) < 3000) lastFreshApp = now;
        if ((uint32_t)(now - startTick) > 8000 && (uint32_t)(now - lastFreshApp) > 5000) break;   // App zu / OSD aus
        double tmax = 0; int outFps = 0, outFtX100 = 0; uint32_t bestPid = 0;
        // Vordergrundfenster bevorzugen (wie RTSS' dwLastForegroundApp): sonst gewinnt
        // "wer praesentiert am meisten" - ein staerker praesentierender Hintergrundprozess
        // (Overlay/Browser/Launcher) schlaegt dann ein absichtlich niedriger limitiertes
        // Spiel, und dessen (ggf. an die Desktop-Hz gekoppelte) Rate landet im OSD statt
        // der echten Spiel-Framerate (real beobachtet: Spiel auf 120 fps begrenzt, OSD
        // zeigte die Monitor-Aktualisierungsrate).
        DWORD fgPid = 0; { HWND fg = GetForegroundWindow(); if (fg) GetWindowThreadProcessId(fg, &fgPid); }
        std::string fgKey = fgPid ? std::to_string(fgPid) : std::string();
        {
            std::lock_guard<std::mutex> lk(mx);
            for (auto& [pid, e] : frames) if (!e.times.empty()) tmax = (std::max)(tmax, e.times.back());
            auto countRecent = [&](const Frames& e) { int c = 0; for (int i = (int)e.times.size() - 1; i >= 0 && e.times[i] >= tmax - 0.5; --i) c++; return c; };
            int bestC = 0;
            auto fgIt = fgKey.empty() ? frames.end() : frames.find(fgKey);
            if (fgIt != frames.end() && (bestC = countRecent(fgIt->second)) >= 2) {
                outFps = (int)(bestC / 0.5 + 0.5); outFtX100 = (int)(fgIt->second.ft.back() * 100 + 0.5); bestPid = fgPid;
            } else {
                bestC = 0;
                for (auto& [pid, e] : frames) {
                    int c = countRecent(e);
                    if (c > bestC) { bestC = c; outFps = (int)(c / 0.5 + 0.5); outFtX100 = (int)(e.ft.back() * 100 + 0.5); bestPid = (uint32_t)atoi(pid.c_str()); }
                }
                if (bestC < 2) { outFps = 0; outFtX100 = 0; }
            }
        }
        mem[0] = FPS_MAGIC; mem[1] = now; mem[2] = (uint32_t)outFps; mem[3] = (uint32_t)outFtX100; mem[4] = 0; mem[5] = bestPid;

        // --- Netzwerk-Statistik ---
        uint32_t wollen = mem[8];
        if (wollen && !netAn) { netAn = net.start(); netPid = 0; }
        if (!wollen && netAn) { net.stop(); netAn = false; netPid = 0; netFam.clear(); mem[13] = 0; mem[9] = mem[10] = mem[11] = mem[12] = 0; }
        if (netAn && wollen) {
            if (wollen != netPid) {   // neue Spielsitzung: ab hier frisch zaehlen
                netPid = wollen; net.setTarget(netPid); netFamTick = 0; diagZeilen = 0;
            }
            // Kinder, die es schon VOR dem Trace-Start gab, per Schnappschuss nachreichen
            // (ab dann uebernimmt der Prozess-Start-Event-Pfad im Trace - lueckenlos).
            if ((uint32_t)(now - netFamTick) >= 1000) {
                netFamTick = now;
                netFam = net.family();
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap != INVALID_HANDLE_VALUE) {
                    PROCESSENTRY32W pe{ sizeof(pe) };
                    // Mehrere Durchgaenge, bis nichts Neues kommt: Kind-von-Kind-Ketten
                    // (Launcher -> Wrapper -> Spiel) brauchen mehr als einen Blick.
                    bool neu = true;
                    for (int runde = 0; runde < 4 && neu; ++runde) {
                        neu = false;
                        if (Process32FirstW(snap, &pe)) do {
                            if (netFam.count(pe.th32ParentProcessID) && !netFam.count(pe.th32ProcessID)) {
                                netFam.insert(pe.th32ProcessID); net.addFamily(pe.th32ProcessID); neu = true;
                            }
                        } while (Process32NextW(snap, &pe));
                    }
                    CloseHandle(snap);
                }
            }
            uint64_t in = 0, out = 0; net.bytesFam(in, out);
            mem[9] = (uint32_t)(in & 0xFFFFFFFF); mem[10] = (uint32_t)(in >> 32);
            mem[11] = (uint32_t)(out & 0xFFFFFFFF); mem[12] = (uint32_t)(out >> 32);
            mem[13] = netPid;
            // Diagnose-Datei (5-s-Takt): welche Opcodes kommen an, wohin buchen die
            // Bytes, wer ist in der PID-Familie? Die Antwort auf "es zaehlt fast nichts".
            if ((uint32_t)(now - diagTick) >= 5000 && diagZeilen < 12) {   // begrenzt: Diagnose, kein Dauerlog
                diagTick = now; diagZeilen++;
                PWSTR ad = nullptr; std::wstring dp;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &ad)) && ad) { dp = ad; CoTaskMemFree(ad); }
                dp += L"\\lumora\\net-diagnose.txt";
                FILE* f = nullptr; _wfopen_s(&f, dp.c_str(), L"a");
                if (f) {
                    std::string fam; for (uint32_t p2 : netFam) { if (!fam.empty()) fam += ","; fam += std::to_string(p2); }
                    std::string line = "tick=" + std::to_string(now) + " ziel=" + std::to_string(netPid)
                        + " familie=[" + fam + "] summe(in=" + std::to_string(in) + ",out=" + std::to_string(out) + ") "
                        + net.diag() + "\n";
                    fwrite(line.data(), 1, line.size(), f); fclose(f);
                }
            }
        } else if (wollen && !netAn) {
            mem[13] = 0;   // Trace-Start fehlgeschlagen -> App sieht: Zaehlung laeuft NICHT
        }
    }
    if (netAn) net.stop();
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
