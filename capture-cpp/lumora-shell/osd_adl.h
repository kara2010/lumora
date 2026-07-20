// AMD-GPU-Sensoren via ADL (atiadlxx.dll) - treiberfrei, Pendant zu NVML.
// 1:1 aus main.js setupAdl/readAdlIdx portiert. Laeuft nur mit AMD-Treiber
// (sonst inert - atiadlxx.dll fehlt). Moderner Weg ADL2_New_QueryPMLogData_Get:
// fuellt {supported,value}[256] ab Offset 4; wir lesen roh per Offset (robuster
// als Structs). Multi-GPU: dedizierte Radeon (Edge/Hotspot-Temp) wird bevorzugt.
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>

namespace luadl {

// Sensor-IDs laut ADL SDK.
enum { PM_gfxclk = 1, PM_tempEdge = 8, PM_gfxActivity = 19, PM_asicPower = 23, PM_tempHotspot = 27, PM_tempGfx = 28, PM_gfxPower = 30 };

struct Adapter { int idx; std::string name; bool hasSensors; bool dedicated; };
struct GpuVals { bool ok = false, hasTemp = false, hasClock = false, hasPower = false, hasLoad = false; int temp = 0, clock = 0, power = 0, load = 0; std::string name; };

typedef void* (__stdcall* AdlMalloc_t)(int);
typedef int (*Create_t)(AdlMalloc_t, int, void**);
typedef int (*NumAdapters_t)(void*, int*);
typedef int (*AdapterInfo_t)(void*, void*, int);
typedef int (*QueryPMLog_t)(void*, int, void*);

static bool g_tried = false, g_ok = false;
static void* g_ctx = nullptr;
static QueryPMLog_t g_query = nullptr;
static int g_activeIdx = 0;
static std::string g_activeName;
static std::vector<Adapter> g_adapters;   // nur die mit auslesbaren Sensoren

static void* __stdcall adlMallocCb(int s) { return malloc(s); }

inline int rdI32(const uint8_t* p, size_t off) { int32_t v; memcpy(&v, p + off, 4); return v; }

// Einen QueryPMLogData-Puffer auslesen: val(id) = supported ? value : NONE.
struct PmBuf { uint8_t d[2052]; };
inline bool pmVal(const PmBuf& b, int id, int& out) {
    size_t o = 4 + (size_t)id * 8;
    if (rdI32(b.d, o) == 0) return false;
    out = rdI32(b.d, o + 4); return true;
}

inline void setup() {
    if (g_tried) return;
    g_tried = true;
    HMODULE lib = LoadLibraryW(L"atiadlxx.dll");
    if (!lib) return;   // kein AMD-Treiber -> inert
    auto create = (Create_t)GetProcAddress(lib, "ADL2_Main_Control_Create");
    auto numGet = (NumAdapters_t)GetProcAddress(lib, "ADL2_Adapter_NumberOfAdapters_Get");
    auto infoGet = (AdapterInfo_t)GetProcAddress(lib, "ADL2_Adapter_AdapterInfo_Get");
    g_query = (QueryPMLog_t)GetProcAddress(lib, "ADL2_New_QueryPMLogData_Get");
    if (!create || !numGet || !infoGet || !g_query) return;
    void* ctx = nullptr;
    if (create(adlMallocCb, 1, &ctx) != 0 || !ctx) return;
    g_ctx = ctx;
    int num = 0;
    if (numGet(ctx, &num) != 0 || num <= 0) return;
    const int INFO = 1572;   // ADLAdapterInfo (x64): idx@4, vendor@276, name@280, present@792
    std::vector<uint8_t> buf((size_t)num * INFO);
    if (infoGet(ctx, buf.data(), (int)buf.size()) != 0) return;
    std::vector<int> amdIdxs; std::vector<std::string> amdNames;
    for (int i = 0; i < num; ++i) {
        size_t o = (size_t)i * INFO;
        int idx = rdI32(buf.data(), o + 4), vendor = rdI32(buf.data(), o + 276), present = rdI32(buf.data(), o + 792);
        const char* nmp = (const char*)(buf.data() + o + 280);
        std::string name(nmp, strnlen(nmp, 256));
        bool isAmd = (vendor == 1002 || vendor == 0x1002);   // ADL liefert 1002 dezimal
        if (isAmd && present) {
            bool seen = false; for (int x : amdIdxs) if (x == idx) { seen = true; break; }
            if (!seen) { amdIdxs.push_back(idx); amdNames.push_back(name); }
        }
    }
    if (amdIdxs.empty()) return;
    // Je AMD-Adapter pruefen: liefert er Werte, und ist er eine dedizierte Radeon?
    int dedIdx = -1, anyIdx = -1; std::string dedName, anyName;
    for (size_t k = 0; k < amdIdxs.size(); ++k) {
        int ai = amdIdxs[k];
        PmBuf pm{};
        if (g_query(ctx, ai, pm.d) != 0) continue;
        bool hasSensors = false;
        for (int id = 0; id < 256; ++id) if (rdI32(pm.d, 4 + (size_t)id * 8)) { hasSensors = true; break; }
        int tmp;
        bool dedicated = pmVal(pm, PM_tempEdge, tmp) || pmVal(pm, PM_tempHotspot, tmp);
        if (hasSensors) {
            g_adapters.push_back({ ai, amdNames[k], true, dedicated });
            if (dedicated && dedIdx < 0) { dedIdx = ai; dedName = amdNames[k]; }
            if (anyIdx < 0) { anyIdx = ai; anyName = amdNames[k]; }
        }
    }
    if (anyIdx < 0) return;   // kein AMD-Adapter mit Sensoren
    g_activeIdx = dedIdx >= 0 ? dedIdx : anyIdx;
    g_activeName = dedIdx >= 0 ? dedName : anyName;
    g_ok = true;
}

inline bool available() { return g_ok; }
inline const std::vector<Adapter>& adapters() { return g_adapters; }
inline int activeIdx() { return g_activeIdx; }
inline const std::string& activeName() { return g_activeName; }

// Sensoren fuer einen Adapter-Index lesen (name = Anzeigename, roh).
inline GpuVals read(int idx, const std::string& name) {
    GpuVals r;
    if (!g_ok || !g_query || !g_ctx) return r;
    PmBuf pm{};
    if (g_query(g_ctx, idx, pm.d) != 0) return r;
    int v;
    if (pmVal(pm, PM_tempEdge, v) || pmVal(pm, PM_tempHotspot, v) || pmVal(pm, PM_tempGfx, v)) { r.hasTemp = true; r.temp = v; }
    if (pmVal(pm, PM_gfxclk, v)) { r.hasClock = true; r.clock = v; }
    if (pmVal(pm, PM_gfxPower, v) || pmVal(pm, PM_asicPower, v)) { r.hasPower = true; r.power = v; }
    if (pmVal(pm, PM_gfxActivity, v)) { r.hasLoad = true; r.load = v; }
    if (!r.hasTemp && !r.hasClock && !r.hasPower && !r.hasLoad) return r;   // nichts -> ok bleibt false
    r.ok = true;
    r.name = name.empty() ? g_activeName : name;
    return r;
}
inline GpuVals readActive() { return read(g_activeIdx, g_activeName); }

} // namespace luadl
