@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem  Lumora AMD-GPU-Test RUNDE 2 (fuer AMD-Systeme, z.B. Ryzen 6800H / 680M)
rem  Runde 1 zeigte: vpp_amf-Kurzform scheitert mit AMFConverter-Init error 10
rem  (= AMF_NOT_SUPPORTED, vermutlich fehlende Device-Verknuepfung).
rem  Diese Runde testet die OFFIZIELLE AMD-Wiki-Form mit explizitem AMF-Device
rem  in drei Varianten. Laufzeit: ~20 Sekunden.
rem  Ergebnis: amf-gpu-test2-ergebnis.txt (bitte komplett zurueckschicken).
rem ===========================================================================

set "FF=%LOCALAPPDATA%\Programs\lumora\resources\bin\ffmpeg.exe"
if not exist "%FF%" set "FF=%~dp0..\bin\ffmpeg.exe"
if not exist "%FF%" (
  echo FEHLER: ffmpeg.exe nicht gefunden.
  pause
  exit /b 1
)

set "OUT=%~dp0amf-gpu-test2-ergebnis.txt"
echo Lumora AMD-GPU-Test Runde 2 %DATE% %TIME% > "%OUT%"
echo FFmpeg: %FF% >> "%OUT%"
echo. >> "%OUT%"

echo.
echo === Test A: AMD-Wiki-Form (explizites AMF-Device, filter_hw=d3d11) ===
echo ============================================================ >> "%OUT%"
echo TEST A: -init_hw_device d3d11va=dx11 + amf=amf0@dx11, filter_hw_device dx11, vpp_amf scale+nv12 >> "%OUT%"
"%FF%" -hide_banner -loglevel warning -stats -y -init_hw_device d3d11va=dx11 -init_hw_device amf=amf0@dx11 -filter_hw_device dx11 -f lavfi -i "ddagrab=output_idx=0:framerate=60" -vf "vpp_amf=w=1920:h=1080:format=nv12" -c:v h264_amf -usage lowlatency -rc cbr -b:v 8000k -maxrate 8000k -bufsize 4000k -g 120 -bf 0 -t 5 -f null - >> "%OUT%" 2>&1
if !errorlevel! equ 0 (echo ERGEBNIS TEST A: OK >> "%OUT%" & echo   -^> OK) else (echo ERGEBNIS TEST A: FEHLGESCHLAGEN ^(Exit !errorlevel!^) >> "%OUT%" & echo   -^> FEHLGESCHLAGEN)
echo. >> "%OUT%"

echo === Test B: wie A, aber filter_hw_device amf0 ===
echo ============================================================ >> "%OUT%"
echo TEST B: filter_hw_device amf0 statt dx11 >> "%OUT%"
"%FF%" -hide_banner -loglevel warning -stats -y -init_hw_device d3d11va=dx11 -init_hw_device amf=amf0@dx11 -filter_hw_device amf0 -f lavfi -i "ddagrab=output_idx=0:framerate=60" -vf "vpp_amf=w=1920:h=1080:format=nv12" -c:v h264_amf -usage lowlatency -rc cbr -b:v 8000k -maxrate 8000k -bufsize 4000k -g 120 -bf 0 -t 5 -f null - >> "%OUT%" 2>&1
if !errorlevel! equ 0 (echo ERGEBNIS TEST B: OK >> "%OUT%" & echo   -^> OK) else (echo ERGEBNIS TEST B: FEHLGESCHLAGEN ^(Exit !errorlevel!^) >> "%OUT%" & echo   -^> FEHLGESCHLAGEN)
echo. >> "%OUT%"

echo === Test C: hwmap=derive_device=amf im Filtergraphen ===
echo ============================================================ >> "%OUT%"
echo TEST C: ddagrab - hwmap=derive_device=amf - vpp_amf scale+nv12 >> "%OUT%"
"%FF%" -hide_banner -loglevel warning -stats -y -f lavfi -i "ddagrab=output_idx=0:framerate=60" -vf "hwmap=derive_device=amf,vpp_amf=w=1920:h=1080:format=nv12" -c:v h264_amf -usage lowlatency -rc cbr -b:v 8000k -maxrate 8000k -bufsize 4000k -g 120 -bf 0 -t 5 -f null - >> "%OUT%" 2>&1
if !errorlevel! equ 0 (echo ERGEBNIS TEST C: OK >> "%OUT%" & echo   -^> OK) else (echo ERGEBNIS TEST C: FEHLGESCHLAGEN ^(Exit !errorlevel!^) >> "%OUT%" & echo   -^> FEHLGESCHLAGEN)
echo. >> "%OUT%"

echo.
echo Fertig. Ergebnis-Datei: %OUT%
echo Bitte die komplette Datei zurueckschicken.
pause
