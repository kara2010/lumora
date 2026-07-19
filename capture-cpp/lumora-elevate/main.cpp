// lumora-elevate.exe: winziger, SIGNIERTER Rechte-Helfer. Wird von Lumora per
// "Start-Process -Verb RunAs" gestartet -> der UAC-Dialog zeigt DIESE Lumora-EXE
// (Logo + verifizierter Herausgeber) statt "Windows PowerShell". Fuehrt mit den
// erhaltenen Adminrechten den uebergebenen Befehl aus:
//   --ps-encoded <base64>  ->  powershell -NoProfile -EncodedCommand <base64>
// GUI-Subsystem (kein Konsolenfenster). KEIN requireAdministrator-Manifest noetig:
// die Elevation macht bereits das aufrufende "-Verb RunAs".
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3 && wcscmp(argv[1], L"--ps-encoded") == 0) {
        std::wstring cmd = L"powershell.exe -NoProfile -WindowStyle Hidden -EncodedCommand ";
        cmd += argv[2];
        STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return 1;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return (int)ec;
    }
    return 2;   // falscher Aufruf
}
