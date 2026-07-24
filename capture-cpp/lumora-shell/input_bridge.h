// input_bridge.h - Eingabe-Bruecke: beliebige HID-Geraete (Joystick, Lenkrad, ...)
// auf ein virtuelles Xbox-360-Pad mappen (ViGEmBus-Treiber, MIT-lizenzierter Client
// einkompiliert aus third_party/ViGEmClient). Damit sind auch MS-Store/Xbox-Titel
// (Forza & Co.), wo Steam Input nicht greift, mit jedem Geraet steuerbar -
// Barrierefreiheits-Faelle inklusive (Knopf->Achse, Achse->Knopf).
//
// Architektur: EIGENER Thread mit message-only Window und eigener Message-Pump.
// Eingabe kommt event-getrieben per WM_INPUT (RawInput, RIDEV_INPUTSINK) - Latenz
// im HID-Intervall (1-8 ms), NIE ueber den UI-Thread (dessen 40-ms-Timer waere fuer
// Gameplay viel zu langsam). ViGEm-Report wird direkt im WM_INPUT-Handler
// aktualisiert; ein 100-ms-Keepalive haelt den Report bei Funkstille frisch.
#pragma once
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#include "ViGEm/Client.h"
#include "json.hpp"
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <functional>
#include <cmath>

namespace lubridge {

using nlohmann::json;

// Push-Funktion (sendToUi aus main.cpp) - wird bei init() gesetzt, damit dieser
// Header keine Abhaengigkeit auf die Shell-Interna hat.
inline std::function<void(const std::string&, const json&)> g_push;
inline void init(std::function<void(const std::string&, const json&)> push) { g_push = std::move(push); }

// ---- Hilfen -----------------------------------------------------------------
inline std::string narrow8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
// VID/PID aus dem RawInput-Geraetenamen ("\\?\HID#VID_046D&PID_C24F#..." ) ziehen.
inline void vidPidFromName(const std::wstring& name, std::string& vid, std::string& pid) {
    std::wstring up = name; for (auto& c : up) c = towupper(c);
    size_t v = up.find(L"VID_"), p = up.find(L"PID_");
    if (v != std::wstring::npos && v + 8 <= up.size()) vid = narrow8(up.substr(v + 4, 4));
    if (p != std::wstring::npos && p + 8 <= up.size()) pid = narrow8(up.substr(p + 4, 4));
}
// Produktname direkt vom HID-Geraet (CreateFile auf den Interface-Pfad).
inline std::string productName(const std::wstring& devName) {
    HANDLE h = CreateFileW(devName.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    wchar_t buf[256] = {};
    std::string r = HidD_GetProductString(h, buf, sizeof(buf)) ? narrow8(buf) : "";
    CloseHandle(h);
    return r;
}

// ---- Geraete-Enumeration (fuer die UI-Liste; laeuft auf beliebigem Thread) ----
inline bool isBridgeUsage(USHORT page, USHORT usage) {
    return page == 0x01 && (usage == 0x04 || usage == 0x05 || usage == 0x08);   // Joystick/Gamepad/Multi-Axis
}
inline json listDevices() {
    json arr = json::array();
    UINT n = 0; GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    if (!n) return arr;
    std::vector<RAWINPUTDEVICELIST> list(n);
    if (GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) return arr;
    for (auto& d : list) {
        if (d.dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info{}; info.cbSize = sizeof(info); UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) continue;
        if (!isBridgeUsage(info.hid.usUsagePage, info.hid.usUsage)) continue;
        UINT nsz = 0; GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICENAME, nullptr, &nsz);
        std::wstring name(nsz, 0);
        if (GetRawInputDeviceInfoW(d.hDevice, RIDI_DEVICENAME, &name[0], &nsz) == (UINT)-1) continue;
        name.resize(wcslen(name.c_str()));
        std::string vid, pid; vidPidFromName(name, vid, pid);
        std::string prod = productName(name);
        char vp[16]; snprintf(vp, sizeof(vp), "%04X", info.hid.dwVendorId);
        char pp[16]; snprintf(pp, sizeof(pp), "%04X", info.hid.dwProductId);
        if (vid.empty()) vid = vp;
        if (pid.empty()) pid = pp;
        arr.push_back({ {"vid", vid}, {"pid", pid}, {"name", prod.empty() ? ("HID " + vid + ":" + pid) : prod},
                        {"usage", info.hid.usUsage} });
    }
    return arr;
}

// ---- Mapping-Modell (aus input-profiles.json geparst) -------------------------
enum class Tgt { NONE, LX, LY, RX, RY, LT, RT, DPAD,
                 A, B, X, Y, LB, RB, BACK, START, LS, RS, DU, DD, DL, DR, GUIDE };
inline Tgt tgtFromStr(const std::string& s) {
    static const std::map<std::string, Tgt> m = {
        {"LX",Tgt::LX},{"LY",Tgt::LY},{"RX",Tgt::RX},{"RY",Tgt::RY},{"LT",Tgt::LT},{"RT",Tgt::RT},{"DPAD",Tgt::DPAD},
        {"A",Tgt::A},{"B",Tgt::B},{"X",Tgt::X},{"Y",Tgt::Y},{"LB",Tgt::LB},{"RB",Tgt::RB},{"BACK",Tgt::BACK},
        {"START",Tgt::START},{"LS",Tgt::LS},{"RS",Tgt::RS},{"DPAD_UP",Tgt::DU},{"DPAD_DOWN",Tgt::DD},
        {"DPAD_LEFT",Tgt::DL},{"DPAD_RIGHT",Tgt::DR},{"GUIDE",Tgt::GUIDE} };
    auto it = m.find(s); return it == m.end() ? Tgt::NONE : it->second;
}
inline USHORT tgtButtonMask(Tgt t) {
    switch (t) {
    case Tgt::A: return XUSB_GAMEPAD_A; case Tgt::B: return XUSB_GAMEPAD_B;
    case Tgt::X: return XUSB_GAMEPAD_X; case Tgt::Y: return XUSB_GAMEPAD_Y;
    case Tgt::LB: return XUSB_GAMEPAD_LEFT_SHOULDER; case Tgt::RB: return XUSB_GAMEPAD_RIGHT_SHOULDER;
    case Tgt::BACK: return XUSB_GAMEPAD_BACK; case Tgt::START: return XUSB_GAMEPAD_START;
    case Tgt::LS: return XUSB_GAMEPAD_LEFT_THUMB; case Tgt::RS: return XUSB_GAMEPAD_RIGHT_THUMB;
    case Tgt::DU: return XUSB_GAMEPAD_DPAD_UP; case Tgt::DD: return XUSB_GAMEPAD_DPAD_DOWN;
    case Tgt::DL: return XUSB_GAMEPAD_DPAD_LEFT; case Tgt::DR: return XUSB_GAMEPAD_DPAD_RIGHT;
    case Tgt::GUIDE: return XUSB_GAMEPAD_GUIDE;
    default: return 0;
    }
}
// vid/pid je Eintrag: welches PHYSISCHE Geraet diese Zuordnung erzeugt hat (leer =
// unbekannt/altes Profil vor diesem Fix -> Fallback "irgendein Geraet", altes Verhalten).
// Noetig, weil ein Profil bewusst mehrere Geraete gleichzeitig kombinieren darf (z.B.
// Lenkrad + separates Pedal-Set als zwei USB-HID-Geraete) - siehe g_vals/g_btns unten.
struct AxisMap { USHORT usagePage = 1, usage = 0; Tgt target = Tgt::NONE;
                 double deadzone = 0, curve = 1.0; bool invert = false;
                 double mn = 0, mx = 1; bool hasRange = false; std::string vid, pid; };
struct BtnMap  { USHORT usage = 0; Tgt target = Tgt::NONE; std::string vid, pid; };
struct AxisBtn { USHORT usagePage = 1, usage = 0; double threshold = 0.7; Tgt target = Tgt::NONE; std::string vid, pid; };
struct BtnAxis { USHORT usage = 0; Tgt target = Tgt::NONE; double value = 1.0; std::string vid, pid; };
struct Profile {
    std::string vid, pid;                 // leer = jedes Geraet
    std::vector<AxisMap> axes;
    std::vector<BtnMap>  buttons;
    std::vector<AxisBtn> axisToButton;
    std::vector<BtnAxis> buttonToAxis;
};
// vid/pid aus einem JSON-Eintrag lesen + normalisieren (Grosschreibung wie ueberall sonst).
inline void readDevTag(const json& j, std::string& vid, std::string& pid) {
    vid = j.value("vid", ""); pid = j.value("pid", "");
    for (auto& c : vid) c = (char)toupper((unsigned char)c);
    for (auto& c : pid) c = (char)toupper((unsigned char)c);
}
inline Profile parseProfile(const json& p) {
    Profile r;
    if (p.contains("device") && p["device"].is_object()) {
        r.vid = p["device"].value("vid", ""); r.pid = p["device"].value("pid", "");
        for (auto& c : r.vid) c = (char)toupper((unsigned char)c);
        for (auto& c : r.pid) c = (char)toupper((unsigned char)c);
    }
    for (auto& a : p.value("axes", json::array())) {
        AxisMap m; m.usagePage = (USHORT)a.value("usagePage", 1); m.usage = (USHORT)a.value("usage", 0);
        m.target = tgtFromStr(a.value("target", "")); m.deadzone = a.value("deadzone", 0.0);
        m.curve = a.value("curve", 1.0); m.invert = a.value("invert", false);
        readDevTag(a, m.vid, m.pid);
        if (a.contains("min") && a["min"].is_number() && a.contains("max") && a["max"].is_number()) {
            m.mn = a["min"].get<double>(); m.mx = a["max"].get<double>(); m.hasRange = (m.mx != m.mn);   // mn>mx erlaubt (invertierter Bereich, z.B. Bremse einer kombinierten Pedalachse)
        }
        if (m.target != Tgt::NONE && m.usage) r.axes.push_back(m);
    }
    for (auto& b : p.value("buttons", json::array())) {
        BtnMap m; m.usage = (USHORT)b.value("usage", 0); m.target = tgtFromStr(b.value("target", ""));
        readDevTag(b, m.vid, m.pid);
        if (m.target != Tgt::NONE && m.usage) r.buttons.push_back(m);
    }
    for (auto& a : p.value("axisToButton", json::array())) {
        AxisBtn m; m.usagePage = (USHORT)a.value("usagePage", 1); m.usage = (USHORT)a.value("usage", 0);
        m.threshold = a.value("threshold", 0.7); m.target = tgtFromStr(a.value("target", ""));
        readDevTag(a, m.vid, m.pid);
        if (m.target != Tgt::NONE && m.usage) r.axisToButton.push_back(m);
    }
    for (auto& b : p.value("buttonToAxis", json::array())) {
        BtnAxis m; m.usage = (USHORT)b.value("usage", 0); m.target = tgtFromStr(b.value("target", ""));
        m.value = b.value("value", 1.0);
        readDevTag(b, m.vid, m.pid);
        if (m.target != Tgt::NONE && m.usage) r.buttonToAxis.push_back(m);
    }
    return r;
}

// ---- Bridge-Zustand ------------------------------------------------------------
struct DevCache {                         // Preparsed-Data + Caps je Geraet (einmal geholt)
    std::vector<BYTE> prep;               // PHIDP_PREPARSED_DATA
    std::vector<HIDP_VALUE_CAPS>  vcaps;
    std::vector<HIDP_BUTTON_CAPS> bcaps;
    std::string vid, pid;
    bool bad = false;
    bool usesReportIds = false;           // irgendein Cap mit ReportID != 0 -> Multi-Report-Geraet
    std::set<BYTE> diagSeen;              // Diagnose: je Report-ID einmal die Struktur loggen
};
inline std::function<void(const std::string&)> g_diag;   // optionales Diagnose-Log (Shell haengt bcLogStream ein)
inline std::mutex g_mx;                   // schuetzt Profil + Monitor-Flags (Bridge-Thread vs. IPC)
inline Profile g_profile;
inline std::atomic<bool> g_running{ false };   // Thread lebt
inline std::atomic<bool> g_feeding{ false };   // ViGEm-Target aktiv (Pad sichtbar)
inline std::atomic<bool> g_monitor{ false };   // Live-Zustand an die UI pushen
inline std::atomic<bool> g_capture{ false };   // "Druecken zum Zuweisen"
inline std::atomic<LONG> g_vigemSlot{ -1 };    // XInput-UserIndex des virtuellen Pads (Hotkey-Echo vermeiden)
inline HWND g_bridgeWnd = nullptr;
inline std::thread g_thread;

// Laufende Eingabewerte, gescopt PRO URSPRUNGSGERAET (vid/pid) + usagePage/usage -
// normalisiert 0..1; Buttons als (Geraet, Usage)-Set. OHNE das Geraet im Key wuerden
// zwei gleichzeitig genutzte physische Geraete (z.B. Lenkrad + separates Pedal-Set,
// beide melden generische Achsen wie X/Y/Z unabhaengig voneinander) sich gegenseitig
// ueberschreiben, sobald sie zufaellig dieselbe usagePage/usage benutzen - real
// beobachtet: Lenken loeste Bremsen aus, weil die Lenkachse des Rads und die
// Bremsachse des Pedal-Sets denselben Key trafen.
struct AxisKey {
    std::string vid, pid; DWORD pu;   // pu = usagePage<<16 | usage
    bool operator<(const AxisKey& o) const {
        if (vid != o.vid) return vid < o.vid;
        if (pid != o.pid) return pid < o.pid;
        return pu < o.pu;
    }
};
struct BtnKey {
    std::string vid, pid; USHORT usage;
    bool operator<(const BtnKey& o) const {
        if (vid != o.vid) return vid < o.vid;
        if (pid != o.pid) return pid < o.pid;
        return usage < o.usage;
    }
};
inline std::map<AxisKey, double> g_vals;
inline std::set<BtnKey> g_btns;
inline std::map<AxisKey, double> g_capBase;      // Capture: Ausgangslage zum Vergleich
inline std::set<BtnKey> g_capBtns;
// Wert/Zustand einer Zuordnung nachschlagen: mit Geraete-Tag (neu zugewiesen) exakt
// dieses Geraet, ohne Tag (altes Profil vor diesem Fix) das erste passende - altes,
// unveraendertes Verhalten fuer bestehende Ein-Geraet-Profile.
inline const double* findAxisVal(const std::string& vid, const std::string& pid, USHORT usagePage, USHORT usage) {
    DWORD pu = ((DWORD)usagePage << 16) | usage;
    if (!vid.empty()) {
        auto it = g_vals.find(AxisKey{ vid, pid, pu });
        return it == g_vals.end() ? nullptr : &it->second;
    }
    for (auto& [k, v] : g_vals) if (k.pu == pu) return &v;
    return nullptr;
}
inline bool findBtnPressed(const std::string& vid, const std::string& pid, USHORT usage) {
    if (!vid.empty()) return g_btns.count(BtnKey{ vid, pid, usage }) > 0;
    for (auto& k : g_btns) if (k.usage == usage) return true;
    return false;
}

inline PVIGEM_CLIENT g_vigem = nullptr;
inline PVIGEM_TARGET g_target = nullptr;
inline XUSB_REPORT g_report{};

// ViGEmBus-Treiber installiert? (Dienst existiert - Abfrage ohne Adminrechte moeglich)
inline bool busInstalled() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"ViGEmBus", SERVICE_QUERY_STATUS);
    bool ok = svc != nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

// ---- HID-Parsing (WM_INPUT-Pfad) -------------------------------------------------
inline std::map<HANDLE, DevCache> g_devCache;   // nur vom Bridge-Thread benutzt
inline DevCache& devCache(HANDLE hDev) {
    auto& cache = g_devCache;
    auto it = cache.find(hDev);
    if (it != cache.end()) return it->second;
    DevCache& c = cache[hDev];
    UINT sz = 0; GetRawInputDeviceInfoW(hDev, RIDI_PREPARSEDDATA, nullptr, &sz);
    if (!sz) { c.bad = true; return c; }
    c.prep.resize(sz);
    if (GetRawInputDeviceInfoW(hDev, RIDI_PREPARSEDDATA, c.prep.data(), &sz) == (UINT)-1) { c.bad = true; return c; }
    auto pp = (PHIDP_PREPARSED_DATA)c.prep.data();
    HIDP_CAPS caps{};
    if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) { c.bad = true; return c; }
    if (caps.NumberInputValueCaps) {
        USHORT n = caps.NumberInputValueCaps; c.vcaps.resize(n);
        HidP_GetValueCaps(HidP_Input, c.vcaps.data(), &n, pp); c.vcaps.resize(n);
    }
    if (caps.NumberInputButtonCaps) {
        USHORT n = caps.NumberInputButtonCaps; c.bcaps.resize(n);
        HidP_GetButtonCaps(HidP_Input, c.bcaps.data(), &n, pp); c.bcaps.resize(n);
    }
    for (auto& vc : c.vcaps) if (vc.ReportID != 0) c.usesReportIds = true;
    for (auto& bc : c.bcaps) if (bc.ReportID != 0) c.usesReportIds = true;
    UINT nsz = 0; GetRawInputDeviceInfoW(hDev, RIDI_DEVICENAME, nullptr, &nsz);
    if (nsz) { std::wstring nm(nsz, 0); if (GetRawInputDeviceInfoW(hDev, RIDI_DEVICENAME, &nm[0], &nsz) != (UINT)-1) vidPidFromName(nm, c.vid, c.pid); }
    return c;
}
inline bool deviceMatches(const DevCache& c, const Profile& p) {
    if (p.vid.empty() && p.pid.empty()) return true;   // Profil ohne Geraetebindung = alles
    return c.vid == p.vid && c.pid == p.pid;
}
// Achsenwert 0..1 mit Profil-Verarbeitung -> -1..1 (Sticks) bzw. 0..1 (Trigger)
inline double shapeAxis(double v01, const AxisMap& m, bool trigger) {
    if (m.hasRange) v01 = (v01 - m.mn) / (m.mx - m.mn);   // Geraete-Macken (kombinierte Pedale) ausblenden
    v01 = v01 < 0 ? 0 : v01 > 1 ? 1 : v01;
    if (m.invert) v01 = 1.0 - v01;
    if (trigger) {
        double v = v01 <= m.deadzone ? 0 : (v01 - m.deadzone) / (1.0 - m.deadzone);
        return pow(v, m.curve <= 0 ? 1.0 : m.curve);
    }
    double a = v01 * 2.0 - 1.0;
    double ab = fabs(a);
    if (ab <= m.deadzone) return 0;
    ab = (ab - m.deadzone) / (1.0 - m.deadzone);
    ab = pow(ab, m.curve <= 0 ? 1.0 : m.curve);
    return a < 0 ? -ab : ab;
}
inline void setAxisTarget(XUSB_REPORT& r, Tgt t, double shaped) {
    auto toS = [](double a) { return (SHORT)(a < -1 ? -32767 : a > 1 ? 32767 : a * 32767); };
    auto toB = [](double v) { return (BYTE)(v < 0 ? 0 : v > 1 ? 255 : v * 255); };
    switch (t) {
    case Tgt::LX: r.sThumbLX = toS(shaped); break;
    case Tgt::LY: r.sThumbLY = toS(-shaped); break;   // HID-Y ist "unten=positiv", XInput "oben=positiv"
    case Tgt::RX: r.sThumbRX = toS(shaped); break;
    case Tgt::RY: r.sThumbRY = toS(-shaped); break;
    case Tgt::LT: r.bLeftTrigger = toB(shaped); break;
    case Tgt::RT: r.bRightTrigger = toB(shaped); break;
    default: break;
    }
}
// Kompletten XUSB-Report aus dem aktuellen Eingabe-Zustand bauen (Mapping anwenden).
inline void rebuildReport() {
    XUSB_REPORT r{};
    for (auto& m : g_profile.axes) {
        const double* pv = findAxisVal(m.vid, m.pid, m.usagePage, m.usage); if (!pv) continue;
        if (m.target == Tgt::DPAD) {                    // Hat-Switch (Usage 0x39) -> DPad
            // Der Parser legt Hats als Richtungsindex/8 ab; neutral (raw ausserhalb
            // LogicalMin..Max) = -1. 8 Richtungen im Uhrzeigersinn ab "oben".
            double v = *pv;
            if (v < 0) continue;                        // neutral
            int dir = (int)floor(v * 8.0 + 0.5) % 8;
            static const USHORT DIRS[8] = {
                XUSB_GAMEPAD_DPAD_UP, XUSB_GAMEPAD_DPAD_UP | XUSB_GAMEPAD_DPAD_RIGHT,
                XUSB_GAMEPAD_DPAD_RIGHT, XUSB_GAMEPAD_DPAD_DOWN | XUSB_GAMEPAD_DPAD_RIGHT,
                XUSB_GAMEPAD_DPAD_DOWN, XUSB_GAMEPAD_DPAD_DOWN | XUSB_GAMEPAD_DPAD_LEFT,
                XUSB_GAMEPAD_DPAD_LEFT, XUSB_GAMEPAD_DPAD_UP | XUSB_GAMEPAD_DPAD_LEFT };
            r.wButtons |= DIRS[dir];
            continue;
        }
        bool trig = m.target == Tgt::LT || m.target == Tgt::RT;
        setAxisTarget(r, m.target, shapeAxis(*pv, m, trig));
    }
    for (auto& m : g_profile.buttons)
        if (findBtnPressed(m.vid, m.pid, m.usage)) r.wButtons |= tgtButtonMask(m.target);
    for (auto& m : g_profile.axisToButton) {
        const double* pv = findAxisVal(m.vid, m.pid, m.usagePage, m.usage);
        if (pv && *pv >= m.threshold) r.wButtons |= tgtButtonMask(m.target);
    }
    for (auto& m : g_profile.buttonToAxis)
        if (findBtnPressed(m.vid, m.pid, m.usage)) setAxisTarget(r, m.target, m.value);
    g_report = r;
    if (g_feeding.load() && g_vigem && g_target) vigem_target_x360_update(g_vigem, g_target, r);
}

// Live-Zustand an die UI (gedrosselt ~10 Hz, nur bei offenem Monitor/Capture).
inline ULONGLONG g_lastPush = 0;
inline void pushMonitor(bool force = false) {
    if (!g_push || (!g_monitor.load() && !g_capture.load())) return;
    ULONGLONG now = GetTickCount64();
    if (!force && now - g_lastPush < 100) return;
    g_lastPush = now;
    json axes = json::array();
    for (auto& [key, v] : g_vals)
        axes.push_back({ {"usagePage", (key.pu >> 16) & 0xFFFF}, {"usage", key.pu & 0xFFFF}, {"value", v} });
    json btns = json::array();
    for (auto& k : g_btns) btns.push_back(k.usage);
    g_push("input-bridge-state", {
        {"axes", axes}, {"buttons", btns},
        {"out", { {"lx", g_report.sThumbLX}, {"ly", g_report.sThumbLY}, {"rx", g_report.sThumbRX},
                  {"ry", g_report.sThumbRY}, {"lt", g_report.bLeftTrigger}, {"rt", g_report.bRightTrigger},
                  {"buttons", g_report.wButtons} }},
        {"feeding", g_feeding.load()} });
}
// Capture ("Druecken zum Zuweisen"): erste deutliche Aenderung gegenueber der
// Ausgangslage melden - neu gedrueckter Knopf gewinnt vor bewegter Achse.
inline void captureCheck() {
    if (!g_capture.load() || !g_push) return;
    for (auto& k : g_btns) {
        if (g_capBtns.count(k)) continue;
        g_capture = false;
        // vid/pid mit ausliefern - die UI taggt die neue Zuordnung damit auf GENAU dieses
        // Geraet, statt (wie vor diesem Fix) blind auf jedes Geraet mit derselben Usage zu passen.
        g_push("input-bridge-captured", { {"type", "button"}, {"usage", k.usage}, {"vid", k.vid}, {"pid", k.pid} });
        return;
    }
    for (auto& [key, v] : g_vals) {
        auto it = g_capBase.find(key);
        double base = it == g_capBase.end() ? v : it->second;
        if (it == g_capBase.end()) { g_capBase[key] = v; continue; }   // Achse erst jetzt gesehen: Basis merken
        if ((key.pu & 0xFFFF) == 0x39) continue;          // Hat zaehlt als DPad, nicht als Achse zuweisen
        if (fabs(v - base) < 0.30) continue;           // 30 % Weg = bewusste Bewegung
        g_capture = false;
        // base = Ruhelage (0..1), val = Auslenkung -> UI kann Trigger auto-kalibrieren
        // (Deadzone/Invert so, dass die Ruhelage = 0 ist, egal wo die Achse ruht).
        g_push("input-bridge-captured", { {"type", "axis"}, {"usagePage", (key.pu >> 16) & 0xFFFF}, {"usage", key.pu & 0xFFFF}, {"base", base}, {"val", v}, {"vid", key.vid}, {"pid", key.pid} });
        return;
    }
}
// WM_INPUT: HID-Report parsen -> Zustand aktualisieren -> Report neu bauen.
inline void onRawInput(HRAWINPUT hRaw) {
    UINT sz = 0; GetRawInputData(hRaw, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
    if (!sz) return;
    static std::vector<BYTE> buf; buf.resize(sz);
    if (GetRawInputData(hRaw, RID_INPUT, buf.data(), &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    auto* ri = (RAWINPUT*)buf.data();
    if (ri->header.dwType != RIM_TYPEHID) return;
    DevCache& c = devCache(ri->header.hDevice);
    if (c.bad) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!deviceMatches(c, g_profile) && !g_monitor.load() && !g_capture.load()) return;
    auto pp = (PHIDP_PREPARSED_DATA)c.prep.data();
    PCHAR report = (PCHAR)ri->data.hid.bRawData;
    ULONG repLen = ri->data.hid.dwSizeHid;
    // Achsen (Value-Caps): normalisiert 0..1; Hats (0x39) als Richtungsindex/8, neutral=-1.
    for (auto& vc : c.vcaps) {
        USHORT uMin = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
        USHORT uMax = vc.IsRange ? vc.Range.UsageMax : vc.NotRange.Usage;
        for (USHORT u = uMin; u <= uMax; ++u) {
            ULONG raw = 0;
            if (HidP_GetUsageValue(HidP_Input, vc.UsagePage, 0, u, &raw, pp, report, repLen) != HIDP_STATUS_SUCCESS) continue;
            LONG lmin = vc.LogicalMin, lmax = vc.LogicalMax;
            AxisKey key{ c.vid, c.pid, ((DWORD)vc.UsagePage << 16) | u };
            if (u == 0x39 && vc.UsagePage == 0x01) {
                LONG v = (LONG)raw;
                LONG count = lmax - lmin + 1;
                if (count < 2 || v < lmin || v > lmax) { g_vals[key] = -1; continue; }   // neutral
                double dir8 = (double)(v - lmin) * (8.0 / count);                        // 4er-Hats auf 8 skalieren
                g_vals[key] = dir8 / 8.0;
            } else {
                // Manche Geraete melden signed-Werte; LogicalMin>LogicalMax kommt vor -> absichern.
                if (lmax <= lmin) { g_vals[key] = 0; continue; }
                LONG v = (LONG)raw;
                if (lmin < 0 && v > lmax) v = (LONG)(int16_t)raw;   // sign-extend fuer 16-bit-signed-Reports
                double n = (double)(v - lmin) / (double)(lmax - lmin);
                g_vals[key] = n < 0 ? 0 : n > 1 ? 1 : n;
            }
        }
    }
    // Buttons: NUR die Zustaende der Knoepfe ersetzen, die im AKTUELLEN Report enthalten
    // sind (Multi-Report-Fix, 2. Stufe - real gemeldet: "Gas bricht beim Lenken ab, nach
    // Stufe 1 besser, aber nicht weg"). Stufe 1 verhinderte, dass reine ACHSEN-Reports die
    // Knoepfe loeschen. Verbleibende Luecke: verteilt ein Geraet seine Knoepfe auf MEHRERE
    // Report-IDs, ersetzte ein Knopf-Report weiterhin den KOMPLETTEN Zustand - gehaltene
    // Knoepfe aus dem jeweils ANDEREN Report gingen verloren. Jetzt wird per Report-ID
    // exakt abgeglichen: erst die zu DIESEM Report gehoerenden Knopf-Bereiche austragen,
    // dann die in DIESEM Report gedrueckten wieder eintragen - fremde Reports unberuehrt.
    {
        BYTE repId = c.usesReportIds ? (BYTE)report[0] : 0;
        std::set<USHORT> pages; bool any = false;
        for (auto& bc : c.bcaps) {
            if (bc.ReportID != repId) continue;              // Knopf-Cap gehoert zu einem anderen Report
            USHORT uMin = bc.IsRange ? bc.Range.UsageMin : bc.NotRange.Usage;
            USHORT uMax = bc.IsRange ? bc.Range.UsageMax : bc.NotRange.Usage;
            for (USHORT u = uMin; u <= uMax; ++u) g_btns.erase(BtnKey{ c.vid, c.pid, u });
            pages.insert(bc.UsagePage); any = true;
        }
        if (any) for (USHORT page : pages) {
            ULONG n = HidP_MaxUsageListLength(HidP_Input, page, pp);
            if (!n) continue;
            static std::vector<USAGE> usages; usages.resize(n);
            if (HidP_GetUsages(HidP_Input, page, 0, usages.data(), &n, pp, report, repLen) != HIDP_STATUS_SUCCESS) continue;
            for (ULONG i = 0; i < n; ++i) g_btns.insert(BtnKey{ c.vid, c.pid, usages[i] });
        }
        // Diagnose (nur bei offenem Eingabe-Tab, 1x je Report-ID): Struktur des Geraets
        // sichtbar machen - welche Reports existieren, was tragen sie. Landet im Stream-Log.
        if (g_diag && g_monitor.load() && !c.diagSeen.count(repId)) {
            c.diagSeen.insert(repId);
            std::string axes;
            for (auto& vc : c.vcaps) {
                if (vc.ReportID != repId) continue;
                USHORT uMin = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
                USHORT uMax = vc.IsRange ? vc.Range.UsageMax : vc.NotRange.Usage;
                for (USHORT u = uMin; u <= uMax; ++u) { char b[16]; snprintf(b, sizeof(b), "%s0x%02X", axes.empty() ? "" : ",", u); axes += b; }
            }
            std::string btns;
            for (auto& bc : c.bcaps) {
                if (bc.ReportID != repId) continue;
                USHORT uMin = bc.IsRange ? bc.Range.UsageMin : bc.NotRange.Usage;
                USHORT uMax = bc.IsRange ? bc.Range.UsageMax : bc.NotRange.Usage;
                char b[24]; snprintf(b, sizeof(b), "%s%u-%u", btns.empty() ? "" : ",", uMin, uMax); btns += b;
            }
            g_diag("bridge-diag: dev " + c.vid + ":" + c.pid + " report=" + std::to_string(repId) +
                   " len=" + std::to_string((int)repLen) + " achsen=[" + axes + "] knoepfe=[" + btns + "]" +
                   (c.usesReportIds ? " (multi-report)" : " (single-report)"));
        }
    }
    if (deviceMatches(c, g_profile)) rebuildReport();
    captureCheck();
    pushMonitor();
}

// ---- Bridge-Thread (message-only Window + eigene Pump) ---------------------------
#define LUBRIDGE_WM_STOP (WM_APP + 40)
inline LRESULT CALLBACK bridgeWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INPUT: onRawInput((HRAWINPUT)l); return 0;
    case WM_INPUT_DEVICE_CHANGE:   // Geraet weg/neu: Cache raeumen (Windows recycelt HANDLEs)
        if (w == GIDC_REMOVAL) g_devCache.erase((HANDLE)l);
        return 0;
    case WM_TIMER:   // Keepalive: Report bei Funkstille frisch halten (ViGEm-Watchdog/Idle)
        if (g_feeding.load() && g_vigem && g_target) vigem_target_x360_update(g_vigem, g_target, g_report);
        return 0;
    case LUBRIDGE_WM_STOP: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
inline void bridgeThreadProc() {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = bridgeWndProc; wc.lpszClassName = L"LumoraInputBridge";
        wc.hInstance = GetModuleHandleW(nullptr);
        RegisterClassW(&wc); reg = true;
    }
    HWND h = CreateWindowExW(0, L"LumoraInputBridge", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h) { g_running = false; return; }
    g_bridgeWnd = h;
    // RawInput fuer Joystick/Gamepad/Multi-Axis; INPUTSINK = auch ohne Fokus (Spiel im Vordergrund).
    RAWINPUTDEVICE rid[3] = {
        { 0x01, 0x04, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, h },
        { 0x01, 0x05, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, h },
        { 0x01, 0x08, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, h },
    };
    RegisterRawInputDevices(rid, 3, sizeof(RAWINPUTDEVICE));
    SetTimer(h, 1, 100, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    KillTimer(h, 1);
    RAWINPUTDEVICE off[3] = { {0x01,0x04,RIDEV_REMOVE,nullptr},{0x01,0x05,RIDEV_REMOVE,nullptr},{0x01,0x08,RIDEV_REMOVE,nullptr} };
    RegisterRawInputDevices(off, 3, sizeof(RAWINPUTDEVICE));
    g_bridgeWnd = nullptr;
    g_running = false;
}

// ---- ViGEm-Target an/aus (Start kommt vom IPC-Worker, Stop auch vom UI-Thread
// (Spielende-Hook) -> Lifecycle serialisieren) ----------------------------------------
inline std::mutex g_vigemMx;
inline bool vigemStart() {
    std::lock_guard<std::mutex> lk(g_vigemMx);
    if (g_feeding.load()) return true;
    if (!g_vigem) {
        g_vigem = vigem_alloc();
        if (!g_vigem || !VIGEM_SUCCESS(vigem_connect(g_vigem))) {
            if (g_vigem) { vigem_free(g_vigem); g_vigem = nullptr; }
            return false;
        }
    }
    g_target = vigem_target_x360_alloc();
    if (!g_target || !VIGEM_SUCCESS(vigem_target_add(g_vigem, g_target))) {
        if (g_target) { vigem_target_free(g_target); g_target = nullptr; }
        return false;
    }
    ULONG idx = 0;
    g_vigemSlot = VIGEM_SUCCESS(vigem_target_x360_get_user_index(g_vigem, g_target, &idx)) ? (LONG)idx : -1;
    XUSB_REPORT_INIT(&g_report);
    vigem_target_x360_update(g_vigem, g_target, g_report);
    g_feeding = true;
    return true;
}
inline void vigemStop() {
    std::lock_guard<std::mutex> lk(g_vigemMx);
    g_feeding = false;
    g_vigemSlot = -1;
    if (g_vigem && g_target) { vigem_target_remove(g_vigem, g_target); vigem_target_free(g_target); }
    g_target = nullptr;
}

// ---- Oeffentliche API (vom UI-/IPC-Thread aufgerufen) --------------------------------
// Bridge-Thread hochfahren (idempotent). Laeuft auch ohne aktives Pad (Monitor/Capture).
inline void ensureThread() {
    if (g_running.load()) return;
    g_running = true;
    if (g_thread.joinable()) g_thread.join();
    g_thread = std::thread(bridgeThreadProc);
}
// Ist das Quellgeraet des Profils (Lenkrad/Joystick) ueberhaupt angeschlossen?
// Ohne diese Pruefung legte die automatische Aktivierung bei JEDEM Spielstart ein
// virtuelles Xbox-Pad an - auch wenn nichts da war, was es haette abbilden koennen.
// Ein zusaetzliches XInput-Geraet neben einem echten Controller ist nicht gratis:
// Windows verteilt die vier XInput-Plaetze neu, und der echte Controller kann dabei
// herausfallen. Kein Quellgeraet -> kein virtuelles Pad.
inline bool profileDevicePresent(const Profile& p) {
    if (p.vid.empty() && p.pid.empty()) return false;   // ohne Geraetebindung nicht automatisch aktivieren
    for (auto& d : listDevices()) {
        std::string v = d.value("vid", ""), i = d.value("pid", "");
        for (auto& c : v) c = (char)toupper((unsigned char)c);
        for (auto& c : i) c = (char)toupper((unsigned char)c);
        if (v == p.vid && i == p.pid) return true;
    }
    return false;
}
// Pad aktivieren: Profil setzen + ViGEm-Target anlegen. reason nur fuer den UI-Push.
// requireDevice=true (automatische Aktivierung): nur mit angeschlossenem Quellgeraet.
// Der manuelle Weg aus der UI bleibt ohne diese Huerde (Einrichten/Testen).
inline bool start(const json& profile, const std::string& reason, bool requireDevice = false) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_profile = parseProfile(profile);
    }
    if (requireDevice) {
        Profile snap; { std::lock_guard<std::mutex> lk(g_mx); snap = g_profile; }
        if (!profileDevicePresent(snap)) {
            if (g_push) g_push("input-bridge-active", { {"active", false}, {"reason", "quellgeraet-fehlt"} });
            return false;
        }
    }
    ensureThread();
    bool ok = vigemStart();
    if (g_push) g_push("input-bridge-active", { {"active", ok}, {"reason", ok ? reason : "vigem-fehler"} });
    return ok;
}
inline void stop(const std::string& reason) {
    if (!g_feeding.load()) return;
    vigemStop();
    if (g_push) g_push("input-bridge-active", { {"active", false}, {"reason", reason} });
}
// Monitor/Capture brauchen den Thread, aber kein aktives Pad.
inline void setMonitor(bool on) { g_monitor = on; if (on) ensureThread(); }
inline void setCapture(bool on) {
    if (on) {
        ensureThread();
        std::lock_guard<std::mutex> lk(g_mx);
        g_capBase = g_vals; g_capBtns = g_btns;
    }
    g_capture = on;
}
inline json status() {
    return { {"active", g_feeding.load()}, {"busInstalled", busInstalled()}, {"slot", g_vigemSlot.load()} };
}
// Beim App-Ende: Pad weg, Thread beenden (ViGEm-Verbindung sauber schliessen).
inline void shutdown() {
    vigemStop();
    if (g_vigem) { vigem_disconnect(g_vigem); vigem_free(g_vigem); g_vigem = nullptr; }
    if (g_bridgeWnd) PostMessageW(g_bridgeWnd, LUBRIDGE_WM_STOP, 0, 0);
    if (g_thread.joinable()) g_thread.join();
    g_running = false;
}

} // namespace lubridge
