@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem  Lumora AMD-Encoder-Test RUNDE 3 (Abschluss): Encoder-Flag-Vergleich
rem  vpp_amf ist beerdigt (Runde 1+2: AMF_NOT_SUPPORTED, dreifach belegt).
rem  Diese Runde vergleicht auf der FUNKTIONIERENDEN CPU-Kette die heutigen
rem  Lumora-Encoder-Flags gegen Sunshine-Praxiswerte. Je 10 Sekunden.
rem  Entscheidend sind die fps=/speed=-Werte am Ende jedes Tests.
rem  Ergebnis: amf-gpu-test3-ergebnis.txt (bitte komplett zurueckschicken).
rem ===========================================================================

set "FF=%LOCALAPPDATA%\Programs\lumora\resources\bin\ffmpeg.exe"
if not exist "%FF%" set "FF=%~dp0..\bin\ffmpeg.exe"
if not exist "%FF%" (
  echo FEHLER: ffmpeg.exe nicht gefunden.
  pause
  exit /b 1
)

set "OUT=%~dp0amf-gpu-test3-ergebnis.txt"
set "VF=hwdownload,format=bgra,scale=-2:1080:flags=bicubic,format=yuv420p"
echo Lumora AMD-Encoder-Test Runde 3 %DATE% %TIME% > "%OUT%"
echo FFmpeg: %FF% >> "%OUT%"
echo. >> "%OUT%"

echo.
echo === Test I: HEUTIGE Lumora-Flags (Referenz) - 10s ===
echo ============================================================ >> "%OUT%"
echo TEST I: usage=lowlatency (heutiger Lumora-Stand) >> "%OUT%"
"%FF%" -hide_banner -loglevel warning -stats -y -f lavfi -i "ddagrab=output_idx=0:framerate=60" -vf "%VF%" -c:v h264_amf -usage lowlatency -rc cbr -b:v 8000k -maxrate 8000k -bufsize 4000k -g 120 -bf 0 -forced_idr 1 -force_key_frames "expr:gte(t,n_forced*1)" -t 10 -f null - >> "%OUT%" 2>&1
if !errorlevel! equ 0 (echo ERGEBNIS TEST I: OK >> "%OUT%" & echo   -^> OK) else (echo ERGEBNIS TEST I: FEHLGESCHLAGEN ^(Exit !errorlevel!^) >> "%OUT%" & echo   -^> FEHLGESCHLAGEN)
echo. >> "%OUT%"

echo === Test II: Sunshine-Tuning (ultralowlatency + balanced) - 10s ===
echo ============================================================ >> "%OUT%"
echo TEST II: usage=ultralowlatency quality=balanced preanalysis=0 vbaq=1 latency=1 async_depth=1 >> "%OUT%"
"%FF%" -hide_banner -loglevel warning -stats -y -f lavfi -i "ddagrab=output_idx=0:framerate=60" -vf "%VF%" -c:v h264_amf -usage ultralowlatency -quality balanced -preanalysis 0 -vbaq 1 -latency 1 -async_depth 1 -rc cbr -b:v 8000k -maxrate 8000k -bufsize 4000k -g 120 -bf 0 -forced_idr 1 -force_key_frames "expr:gte(t,n_forced*1)" -t 10 -f null - >> "%OUT%" 2>&1
if !errorlevel! equ 0 (echo ERGEBNIS TEST II: OK >> "%OUT%" & echo   -^> OK) else (echo ERGEBNIS TEST II: FEHLGESCHLAGEN ^(Exit !errorlevel!^) >> "%OUT%" & echo   -^> FEHLGESCHLAGEN)
echo. >> "%OUT%"

echo === Test III: Sunshine-Tuning mit quality=speed - 10s ===
echo ============================================================ >> "%OUT%"
echo TEST III: wie II, aber quality=speed >> "%OUT%"
"%FF%" -hide_banner -loglevel warning -stats -y -f lavfi -i "ddagrab=output_idx=0:framerate=60" -vf "%VF%" -c:v h264_amf -usage ultralowlatency -quality speed -preanalysis 0 -vbaq 1 -latency 1 -async_depth 1 -rc cbr -b:v 8000k -maxrate 8000k -bufsize 4000k -g 120 -bf 0 -forced_idr 1 -force_key_frames "expr:gte(t,n_forced*1)" -t 10 -f null - >> "%OUT%" 2>&1
if !errorlevel! equ 0 (echo ERGEBNIS TEST III: OK >> "%OUT%" & echo   -^> OK) else (echo ERGEBNIS TEST III: FEHLGESCHLAGEN ^(Exit !errorlevel!^) >> "%OUT%" & echo   -^> FEHLGESCHLAGEN)
echo. >> "%OUT%"

echo.
echo Fertig. Ergebnis-Datei: %OUT%
echo Bitte die komplette Datei zurueckschicken.
pause
