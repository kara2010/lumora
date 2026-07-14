@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem  Lumora AMD-GPU-Encoder-Test (fuer AMD-Systeme, z.B. Ryzen 6800H / 680M)
rem  Prueft die recherchierten GPU-Ketten fuer h264_amf, BEVOR sie in die App
rem  eingebaut werden. Laufzeit: ~30 Sekunden. Ergebnis: amf-gpu-test-ergebnis.txt
rem  (bitte komplette Datei zurueckschicken).
rem ===========================================================================

set "FF=%LOCALAPPDATA%\Programs\lumora\resources\bin\ffmpeg.exe"
if not exist "%FF%" set "FF=%~dp0..\bin\ffmpeg.exe"
if not exist "%FF%" (
  echo FEHLER: ffmpeg.exe nicht gefunden. Bitte Lumora installieren oder Skript
  echo im Lumora-Ordner _testlab ausfuehren.
  pause
  exit /b 1
)

set "OUT=%~dp0amf-gpu-test-ergebnis.txt"
echo Lumora AMD-GPU-Test %DATE% %TIME% > "%OUT%"
echo FFmpeg: %FF% >> "%OUT%"
"%FF%" -version 2>&1 | findstr /i "version" >> "%OUT%"
echo. >> "%OUT%"

echo.
echo === Test 1/4: GPU-Kette MIT Skalierung (vpp_amf 1080p + NV12) ===
echo ============================================================ >> "%OUT%"
echo TEST 1: ddagrab - vpp_amf w=1920 h=1080 nv12 - h264_amf >> "%OUT%"
"%FF%" -hide_banner -loglevel warning -stats -y -f lavfi -i "ddagrab=output_idx=0:framerate=60" -vf "vpp_amf=w=1920:h=1080:format=nv12" -c:v h264_amf -usage lowlatency -rc cbr -b:v 8000k -maxrate 8000k -bufsize 4000k -g 120 -bf 0 -t 5 -f null - >> "%OUT%" 2>&1
if !errorlevel! equ 0 (echo ERGEBNIS TEST 1: OK >> "%OUT%" & echo   -^> OK) else (echo ERGEBNIS TEST 1: FEHLGESCHLAGEN ^(Exit !errorlevel!^) >> "%OUT%" & echo   -^> FEHLGESCHLAGEN)
echo. >> "%OUT%"

echo === Test 2/4: GPU-Kette OHNE Skalierung (nur NV12-Konvertierung) ===
echo ============================================================ >> "%OUT%"
echo TEST 2: ddagrab - vpp_amf format=nv12 - h264_amf >> "%OUT%"
"%FF%" -hide_banner -loglevel warning -stats -y -f lavfi -i "ddagrab=output_idx=0:framerate=60" -vf "vpp_amf=format=nv12" -c:v h264_amf -usage lowlatency -rc cbr -b:v 8000k -maxrate 8000k -bufsize 4000k -g 120 -bf 0 -t 5 -f null - >> "%OUT%" 2>&1
if !errorlevel! equ 0 (echo ERGEBNIS TEST 2: OK >> "%OUT%" & echo   -^> OK) else (echo ERGEBNIS TEST 2: FEHLGESCHLAGEN ^(Exit !errorlevel!^) >> "%OUT%" & echo   -^> FEHLGESCHLAGEN)
echo. >> "%OUT%"

echo === Test 3/4: BGRA direkt in den Encoder (erwartet: error 18) ===
echo ============================================================ >> "%OUT%"
echo TEST 3: ddagrab direkt - h264_amf (Gegenprobe, darf scheitern) >> "%OUT%"
"%FF%" -hide_banner -loglevel warning -stats -y -f lavfi -i "ddagrab=output_idx=0:framerate=60" -c:v h264_amf -usage lowlatency -rc cbr -b:v 8000k -maxrate 8000k -bufsize 4000k -g 120 -bf 0 -t 5 -f null - >> "%OUT%" 2>&1
if !errorlevel! equ 0 (echo ERGEBNIS TEST 3: OK ^(unerwartet!^) >> "%OUT%" & echo   -^> OK ^(unerwartet^)) else (echo ERGEBNIS TEST 3: FEHLGESCHLAGEN wie erwartet >> "%OUT%" & echo   -^> fehlgeschlagen wie erwartet)
echo. >> "%OUT%"

echo === Test 4/4: Referenz heutige CPU-Kette (Vergleichswert) ===
echo ============================================================ >> "%OUT%"
echo TEST 4: ddagrab - hwdownload/scale/yuv420p CPU - h264_amf (heutiger Stand) >> "%OUT%"
"%FF%" -hide_banner -loglevel warning -stats -y -f lavfi -i "ddagrab=output_idx=0:framerate=60" -vf "hwdownload,format=bgra,scale=-2:1080:flags=bicubic,format=yuv420p" -c:v h264_amf -usage lowlatency -rc cbr -b:v 8000k -maxrate 8000k -bufsize 4000k -g 120 -bf 0 -t 5 -f null - >> "%OUT%" 2>&1
if !errorlevel! equ 0 (echo ERGEBNIS TEST 4: OK >> "%OUT%" & echo   -^> OK) else (echo ERGEBNIS TEST 4: FEHLGESCHLAGEN ^(Exit !errorlevel!^) >> "%OUT%" & echo   -^> FEHLGESCHLAGEN)
echo. >> "%OUT%"

echo.
echo Fertig. Ergebnis-Datei: %OUT%
echo Bitte die komplette Datei zurueckschicken.
pause
