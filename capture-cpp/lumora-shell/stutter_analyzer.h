// Analyse-Kern der Ruckler-Blackbox (Design: capture-cpp/BOTTLENECK-PLAN.md).
// Sammelt alle Signalquellen in vorallokierten Ringpuffern (~10 s Historie), erkennt
// Frame-Time-Spitzen (Spike = ft > max(k*Median64, Median+8ms)) und friert beim Spike
// das +-200-ms-Fenster ein: Verdaechtigen-Analyse gegen die 2-s-Baseline davor
// (Prozess-CPU-Delta, Treiber-DPC/ISR, GPU-Throttle/Takt, VRAM, Disk, Prozessstart).
//
// THREADING/OVERHEAD (harte <1%-Bedingung, s. Plan):
// - Jeder Ring hat genau EINEN Schreiber (Present-Thread bzw. Kernel-ETW-Thread bzw.
//   Sensor-Thread). Schreiben = POD-Zuweisung + Index-Inkrement, 0 Allokationen.
// - Der Analyse-Worker (BELOW_NORMAL) liest die Ringe OHNE Lock: bewusst tolerierte
//   Races treffen nur die aeltesten, gerade ueberschriebenen Eintraege - fuer eine
//   statistische Verdaechtigen-Analyse harmlos, und der heisse Pfad bleibt lockfrei.
// - CSwitch wird NUR aggregiert (50-ms-CpuBuckets, fester Open-Addressing-Hash),
//   nie einzeln gespeichert.
#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace luana {

// ---- Ring-Eintraege (POD, pack fuer kompakte Snapshots) ---------------------------
struct FrameSample  { double t; float ftMs; uint32_t pid; };
struct DpcIsrSample { double t; float durUs; int32_t driverIdx; uint8_t isIsr; };
struct DiskSample   { double t; uint32_t bytes; float latMs; };
struct ProcSample   { double t; uint32_t pid; uint8_t start; };
struct SensorSample {
    double t;
    int32_t gpuClockMHz = -1, gpuTempC = -1;
    float gpuPowerW = -1;
    int64_t vramMB = -1, ramMB = -1;
    uint64_t throttleMask = 0;      // NVML-ClocksThrottleReasons (0 = keiner/unbekannt)
    uint8_t throttleKnown = 0;      // 0 = Maske nicht auslesbar (AMD/alt) -> Takt-Heuristik
};
// CSwitch-Aggregat: ein Bucket je 50 ms. topPid/topUs = die 8 groessten CPU-Verbraucher.
struct CpuBucket {
    double t = 0;   // Bucket-Beginn (Feldname "t" wie in allen Ringen - Ring::range verlangt das)
    uint32_t topPid[8] = {}; uint32_t topUs[8] = {};
    uint32_t totalUs = 0;           // Summe aller Nicht-Idle-Laufzeit (alle Kerne)
    uint16_t coreBusyPct[64] = {};  // je Kern 0..100 (fuer Single-Core-Limit-Anzeige)
};

// Fester Ring: ein Schreiber, Index atomar, Leser kopiert rueckwaerts.
template <typename T, uint32_t N>
struct Ring {
    T buf[N] = {};
    std::atomic<uint32_t> idx{ 0 };   // naechste Schreibposition (monoton)
    void push(const T& v) { buf[idx.load(std::memory_order_relaxed) % N] = v; idx.fetch_add(1, std::memory_order_release); }
    // Alle Eintraege mit t in [t0,t1] in out kopieren (Leser-Thread; tolerierte Races s.o.)
    void range(double t0, double t1, std::vector<T>& out) const {
        uint32_t end = idx.load(std::memory_order_acquire);
        uint32_t n = end < N ? end : N;
        for (uint32_t i = 0; i < n; ++i) {
            const T& e = buf[(end - 1 - i) % N];
            if (e.t > t1) continue;
            if (e.t < t0) break;   // rueckwaerts: aelter als t0 -> fertig
            out.push_back(e);
        }
    }
};

// ---- Verdaechtiger + Befund -------------------------------------------------------
struct Suspect {
    std::string kind;       // process | driver | gpu-throttle | vram | disk | proc-start | game-internal
    std::string name;       // exe/Treiber/Leerlauf
    std::string evidence;   // Klartext-Beweisschnipsel ("DPC 812us (Basis 40us)")
    double score = 0;       // 0..1 (Sortierung)
};
struct Finding {
    uint32_t id = 0;
    double t = 0;           // Sekunden auf Session-Basis
    double ftMs = 0, medianMs = 0;
    std::vector<Suspect> suspects;
};

inline std::string jsonEsc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

class StutterAnalyzer {
public:
    struct Config {
        double spikeK = 3.0;            // Spike, wenn ft > max(k*Median, Median+8ms)
        double spikeAbsMs = 8.0;
        double debounceS = 0.25;        // max. 1 Befund je 250 ms
        uint32_t targetPid = 0;         // 0 = aktivster Praesentierer (setzt der Broker)
        std::function<std::string(uint32_t pid)> pidName;    // pidExeName
        std::function<std::string(int idx)> driverName;      // KernelTrace::drivers().name
        std::function<void(const Finding&)> onFinding;       // laeuft auf dem Worker-Thread
    };

    void start(const Config& cfg) {
        cfg_ = cfg;
        stop_ = false;
        worker_ = std::thread([this]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            workerLoop();
        });
    }
    void stop() {
        stop_ = true;
        if (worker_.joinable()) { wake(); worker_.join(); }
    }
    // Ziel-PID nachtraeglich setzen (Broker waehlt den aktivsten Praesentierer erst zur
    // Laufzeit). Benigner u32-Race gegen den Present-Thread - schlimmstenfalls zaehlt
    // ein einzelner Frame in den Wechselmoment.
    void setTargetPid(uint32_t pid) { cfg_.targetPid = pid; }
    uint32_t targetPid() const { return cfg_.targetPid; }
    ~StutterAnalyzer() { stop(); }

    // ---- Feeds (jeweils genau EIN Schreiber-Thread) -------------------------------
    void pushFrame(uint32_t pid, double t) {   // Present-Event -> Frametime des Ziel-PIDs
        if (cfg_.targetPid && pid != cfg_.targetPid) return;
        double last = lastPresent_;
        lastPresent_ = t;
        if (last <= 0) return;
        float ft = (float)((t - last) * 1000.0);
        if (ft <= 0 || ft > 5000) return;
        frames_.push({ t, ft, pid });
        allFtT_.push_back(t); allFt_.push_back(ft);        // Gesamtserie fuer den Report
        ++frameCount_;
        // Spike-Pruefung inline (billig): Median der letzten 64 Frametimes
        double med = median64();
        if (med > 0 && ft > (std::max)(cfg_.spikeK * med, med + cfg_.spikeAbsMs)) {
            if (t - lastSpikeT_ >= cfg_.debounceS) {
                lastSpikeT_ = t;
                pendingSpike_.store(1 + (uint32_t)(t * 1000), std::memory_order_release);
                pendingFt_ = ft; pendingMed_ = med; pendingT_ = t;
                wake();
            }
        }
    }
    void pushDpcIsr(double t, double durUs, int driverIdx, bool isr) { dpcs_.push({ t, (float)durUs, driverIdx, (uint8_t)(isr ? 1 : 0) }); }
    void pushDisk(double t, uint32_t bytes, double latMs) { disks_.push({ t, bytes, (float)latMs }); }
    void pushProc(double t, uint32_t pid, bool start) { procs_.push({ t, pid, (uint8_t)(start ? 1 : 0) }); }
    void pushSensor(const SensorSample& s) { sensors_.push(s); }
    // CSwitch-Aggregation (heissester Pfad! nur Integer, keine Allokation):
    void onCSwitch(double t, uint32_t cpu, uint32_t /*oldTid*/, uint32_t newTid, uint32_t newPid) {
        if (cpu >= 64) return;
        Core& c = cores_[cpu];
        if (c.pid) {   // Laufzeit des bisherigen Prozesses auf diesem Kern verbuchen
            uint32_t us = (uint32_t)((t - c.since) * 1e6);
            if (us > 0 && us < 60000000) { hashAdd(c.pid, us); busyUs_ += us; if (cpu < 64) coreUs_[cpu] += us; }
        }
        c.pid = newPid; c.since = t;
        if (t - bucketT0_ >= 0.05) closeBucket(t);
    }

    // ---- Report-Daten (nach stop() vom Broker gelesen) ----------------------------
    struct Stats { double avgFps = 0, p1LowFps = 0, medianFtMs = 0, p99FtMs = 0; uint64_t frames = 0; };
    Stats stats() const {
        Stats s; s.frames = frameCount_;
        if (allFt_.empty()) return s;
        std::vector<float> v = allFt_;
        std::sort(v.begin(), v.end());
        s.medianFtMs = v[v.size() / 2];
        s.p99FtMs = v[(size_t)((double)(v.size() - 1) * 0.99)];
        double sum = 0; for (float f : v) sum += f;
        s.avgFps = sum > 0 ? 1000.0 * v.size() / sum : 0;
        s.p1LowFps = s.p99FtMs > 0 ? 1000.0 / s.p99FtMs : 0;   // 1%-Low ~ 99. Perzentil der Frametime
        return s;
    }
    // Frametime-Serie auf <=maxPts Punkte eindampfen (je Zelle das MAXIMUM behalten -
    // Spikes muessen die Verdichtung ueberleben, sonst luegt der Berichts-Graph).
    void ftSeries(size_t maxPts, std::vector<std::pair<double, float>>& out) const {
        size_t n = allFt_.size(); if (!n) return;
        size_t cell = (n + maxPts - 1) / maxPts; if (cell < 1) cell = 1;
        for (size_t i = 0; i < n; i += cell) {
            float mx = 0; size_t mi = i;
            for (size_t j = i; j < n && j < i + cell; ++j) if (allFt_[j] > mx) { mx = allFt_[j]; mi = j; }
            out.push_back({ allFtT_[mi], mx });
        }
    }
    const std::vector<Finding>& findings() const { return findings_; }
    uint32_t spikeCount() const { return (uint32_t)findings_.size(); }

    // Selbstcheck mit synthetischen Daten (fuer --analyze-dump): Spike + Taeter muessen erkannt werden.
    static bool selfCheck(std::string& msg);

private:
    struct Core { uint32_t pid = 0; double since = 0; };

    void wake() { SetEvent(evt_); }

    double median64() {
        uint32_t end = frames_.idx.load(std::memory_order_relaxed);
        uint32_t n = end < 64 ? end : 64;
        if (n < 16) return 0;   // erst mit etwas Historie urteilen
        float tmp[64];
        for (uint32_t i = 0; i < n; ++i) tmp[i] = frames_.buf[(end - 1 - i) % 4096].ftMs;
        std::nth_element(tmp, tmp + n / 2, tmp + n);
        return tmp[n / 2];
    }

    // CSwitch-Hilfen: fester Hash pid->us fuer den laufenden 50-ms-Bucket
    void hashAdd(uint32_t pid, uint32_t us) {
        uint32_t h = (pid * 2654435761u) & 511;
        for (uint32_t i = 0; i < 32; ++i) {
            uint32_t s = (h + i) & 511;
            if (hashPid_[s] == pid) { hashUs_[s] += us; return; }
            if (hashPid_[s] == 0) { hashPid_[s] = pid; hashUs_[s] = us; return; }
        }
    }
    void closeBucket(double t) {
        CpuBucket b; b.t = bucketT0_; b.totalUs = busyUs_;
        for (int c = 0; c < 64; ++c) { uint32_t p = (uint32_t)(coreUs_[c] / 500); b.coreBusyPct[c] = (uint16_t)(p > 100 ? 100 : p); }
        for (int k = 0; k < 8; ++k) {   // Top-8 per Auswahl (512 Slots, selten voll)
            uint32_t bi = 0, bu = 0;
            for (int s = 0; s < 512; ++s) if (hashUs_[s] > bu) { bu = hashUs_[s]; bi = s; }
            if (!bu) break;
            b.topPid[k] = hashPid_[bi]; b.topUs[k] = bu; hashUs_[bi] = 0;
        }
        cpu_.push(b);
        memset(hashPid_, 0, sizeof(hashPid_)); memset(hashUs_, 0, sizeof(hashUs_));
        memset(coreUs_, 0, sizeof(coreUs_)); busyUs_ = 0; bucketT0_ = t;
    }

    void workerLoop() {
        while (!stop_) {
            WaitForSingleObject(evt_, 200);
            uint32_t p = pendingSpike_.exchange(0, std::memory_order_acquire);
            if (!p || stop_) continue;
            analyzeSpike(pendingT_, pendingFt_, pendingMed_);
        }
    }

    void analyzeSpike(double t, double ftMs, double medMs);   // unten (lang)

    Config cfg_;
    Ring<FrameSample, 4096> frames_;
    Ring<DpcIsrSample, 16384> dpcs_;
    Ring<DiskSample, 4096> disks_;
    Ring<ProcSample, 256> procs_;
    Ring<SensorSample, 128> sensors_;
    Ring<CpuBucket, 256> cpu_;
    // CSwitch-Arbeitszustand (nur Kernel-ETW-Thread)
    Core cores_[64];
    uint32_t hashPid_[512] = {}, hashUs_[512] = {};
    uint64_t coreUs_[64] = {};
    uint32_t busyUs_ = 0;
    double bucketT0_ = 0;
    // Frame-Zustand (nur Present-Thread)
    double lastPresent_ = 0, lastSpikeT_ = -1;
    uint64_t frameCount_ = 0;
    std::vector<double> allFtT_; std::vector<float> allFt_;   // Gesamtserie (Report)
    // Spike-Uebergabe an den Worker
    std::atomic<uint32_t> pendingSpike_{ 0 };
    double pendingT_ = 0, pendingFt_ = 0, pendingMed_ = 0;
    // Ergebnisse (nur Worker-Thread schreibt; Broker liest nach stop())
    std::vector<Finding> findings_;
    HANDLE evt_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker_;
    std::atomic<bool> stop_{ false };
};

// ---- Fensteranalyse: Verdaechtige im +-200-ms-Fenster gegen die 2-s-Baseline ------
inline void StutterAnalyzer::analyzeSpike(double t, double ftMs, double medMs) {
    Finding f; f.id = (uint32_t)findings_.size() + 1; f.t = t; f.ftMs = ftMs; f.medianMs = medMs;
    const double w0 = t - 0.2, w1 = t + 0.05;          // Fenster: Ursache liegt VOR/IM Spike
    const double b0 = t - 2.2, b1 = t - 0.2;           // Baseline davor
    char ev[160];

    // 1) Prozess-CPU: Bucket-Deltas Fenster vs. Baseline (je PID us aufsummieren)
    {
        std::vector<CpuBucket> wb, bb;
        cpu_.range(w0, w1, wb); cpu_.range(b0, b1, bb);
        auto sumBy = [](const std::vector<CpuBucket>& v, std::vector<std::pair<uint32_t, uint64_t>>& out) {
            for (auto& b : v) for (int k = 0; k < 8; ++k) if (b.topPid[k]) {
                bool found = false;
                for (auto& e : out) if (e.first == b.topPid[k]) { e.second += b.topUs[k]; found = true; break; }
                if (!found) out.push_back({ b.topPid[k], b.topUs[k] });
            }
        };
        std::vector<std::pair<uint32_t, uint64_t>> wSum, bSum;
        sumBy(wb, wSum); sumBy(bb, bSum);
        double wDur = 0.25, bDur = 2.0;                  // Fenster-/Baseline-Laenge (s)
        for (auto& [pid, us] : wSum) {
            if (pid == cfg_.targetPid || pid == 4 || pid == 0) continue;   // Spiel/System-Idle nicht verdaechtigen
            uint64_t bUs = 0; for (auto& e : bSum) if (e.first == pid) { bUs = e.second; break; }
            double wCore = us / (wDur * 1e6), bCore = bUs / (bDur * 1e6);  // Anteil eines Kerns
            if (wCore > 0.15 && wCore > bCore * 2.5) {
                std::string nm = cfg_.pidName ? cfg_.pidName(pid) : "";
                if (nm.empty()) nm = "pid " + std::to_string(pid);
                sprintf_s(ev, "%.0f%% Kernlast im Ruckler-Fenster (Basis %.0f%%)", wCore * 100, bCore * 100);
                f.suspects.push_back({ "process", nm, ev, (std::min)(1.0, 0.4 + wCore) });
            }
        }
    }
    // 2) Treiber-DPC/ISR: Einzel-Ausreisser oder Fenster-Summe deutlich ueber Baseline
    {
        std::vector<DpcIsrSample> wd, bd;
        dpcs_.range(w0, w1, wd); dpcs_.range(b0, b1, bd);
        std::vector<std::pair<int, std::pair<double, double>>> agg;   // drv -> (sumUs, maxUs)
        for (auto& d : wd) {
            bool found = false;
            for (auto& e : agg) if (e.first == d.driverIdx) { e.second.first += d.durUs; if (d.durUs > e.second.second) e.second.second = d.durUs; found = true; break; }
            if (!found) agg.push_back({ d.driverIdx, { d.durUs, d.durUs } });
        }
        for (auto& [drv, su] : agg) {
            double bSum = 0, bMax = 0;
            for (auto& d : bd) if (d.driverIdx == drv) { bSum += d.durUs; if (d.durUs > bMax) bMax = d.durUs; }
            double bScaled = bSum * (0.25 / 2.0);        // Baseline auf Fensterlaenge skaliert
            if (su.second > 250.0 || (su.first > 1000.0 && su.first > bScaled * 3)) {
                std::string nm = cfg_.driverName ? cfg_.driverName(drv) : "(Treiber)";
                sprintf_s(ev, "DPC/ISR max %.0fus, Summe %.0fus im Fenster (Basis max %.0fus)", su.second, su.first, bMax);
                f.suspects.push_back({ "driver", nm, ev, (std::min)(1.0, 0.4 + su.second / 1000.0) });
            }
        }
    }
    // 3) GPU: Taktsturz / neue Throttle-Bits / VRAM-Sprung
    {
        std::vector<SensorSample> ws, bs;
        sensors_.range(w0, w1 + 0.15, ws); sensors_.range(b0, b1, bs);
        if (!ws.empty() && !bs.empty()) {
            double bClk = 0; int nClk = 0; uint64_t bMask = 0; int64_t bVram = -1;
            for (auto& s : bs) { if (s.gpuClockMHz > 0) { bClk += s.gpuClockMHz; ++nClk; } bMask |= s.throttleMask; if (s.vramMB > bVram) bVram = s.vramMB; }
            if (nClk) bClk /= nClk;
            for (auto& s : ws) {
                if (s.gpuClockMHz > 0 && bClk > 500 && s.gpuClockMHz < bClk - 200) {
                    sprintf_s(ev, "GPU-Takt %d MHz (Basis %.0f MHz)", s.gpuClockMHz, bClk);
                    f.suspects.push_back({ "gpu-throttle", "GPU-Takteinbruch", ev, 0.7 }); break;
                }
            }
            for (auto& s : ws) {
                uint64_t neu = s.throttleMask & ~bMask & ~0x1ull;   // Bit 0x1 = GpuIdle, kein Drossel-Grund
                if (s.throttleKnown && neu) {
                    const char* grund = (neu & 0x4) ? "Power-Limit" : (neu & 0x60) ? "Temperatur-Limit" : "Treiber-Limit";
                    sprintf_s(ev, "GPU drosselt: %s (Maske 0x%llx)", grund, (unsigned long long)neu);
                    f.suspects.push_back({ "gpu-throttle", grund, ev, 0.85 }); break;
                }
            }
            for (auto& s : ws) if (bVram > 0 && s.vramMB > bVram + 200) {
                sprintf_s(ev, "VRAM-Sprung auf %lld MB (+%lld)", (long long)s.vramMB, (long long)(s.vramMB - bVram));
                f.suspects.push_back({ "vram", "VRAM-Anstieg", ev, 0.5 }); break;
            }
        }
    }
    // 4) Disk-Burst
    {
        std::vector<DiskSample> wd;
        disks_.range(w0, w1, wd);
        double bytes = 0, maxLat = 0;
        for (auto& d : wd) { bytes += d.bytes; if (d.latMs > maxLat) maxLat = d.latMs; }
        if (bytes > 32.0 * 1048576 || maxLat > 50) {
            sprintf_s(ev, "%.1f MB Disk-I/O, max. Latenz %.0f ms", bytes / 1048576, maxLat);
            f.suspects.push_back({ "disk", "Datentraeger-Last", ev, maxLat > 50 ? 0.6 : 0.4 });
        }
    }
    // 5) Prozessstart im Fenster
    {
        std::vector<ProcSample> wp;
        procs_.range(t - 1.0, w1, wp);   // grosszuegiger: Start kurz vorher zaehlt
        for (auto& p : wp) if (p.start) {
            std::string nm = cfg_.pidName ? cfg_.pidName(p.pid) : "";
            if (nm.empty()) nm = "pid " + std::to_string(p.pid);
            sprintf_s(ev, "Prozess startete %.1f s vor dem Ruckler", t - p.t);
            f.suspects.push_back({ "proc-start", nm, ev, 0.45 });
        }
    }
    if (f.suspects.empty())
        f.suspects.push_back({ "game-internal", "", "Kein Systemstoerer im Fenster gefunden - vermutlich spielintern (z.B. Shader/Streaming)", 0.3 });
    std::sort(f.suspects.begin(), f.suspects.end(), [](const Suspect& a, const Suspect& b) { return a.score > b.score; });
    if (f.suspects.size() > 5) f.suspects.resize(5);
    findings_.push_back(f);
    if (cfg_.onFinding) cfg_.onFinding(f);
}

// ---- Befund als JSON-Zeile (findings.jsonl) ---------------------------------------
inline std::string findingJson(const Finding& f) {
    char head[128];
    sprintf_s(head, "{\"id\":%u,\"t\":%.3f,\"ftMs\":%.1f,\"medianMs\":%.1f,\"suspects\":[", f.id, f.t, f.ftMs, f.medianMs);
    std::string s = head;
    for (size_t i = 0; i < f.suspects.size(); ++i) {
        const Suspect& x = f.suspects[i];
        if (i) s += ",";
        char sc[32]; sprintf_s(sc, "%.2f", x.score);
        s += "{\"kind\":\"" + jsonEsc(x.kind) + "\",\"name\":\"" + jsonEsc(x.name) + "\",\"evidence\":\"" + jsonEsc(x.evidence) + "\",\"score\":" + sc + "}";
    }
    s += "]}";
    return s;
}

// ---- Selbstcheck: synthetischer Ruckler mit klarem Taeter -------------------------
inline bool StutterAnalyzer::selfCheck(std::string& msg) {
    StutterAnalyzer a;
    Config c;
    c.targetPid = 100;
    c.pidName = [](uint32_t pid) { return pid == 200 ? std::string("stoerer.exe") : std::string("spiel.exe"); };
    c.driverName = [](int) { return std::string("test.sys"); };
    Finding got; bool called = false;
    c.onFinding = [&](const Finding& f) { got = f; called = true; };
    a.start(c);
    // 3 s ruhige 60-fps-Frames, CPU-Buckets mit Stoerer-Last kurz vor t=3.0, dann 90-ms-Spike
    double t = 0;
    for (int i = 0; i < 180; ++i) { t = i * (1.0 / 60.0); a.pushFrame(100, t); }
    for (double bt = 2.75; bt < 3.0; bt += 0.05) {   // Stoerer frisst einen Kern
        for (int k = 0; k < 50; ++k) a.onCSwitch(bt + k * 0.001, 0, 0, 1, 200);
        a.onCSwitch(bt + 0.0499, 0, 0, 0, 0);
    }
    a.pushDpcIsr(2.95, 800.0, 0, false);             // + ein dicker DPC
    a.pushFrame(100, t + 0.090);                     // der Ruckler
    for (int i = 0; i < 20 && !called; ++i) Sleep(50);
    a.stop();
    if (!called) { msg = "Selbstcheck: KEIN Befund ausgeloest"; return false; }
    bool proc = false, drv = false;
    for (auto& s : got.suspects) { if (s.kind == "process" && s.name == "stoerer.exe") proc = true; if (s.kind == "driver") drv = true; }
    msg = "Selbstcheck: Befund ft=" + std::to_string((int)got.ftMs) + "ms, Verdaechtige=" + std::to_string(got.suspects.size())
        + (proc ? " [Prozess ok]" : " [PROZESS FEHLT]") + (drv ? " [Treiber ok]" : " [TREIBER FEHLT]");
    return proc && drv;
}

} // namespace luana
