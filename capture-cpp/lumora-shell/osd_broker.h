// OSD-Broker-Modi der Shell (1:1 aus main.js runFpsBroker/runSenseBroker).
// Die geplanten Aufgaben LumoraOSD-FPS / LumoraOSD-Sensors starten
// "lumora_shell.exe --fps-broker" bzw. "--sensor-broker" elevated; der Broker
// sammelt die Werte und schreibt sie ins Shared Memory, das die UI-Instanz liest.
// Getrennte Schreibbereiche: Broker @0 (magic..pid), App @24 (appTick,wanted).
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <map>
#include <vector>
#include <set>
#include <algorithm>

namespace lubroker {

static const uint32_t FPS_MAGIC = 0x4C4F5344;   // 'LOSD'
// PresentMon-Prozesse, die kein Spiel sind (1:1 aus main.js PM_IGNORE)
static bool pmIgnore(const std::string& app) {
    static const std::set<std::string> ig = { "dwm.exe","explorer.exe","lumora.exe","lumora_shell.exe","presentmon.exe",
        "searchhost.exe","textinputhost.exe","shellexperiencehost.exe","startmenuexperiencehost.exe","applicationframehost.exe" };
    std::string a = app; for (auto& c : a) c = (char)tolower((unsigned char)c);
    return ig.count(a) > 0;
}

// PresentMon.exe suchen: bin/ neben der Shell, sonst App-Ordner, sonst Projekt-Root (Dev).
inline std::wstring findPresentMon(const std::wstring& binDir) {
    for (const std::wstring& d : { binDir, binDir + L"\\..", binDir + L"\\..\\..\\.." }) {
        std::wstring p = d + L"\\PresentMon.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return binDir + L"\\PresentMon.exe";
}
// Ein einzelner FPS-Broker-Lauf: PresentMon starten, CSV parsen, FPS ins SHM.
inline int runFpsBroker(const std::wstring& binDir) {
    HANDLE shm = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, "Local\\LumoraOSDFps");
    if (!shm) return 1;
    uint32_t* mem = (uint32_t*)MapViewOfFile(shm, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!mem) { CloseHandle(shm); return 1; }
    // Laeuft schon ein Broker (frischer brokerTick)? Dann sofort raus (nur EINER darf leben).
    if (mem[0] == FPS_MAGIC && mem[1] && (uint32_t)(GetTickCount() - mem[1]) < 2500) { UnmapViewOfFile(mem); CloseHandle(shm); return 0; }

    // PresentMon mit stdout-Pipe starten (Args 1:1 aus main.js).
    SECURITY_ATTRIBUTES sa{ sizeof(sa) }; sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr; CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr;
    std::wstring cmd = L"\"" + findPresentMon(binDir) + L"\" --output_stdout --session_name LumoraOSD --stop_existing_session --no_console_stats --v1_metrics";
    PROCESS_INFORMATION pi{};
    std::wstring mcmd = cmd;
    if (!CreateProcessW(nullptr, &mcmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); UnmapViewOfFile(mem); CloseHandle(shm); return 1;
    }
    CloseHandle(wr);

    // stdout-Leser-Thread: CSV -> Frames je PID (times/frametimes, max 400).
    struct Frames { std::vector<double> times, ft; };
    static std::map<std::string, Frames> frames;   // pid -> Frames
    static std::mutex mx;
    int colApp = -1, colPid = -1, colT = -1, colFt = -1;
    std::thread reader([&, rd]() {
        std::string acc; char buf[8192]; DWORD got = 0; bool haveCols = false;
        int cApp = -1, cPid = -1, cT = -1, cFt = -1;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) {
            acc.append(buf, got);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl); acc.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                std::vector<std::string> p; size_t s = 0, c;
                while ((c = line.find(',', s)) != std::string::npos) { p.push_back(line.substr(s, c - s)); s = c + 1; }
                p.push_back(line.substr(s));
                if (!haveCols) {   // Header: Spaltenindizes bestimmen
                    for (size_t i = 0; i < p.size(); ++i) { std::string h = p[i]; for (auto& ch : h) ch = (char)tolower((unsigned char)ch);
                        while (!h.empty() && (h.front() == ' ')) h.erase(0, 1); while (!h.empty() && h.back() == ' ') h.pop_back();
                        if (h == "application") cApp = (int)i; else if (h == "processid") cPid = (int)i;
                        else if (h == "timeinseconds") cT = (int)i; else if (h == "msbetweenpresents") cFt = (int)i; }
                    haveCols = true; continue;
                }
                if (cPid < 0 || cT < 0 || cFt < 0 || (int)p.size() <= (std::max)({ cPid, cT, cFt })) continue;
                std::string pid = p[cPid], app = cApp >= 0 ? p[cApp] : "";
                while (!app.empty() && app.front() == ' ') app.erase(0, 1);
                double t = atof(p[cT].c_str()), ft = atof(p[cFt].c_str());
                if (pid.empty() || t == 0 || pmIgnore(app)) continue;
                std::lock_guard<std::mutex> lk(mx);
                auto& e = frames[pid];
                e.times.push_back(t); e.ft.push_back(ft);
                if (e.times.size() > 400) { e.times.erase(e.times.begin()); e.ft.erase(e.ft.begin()); }
            }
        }
        CloseHandle(rd);
    });

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
        DWORD ec = 0;   // PresentMon gestorben? dann raus
        if (GetExitCodeProcess(pi.hProcess, &ec) && ec != STILL_ACTIVE) break;
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    reader.join();
    UnmapViewOfFile(mem); CloseHandle(shm);
    return 0;
}

} // namespace lubroker
