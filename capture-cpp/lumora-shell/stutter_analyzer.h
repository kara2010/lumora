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
#include <map>
#include <memory>
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
    int64_t vramMB = -1, ramMB = -1, vramTotalMB = -1;
    uint64_t throttleMask = 0;      // NVML-ClocksThrottleReasons (0 = keiner/unbekannt)
    uint8_t throttleKnown = 0;      // 0 = Maske nicht auslesbar (AMD/alt) -> Takt-Heuristik
    int32_t gpuLoadPct = -1;        // GPU-Auslastung (NVML bzw. PDH-3D-Engine), -1 = unbekannt
};

// --- Flaschenhals-Klassifikation (Live-Anzeige + Verteilung im Bericht) -------------
// EHRLICHKEIT: Das ist eine Heuristik aus GPU-Auslastung, Kernlast und Frametime-
// Stabilitaet - KEINE frame-genaue GPU-Busy-Messung (die braeuchte DxgKrnl-Events).
// Deshalb gibt es die Stufe "unklar" statt einer erfundenen Aussage, und ein
// erkannter Framecap (VSync/Limiter) wird als solcher benannt, nicht als CPU-Limit.
enum class Limit : uint8_t { Unknown = 0, Gpu, Cpu, CpuCore, Framecap, Throttle, Vram };
inline const char* limitKey(Limit l) {
    switch (l) {
    case Limit::Gpu: return "gpu"; case Limit::Cpu: return "cpu"; case Limit::CpuCore: return "cpu-core";
    case Limit::Framecap: return "framecap"; case Limit::Throttle: return "throttle"; case Limit::Vram: return "vram";
    default: return "unknown";
    }
}
// CSwitch-Aggregat: ein Bucket je 50 ms. topPid/topUs = die 8 groessten CPU-Verbraucher.
struct CpuBucket {
    double t = 0;   // Bucket-Beginn (Feldname "t" wie in allen Ringen - Ring::range verlangt das)
    uint32_t topPid[8] = {}; uint32_t topUs[8] = {};
    uint32_t totalUs = 0;           // Summe aller Nicht-Idle-Laufzeit (alle Kerne)
    uint16_t coreBusyPct[64] = {};  // je Kern 0..100 (fuer Single-Core-Limit-Anzeige)
};

// ---- Session-Spuren (Datenmodell v2, ANALYSE-WERKBANK-PLAN.md) --------------------
// Die Ringe oben halten ~10 s Historie - fuer die Werkbank-Timeline braucht es die
// GANZE Session. Feste 250-ms-Buckets, Index k deckt [k*0.25, (k+1)*0.25) auf der
// Session-Zeitbasis (Luecken werden als Leer-Buckets gefuellt -> Index IST das
// Zeitraster, die UI braucht keine t-Spalte je Bucket).
// Threading: jede Spur hat genau EINEN Schreiber (cpu/dpc/disk/proc: Kernel-ETW-
// Consumer, gpu: Sensor-Thread, limits: Broker-Schleife); gelesen wird erst NACH
// stop() im Report-Writer. Vektoren sind auf 4 h vorreserviert - ist die Kapazitaet
// erreicht, ENDET die Spur (full) statt im Schreiber-Thread zu reallozieren. Der
// CSwitch-Callback selbst bleibt unangetastet (Overhead-Garantie): die CPU-Spur
// speist sich aus closeBucket, das ohnehin nur alle 50 ms laeuft.
constexpr uint32_t TRACK_MAX = 57600;   // 4 h * 4 Buckets/s
constexpr double   TRACK_DT  = 0.25;
struct CpuTrackB  { uint32_t totalUs = 0; uint16_t maxCorePct = 0; uint8_t maxCore = 0; uint32_t topPid = 0, topUs = 0; };
struct GpuTrackB  { int32_t clockMHz = -1; int16_t loadPct = -1; int32_t vramMB = -1; uint8_t throttle = 0; };
struct DpcTrackB  { uint32_t maxUs = 0; int32_t maxDrv = -1; uint32_t count = 0; };
struct DiskTrackB { uint32_t kb = 0; uint16_t maxLat10 = 0; };   // Latenz in 0,1-ms-Schritten
struct ProcEvent  { double t; uint32_t pid; uint8_t start; };
template <typename B>
struct SessionTrack {
    std::vector<B> v; B cur{}; double t0 = 0; bool full = false;
    void reserve() { v.reserve(TRACK_MAX); }
    // Buckets bis t schliessen; danach gehoert t in den (frischen) cur-Bucket.
    void roll(double t) {
        while (t >= t0 + TRACK_DT) {
            if (v.size() < TRACK_MAX) v.push_back(cur); else full = true;
            cur = B{}; t0 += TRACK_DT;
        }
    }
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
        // Wahrnehmbarkeits-Schwelle (Nutzer-Befund: 3x Median meldete bei 138 fps schon
        // 21-ms-Frames als "Ruckler" - das spuert niemand): ein Frame zaehlt erst, wenn
        // er k*Median UEBERschreitet UND absolut mindestens minSpikeMs lang ist
        // (~2+ verlorene Frames am Stueck = fuehlbarer Mikro-Ruckler).
        double spikeK = 3.0;            // Spike, wenn ft > max(k*Median, Median+15ms) UND ft >= minSpikeMs
        double spikeAbsMs = 15.0;
        double minSpikeMs = 28.0;
        double debounceS = 0.25;        // max. 1 Befund je 250 ms
        uint32_t targetPid = 0;         // 0 = aktivster Praesentierer (setzt der Broker)
        std::function<std::string(uint32_t pid)> pidName;    // pidExeName
        std::function<std::string(int idx)> driverName;      // KernelTrace::drivers().name
        std::function<void(const Finding&)> onFinding;       // laeuft auf dem Worker-Thread
    };

    void start(const Config& cfg) {
        cfg_ = cfg;
        stop_ = false;
        // v2-Spuren: Kapazitaet VOR dem ersten Event reservieren - die Schreiber-Threads
        // (ETW/Sensor) duerfen nie eine Reallokation ausloesen (s. SessionTrack-Kommentar).
        trkCpu_.reserve(); trkGpu_.reserve(); trkDpc_.reserve(); trkDisk_.reserve();
        procEv_.reserve(4096); limitsSeries_.reserve(14400);
        SYSTEM_INFO si{}; GetSystemInfo(&si);
        nCores_ = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
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
    // Empfindlichkeit live umstellbar (UI-Regler): 0 = empfindlich, 1 = normal,
    // 2 = nur harte Ruckler. Doubles einzeln geschrieben - Leser-Race harmlos.
    void setSensitivity(int level) {
        if (level <= 0)      { cfg_.minSpikeMs = 18.0; cfg_.spikeK = 2.5; }
        else if (level == 1) { cfg_.minSpikeMs = 28.0; cfg_.spikeK = 3.0; }
        else                 { cfg_.minSpikeMs = 45.0; cfg_.spikeK = 3.5; }
    }
    void setTargetPid(uint32_t pid) {
        cfg_.targetPid = pid;
        // Schonfrist nach (Neu-)Wahl des Ziels: die ersten Sekunden eines Spiels sind
        // Lade-/Shader-Phase - deren Spitzen als "Ruckler mit Taeter Spiel" zu melden
        // ist irrefuehrend (real passiert: Forza wurde beim Start selbst verdaechtigt).
        graceUntil_ = lastPresent_ + 10.0;
    }
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
        // Fester Live-Ring fuer recentFt (OSD-Schrieb): der grosse Vektor darf von fremden
        // Threads NICHT gelesen werden (push_back-Reallokation -> Use-after-free; real
        // passiert: Broker-Absturz nach ~5 Minuten Messung). Ring ist fix + lockfrei.
        recent_[frameCount_ % 64] = ft;
        ++frameCount_;
        // Spike-Pruefung inline (billig): Median der letzten 64 Frametimes
        double med = median64();
        if (med > 0 && t >= graceUntil_ && ft >= cfg_.minSpikeMs
            && ft > (std::max)(cfg_.spikeK * med, med + cfg_.spikeAbsMs)) {
            if (t - lastSpikeT_ >= cfg_.debounceS) {
                lastSpikeT_ = t;
                // ERST die Nutzdaten, DANN das Release-Signal: andersherum konnte der
                // Worker (wenn noch ein Wake anstand) zwischen Signal und Nutzdaten
                // zugreifen und den Spike mit den Werten des VORIGEN analysieren.
                pendingFt_ = ft; pendingMed_ = med; pendingT_ = t;
                pendingSpike_.store(1 + (uint32_t)(t * 1000), std::memory_order_release);
                wake();
            }
        }
    }
    void pushDpcIsr(double t, double durUs, int driverIdx, bool isr) {
        dpcs_.push({ t, (float)durUs, driverIdx, (uint8_t)(isr ? 1 : 0) });
        trkDpc_.roll(t);
        auto& c = trkDpc_.cur; ++c.count;
        if ((uint32_t)durUs > c.maxUs) { c.maxUs = (uint32_t)durUs; c.maxDrv = driverIdx; }
    }
    void pushDisk(double t, uint32_t bytes, double latMs) {
        disks_.push({ t, bytes, (float)latMs });
        trkDisk_.roll(t);
        auto& c = trkDisk_.cur; c.kb += bytes / 1024;
        uint32_t l10 = (uint32_t)(latMs * 10); if (l10 > 65535) l10 = 65535;
        if ((uint16_t)l10 > c.maxLat10) c.maxLat10 = (uint16_t)l10;
    }
    void pushProc(double t, uint32_t pid, bool start) {
        procs_.push({ t, pid, (uint8_t)(start ? 1 : 0) });
        if (procEv_.size() < procEv_.capacity()) procEv_.push_back({ t, pid, (uint8_t)(start ? 1 : 0) });
    }
    void pushSensor(const SensorSample& s) {
        sensors_.push(s);
        trkGpu_.roll(s.t);
        auto& c = trkGpu_.cur;
        if (s.gpuClockMHz > 0) c.clockMHz = s.gpuClockMHz;                     // letzter Wert
        if (s.gpuLoadPct > c.loadPct) c.loadPct = (int16_t)s.gpuLoadPct;      // Maximum
        if (s.vramMB > c.vramMB) c.vramMB = (int32_t)s.vramMB;
        if (s.throttleKnown && (s.throttleMask & ~0x1ull)) c.throttle = 1;    // Bit 0x1 = GpuIdle
    }
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

    // Live-Werte fuer den 100ms-SHM-Schrieb der Broker-Schleife: AUSSCHLIESSLICH aus dem
    // festen frames_-Ring (tolerierter Race, s. Klassenkommentar). stats() unten liest die
    // WACHSENDEN Report-Vektoren und ist waehrend der Messung tabu - push_back-Reallokation
    // im Present-Thread + Kopie im Fremd-Thread = Use-after-free, exakt die Klasse des
    // behobenen recentFt-Absturzes (Broker-Crash nach ~5 Minuten, c2758b6).
    struct Live { double medianFtMs = 0, avgFps = 0; };
    Live live() const {
        Live l;
        uint32_t end = frames_.idx.load(std::memory_order_acquire);
        uint32_t n = end < 256 ? end : 256;
        if (n < 4) return l;
        float tmp[256]; double newest = 0, oldest = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const FrameSample& e = frames_.buf[(end - 1 - i) % 4096];
            tmp[i] = e.ftMs;
            if (i == 0) newest = e.t;
            oldest = e.t;
        }
        std::nth_element(tmp, tmp + n / 2, tmp + n);
        l.medianFtMs = tmp[n / 2];
        double span = newest - oldest;
        l.avgFps = span > 0 ? (double)(n - 1) / span : 0;
        return l;
    }

    // ---- Report-Daten (NUR nach stop() vom Broker lesen - waehrend der Messung live()!) ----
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
    // --- Flaschenhals: Klassifikation des LETZTEN Sekundenfensters + Verteilung -----
    // Aufruf 1x/s aus der Broker-Schleife. Entscheidungsreihenfolge bewusst so:
    // harte Fakten (Throttle/VRAM) vor Auslastungs-Heuristik, und ein Framecap wird
    // VOR dem CPU-Verdacht geprueft - sonst wuerde jedes VSync-Limit als "CPU-Limit"
    // gemeldet (der klassische Fehlschluss solcher Tools).
    Limit classifyLimit() {
        double now = lastPresent_;
        if (now <= 0) return Limit::Unknown;
        std::vector<FrameSample> fr; frames_.range(now - 1.0, now + 0.01, fr);
        if (fr.size() < 20) return Limit::Unknown;                 // zu wenig Daten
        std::vector<SensorSample> ss; sensors_.range(now - 1.0, now + 0.01, ss);
        std::vector<CpuBucket> cb; cpu_.range(now - 1.0, now + 0.01, cb);

        // Frametime-Statistik des Fensters
        std::vector<float> ft; ft.reserve(fr.size());
        for (auto& f : fr) ft.push_back(f.ftMs);
        std::sort(ft.begin(), ft.end());
        double med = ft[ft.size() / 2];
        double p10 = ft[(size_t)(ft.size() * 0.10)], p90 = ft[(size_t)(ft.size() * 0.90)];
        double jitter = med > 0 ? (p90 - p10) / med : 1.0;         // relative Streuung

        // Sensorik mitteln
        int gpuLoad = -1, gpuClk = -1; uint64_t thr = 0; uint8_t thrKnown = 0;
        int64_t vram = -1, vramTot = -1;
        { int n = 0, sum = 0;
          for (auto& s : ss) { if (s.gpuLoadPct >= 0) { sum += s.gpuLoadPct; ++n; }
              if (s.gpuClockMHz > 0) gpuClk = s.gpuClockMHz;
              thr |= s.throttleMask; thrKnown |= s.throttleKnown;
              if (s.vramMB > vram) vram = s.vramMB; if (s.vramTotalMB > 0) vramTot = s.vramTotalMB; }
          if (n) gpuLoad = sum / n; }
        int coreMax = 0;
        for (auto& b : cb) for (int c = 0; c < 64; ++c) if (b.coreBusyPct[c] > coreMax) coreMax = b.coreBusyPct[c];

        // 1) Harte Ursachen zuerst (auslesbare Fakten statt Heuristik)
        if (thrKnown && (thr & ~0x1ull) && gpuLoad > 50) return Limit::Throttle;   // Bit 0x1 = GpuIdle
        if (vramTot > 0 && vram > 0 && vram > vramTot * 94 / 100) return Limit::Vram;
        // 2) Framecap/VSync: sehr gleichmaessige Frametimes UND GPU nicht am Anschlag
        if (jitter < 0.12 && gpuLoad >= 0 && gpuLoad < 92) return Limit::Framecap;
        // 3) GPU am Anschlag -> GPU-Limit
        if (gpuLoad >= 95) return Limit::Gpu;
        // 4) GPU hat Luft: CPU-seitig gebremst? Einzelkern-Anschlag ist der haeufigste Fall
        if (gpuLoad >= 0 && gpuLoad < 85) return coreMax >= 90 ? Limit::CpuCore : Limit::Cpu;
        return Limit::Unknown;   // Graubereich 85-95 % ohne klares Indiz: lieber nichts behaupten
    }
    void sampleLimit() {                       // 1x/s aufrufen: Verteilung mitschreiben
        Limit l = classifyLimit();
        lastLimit_ = l;
        limitCount_[(int)l]++; limitTotal_++;
        // Session-Spur (v2): Sekundenserie der Klassifikation (Index = Sekunde).
        if (limitsSeries_.size() < limitsSeries_.capacity()) limitsSeries_.push_back((uint8_t)l);
    }
    Limit lastLimit() const { return lastLimit_; }
    uint32_t limitCount(Limit l) const { return limitCount_[(int)l]; }
    uint32_t limitTotal() const { return limitTotal_; }

    const std::vector<Finding>& findings() const { return findings_; }
    uint32_t spikeCount() const { return (uint32_t)findings_.size(); }
    // Letzte n Frametimes (aeltester zuerst) fuer den Live-Schrieb im Analyse-OSD -
    // AUSSCHLIESSLICH aus dem festen recent_-Ring (nie aus dem wachsenden Vektor,
    // s. Kommentar in pushFrame: Reallokation + Fremd-Thread = Absturz).
    uint32_t recentFt(float* out, uint32_t n) const {
        if (n > 64) n = 64;
        uint64_t fc = frameCount_;
        uint32_t cnt = (uint32_t)(fc < n ? fc : n);
        for (uint32_t i = 0; i < cnt; ++i) out[i] = recent_[(fc - cnt + i) % 64];
        return cnt;
    }
    uint64_t frameCount() const { return frameCount_; }

    // ---- Datenmodell v2: Emitter fuer den Report-Writer (NUR nach stop() rufen -
    // liest die wachsenden Vektoren, die waehrend der Messung Schreiber-Threads
    // gehoeren). Liefert die Felder ftFull/tracks/names OHNE fuehrendes Komma.
    std::string v2Json() const {
        std::string o; o.reserve(allFt_.size() * 5 + trkCpu_.v.size() * 40 + 4096);
        char b[96];
        // ftFull: Frametimes in 10-us-Einheiten; Zeitachse kumulativ rekonstruierbar
        // (x[0]=t0, x[i]=x[i-1]+q[i]/100000). Quantisierungsdrift ziehen sync-Punkte
        // [index, t] alle 512 Frames gerade.
        o += "\"ftFull\":{\"t0\":";
        sprintf_s(b, "%.4f", allFtT_.empty() ? 0.0 : allFtT_[0]); o += b;
        o += ",\"q\":[";
        for (size_t i = 0; i < allFt_.size(); ++i) {
            uint32_t q = (uint32_t)(allFt_[i] * 100.0f + 0.5f);
            sprintf_s(b, "%s%u", i ? "," : "", q); o += b;
        }
        o += "],\"sync\":[";
        { bool first = true;
          for (size_t i = 0; i < allFtT_.size(); i += 512) {
              sprintf_s(b, "%s[%zu,%.4f]", first ? "" : ",", i, allFtT_[i]); o += b; first = false;
          } }
        o += "]},";
        // tracks: feste 250-ms-Buckets, Index = Zeitraster ab Session-Beginn.
        // Der letzte (angefangene) cur-Bucket wird mit emittiert; Spuren duerfen
        // unterschiedlich lang sein (verschiedene letzte Ereignisse).
        auto emitCpu = [&](const CpuTrackB& c, bool first) {
            // totalPct normiert auf alle Kerne; topPct auf EINEN Kern (kann >100 sein
            // = Prozess nutzt mehrere Kerne)
            uint32_t totalPct = (uint32_t)(c.totalUs * 100.0 / (nCores_ * 250000.0) + 0.5);
            if (totalPct > 100) totalPct = 100;
            uint32_t topPct = c.topUs / 500;   // us in 50-ms-Fenster -> % eines Kerns
            sprintf_s(b, "%s[%u,%u,%u,%u,%u]", first ? "" : ",", totalPct, c.maxCorePct, c.maxCore, c.topPid, topPct);
            o += b;
        };
        o += "\"tracks\":{\"hz\":4,\"cpu\":[";
        for (size_t i = 0; i < trkCpu_.v.size(); ++i) emitCpu(trkCpu_.v[i], i == 0);
        emitCpu(trkCpu_.cur, trkCpu_.v.empty());
        o += "],\"gpu\":[";
        auto emitGpu = [&](const GpuTrackB& g, bool first) {
            sprintf_s(b, "%s[%d,%d,%d,%u]", first ? "" : ",", g.clockMHz, (int)g.loadPct, g.vramMB, (unsigned)g.throttle); o += b;
        };
        for (size_t i = 0; i < trkGpu_.v.size(); ++i) emitGpu(trkGpu_.v[i], i == 0);
        emitGpu(trkGpu_.cur, trkGpu_.v.empty());
        o += "],\"dpc\":[";
        auto emitDpc = [&](const DpcTrackB& d, bool first) {
            sprintf_s(b, "%s[%u,%d,%u]", first ? "" : ",", d.maxUs, d.maxDrv, d.count); o += b;
        };
        for (size_t i = 0; i < trkDpc_.v.size(); ++i) emitDpc(trkDpc_.v[i], i == 0);
        emitDpc(trkDpc_.cur, trkDpc_.v.empty());
        o += "],\"disk\":[";
        auto emitDisk = [&](const DiskTrackB& d, bool first) {
            sprintf_s(b, "%s[%u,%u]", first ? "" : ",", d.kb, (unsigned)d.maxLat10); o += b;
        };
        for (size_t i = 0; i < trkDisk_.v.size(); ++i) emitDisk(trkDisk_.v[i], i == 0);
        emitDisk(trkDisk_.cur, trkDisk_.v.empty());
        o += "],\"procEvents\":[";
        for (size_t i = 0; i < procEv_.size(); ++i) {
            sprintf_s(b, "%s[%.3f,%u,%u]", i ? "," : "", procEv_[i].t, procEv_[i].pid, (unsigned)procEv_[i].start); o += b;
        }
        o += "],\"limits\":[";
        for (size_t i = 0; i < limitsSeries_.size(); ++i) { sprintf_s(b, "%s%u", i ? "," : "", (unsigned)limitsSeries_[i]); o += b; }
        o += "],\"truncated\":";
        o += (trkCpu_.full || trkGpu_.full || trkDpc_.full || trkDisk_.full) ? "true" : "false";
        o += "},";
        // names: einmalige Aufloesung aller in den Spuren vorkommenden PIDs/Treiber
        // (waehrend der Messung wird bewusst NIE aufgeloest - Overhead).
        o += "\"names\":{\"procs\":{";
        {   std::map<uint32_t, std::string> pn;
            auto sammel = [&](uint32_t pid) {
                if (pid && !pn.count(pid)) pn[pid] = cfg_.pidName ? cfg_.pidName(pid) : "";
            };
            for (const auto& c : trkCpu_.v) sammel(c.topPid);
            sammel(trkCpu_.cur.topPid);
            for (const auto& p : procEv_) sammel(p.pid);
            bool first = true;
            for (const auto& [pid, nm] : pn) {
                sprintf_s(b, "%s\"%u\":\"", first ? "" : ",", pid); o += b; o += jsonEsc(nm) + "\""; first = false;
            }
        }
        o += "},\"drivers\":{";
        {   std::map<int32_t, std::string> dn;
            for (const auto& d : trkDpc_.v) if (d.maxDrv >= 0 && !dn.count(d.maxDrv)) dn[d.maxDrv] = cfg_.driverName ? cfg_.driverName(d.maxDrv) : "";
            if (trkDpc_.cur.maxDrv >= 0 && !dn.count(trkDpc_.cur.maxDrv)) dn[trkDpc_.cur.maxDrv] = cfg_.driverName ? cfg_.driverName(trkDpc_.cur.maxDrv) : "";
            bool first = true;
            for (const auto& [idx, nm] : dn) {
                sprintf_s(b, "%s\"%d\":\"", first ? "" : ",", idx); o += b; o += jsonEsc(nm) + "\""; first = false;
            }
        }
        o += "}}";
        return o;
    }

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
        // Session-Spur (v2): den geschlossenen 50-ms-Bucket in den laufenden
        // 250-ms-Bucket falten. Laeuft im selben (ETW-)Thread wie der Rest von
        // closeBucket - alle 50 ms, weit ausserhalb des heissen CSwitch-Pfads.
        trkCpu_.roll(b.t);
        auto& tc = trkCpu_.cur; tc.totalUs += b.totalUs;
        for (int c = 0; c < 64; ++c) if (b.coreBusyPct[c] > tc.maxCorePct) { tc.maxCorePct = b.coreBusyPct[c]; tc.maxCore = (uint8_t)c; }
        if (b.topUs[0] > tc.topUs) { tc.topUs = b.topUs[0]; tc.topPid = b.topPid[0]; }
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
    double lastPresent_ = 0, lastSpikeT_ = -1, graceUntil_ = 0;
    volatile uint64_t frameCount_ = 0;
    float recent_[64] = {};   // fester Live-Ring fuer recentFt (lockfrei, nie realloziert)
    Limit lastLimit_ = Limit::Unknown;
    uint32_t limitCount_[8] = {}, limitTotal_ = 0;   // Verteilung ueber die Session
    std::vector<double> allFtT_; std::vector<float> allFt_;   // Gesamtserie (Report)
    // v2-Session-Spuren (je genau EIN Schreiber-Thread, Leser erst nach stop())
    SessionTrack<CpuTrackB>  trkCpu_;    // ETW-Thread (via closeBucket)
    SessionTrack<GpuTrackB>  trkGpu_;    // Sensor-Thread
    SessionTrack<DpcTrackB>  trkDpc_;    // ETW-Thread
    SessionTrack<DiskTrackB> trkDisk_;   // ETW-Thread
    std::vector<ProcEvent>   procEv_;    // ETW-Thread
    std::vector<uint8_t>     limitsSeries_;   // Broker-Schleife (sampleLimit)
    uint32_t nCores_ = 1;
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
            // Schwelle wahrnehmungs-orientiert (Nutzer-Befund: 15% verdaechtigte staendig
            // harmlose Systemprozesse): erst ab ~einem Drittel Kern UND deutlichem Sprung
            // gegenueber der Baseline ist ein Prozess ein plausibler Mit-Verursacher.
            if (wCore > 0.35 && wCore > bCore * 3.0) {
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
            // Einzel-DPCs unter 1 ms sind Alltag (auch nvlddmkm) - erst ab ~1 ms Einzeldauer
            // oder massiver Fenster-Summe (>3 ms UND 4x Baseline) plausibel rucklerrelevant.
            if (su.second > 1000.0 || (su.first > 3000.0 && su.first > bScaled * 4)) {
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
        for (auto& p : wp) if (p.start && p.pid != cfg_.targetPid) {   // das Spiel selbst ist nie "Stoerer durch Start"
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
    // HEAP statt Stapel: die festen Ringe machen den Analyzer ~650KB gross - zwei
    // Instanzen auf demselben Stapel (Aufrufer + diese) sprengen die 1-MB-Grenze
    // (real passiert: 0xC00000FD im --test-analyze).
    auto ap = std::make_unique<StutterAnalyzer>();
    StutterAnalyzer& a = *ap;
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
    a.pushDpcIsr(2.95, 1500.0, 0, false);            // + ein dicker DPC (>1ms-Schwelle)
    a.pushFrame(100, t + 0.090);                     // der Ruckler
    for (int i = 0; i < 20 && !called; ++i) Sleep(50);
    a.stop();
    if (!called) { msg = "Selbstcheck: KEIN Befund ausgeloest"; return false; }
    bool proc = false, drv = false;
    for (auto& s : got.suspects) { if (s.kind == "process" && s.name == "stoerer.exe") proc = true; if (s.kind == "driver") drv = true; }
    // v2-Emitter pruefen: (1) ftFull-Rekonstruktion x[i]=x[i-1]+q[i]/1e5 darf gegen die
    // echten Zeitstempel nur um die Quantisierung (10 us/Frame, hier 180 Frames) driften,
    // (2) die Spuren muessen den Stoerer (pid 200) und den 1,5-ms-DPC enthalten.
    {
        double drift = 0, x = a.allFtT_.empty() ? 0 : a.allFtT_[0];
        for (size_t i = 1; i < a.allFt_.size(); ++i) {
            uint32_t q = (uint32_t)(a.allFt_[i] * 100.0f + 0.5f);
            x += q / 100000.0;
            double d = x - a.allFtT_[i]; if (d < 0) d = -d; if (d > drift) drift = d;
        }
        if (drift > 0.002) { msg = "Selbstcheck: ftFull-Drift " + std::to_string(drift * 1000) + " ms > 2 ms"; return false; }
        std::string v2 = a.v2Json();
        bool hatStoerer = v2.find("\"200\":\"stoerer.exe\"") != std::string::npos;
        bool hatDpc = v2.find("[1500,0,1]") != std::string::npos;   // maxUs=1500, drv=0, count=1
        bool klammern = std::count(v2.begin(), v2.end(), '[') == std::count(v2.begin(), v2.end(), ']')
                     && std::count(v2.begin(), v2.end(), '{') == std::count(v2.begin(), v2.end(), '}');
        if (!hatStoerer || !hatDpc || !klammern) {
            msg = std::string("Selbstcheck v2: ") + (hatStoerer ? "" : "[STOERER FEHLT in names] ")
                + (hatDpc ? "" : "[DPC-BUCKET FEHLT] ") + (klammern ? "" : "[KLAMMERN UNBALANCIERT]");
            return false;
        }
    }
    msg = "Selbstcheck: Befund ft=" + std::to_string((int)got.ftMs) + "ms, Verdaechtige=" + std::to_string(got.suspects.size())
        + (proc ? " [Prozess ok]" : " [PROZESS FEHLT]") + (drv ? " [Treiber ok]" : " [TREIBER FEHLT]") + " [v2 ok]";
    return proc && drv;
}

} // namespace luana
