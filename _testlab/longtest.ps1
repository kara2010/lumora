# Langzeit-Monitor fuer den Lumora-Stream (NUR LESEND, greift nie ein).
# Sammelt pro Minute einen Messpunkt in eine CSV + auffaellige Ereignisse in ein
# Event-Log. Stoppt ueber die Stop-Datei oder nach MaxHours.
# HINWEIS: ASCII-only halten (PowerShell 5.1 liest .ps1 als ANSI).
param(
  [string]$OutDir = "$PSScriptRoot",
  [int]$IntervalSec = 60,
  [int]$MaxHours = 12
)
$ErrorActionPreference = "Continue"
$stamp = Get-Date -Format "yyyy-MM-dd"
$csv = Join-Path $OutDir "longtest-$stamp.csv"
$evt = Join-Path $OutDir "longtest-$stamp-events.log"
$stopFile = Join-Path $OutDir "longtest.stop"
$streamLog = Join-Path $env:TEMP "lumora-stream.log"

if (-not (Test-Path $csv)) {
  "time;ffmpegPid;ffmpegMemMB;mtxPid;mtxMemMB;lumoraMemMB;capMemMB;ready;mbReceived;readers;framesInError;gpuPct;encPct;decPct;gpuTempC;logErrLines;lastFps;lastSpeed" | Out-File -FilePath $csv -Encoding ascii
}
function LogEvent([string]$msg) {
  ((Get-Date -Format "HH:mm:ss") + "  " + $msg) | Out-File -FilePath $evt -Append -Encoding ascii
}
LogEvent "=== Monitor gestartet (Intervall ${IntervalSec}s, max ${MaxHours}h) ==="

$prevFfPid = 0; $prevBytes = -1; $prevReady = $true; $stallCount = 0
$deadline = (Get-Date).AddHours($MaxHours)

while ((Get-Date) -lt $deadline -and -not (Test-Path $stopFile)) {
  try {
    $t = Get-Date -Format "yyyy-MM-dd HH:mm:ss"

    # --- Prozesse ---
    $ff  = Get-Process -Name ffmpeg -ErrorAction SilentlyContinue | Select-Object -First 1
    $mtx = Get-Process -Name mediamtx -ErrorAction SilentlyContinue | Select-Object -First 1
    $lum = Get-Process -Name Lumora -ErrorAction SilentlyContinue
    $cap = Get-Process -Name lumora-capture -ErrorAction SilentlyContinue
    $ffPid = 0;  if ($ff)  { $ffPid = $ff.Id }
    $ffMem = 0;  if ($ff)  { $ffMem = [math]::Round($ff.WorkingSet64/1MB) }
    $mtxPid = 0; if ($mtx) { $mtxPid = $mtx.Id }
    $mtxMem = 0; if ($mtx) { $mtxMem = [math]::Round($mtx.WorkingSet64/1MB) }
    $lumMem = 0; if ($lum) { $lumMem = [math]::Round(($lum | Measure-Object WorkingSet64 -Sum).Sum/1MB) }
    $capMem = 0; if ($cap) { $capMem = [math]::Round(($cap | Measure-Object WorkingSet64 -Sum).Sum/1MB) }

    # Encoder-Neustart erkennen (PID-Wechsel)
    if ($prevFfPid -ne 0 -and $ffPid -ne $prevFfPid) {
      if ($ffPid -eq 0) { LogEvent "FFMPEG WEG (war PID $prevFfPid)" }
      else { LogEvent "FFMPEG NEUSTART: PID $prevFfPid -> $ffPid" }
    }
    $prevFfPid = $ffPid

    # --- mediamtx-API: lebt der Stream wirklich? ---
    $ready = ""; $mb = ""; $readers = ""; $fErr = ""
    try {
      $api = Invoke-WebRequest -Uri "http://127.0.0.1:9997/v3/paths/list" -UseBasicParsing -TimeoutSec 5
      $j = $api.Content | ConvertFrom-Json
      $it = $null
      foreach ($x in $j.items) { if ($x.name -eq "live") { $it = $x } }
      if ($it) {
        $ready = $it.ready
        $mb = [math]::Round($it.bytesReceived/1MB, 1)
        $readers = @($it.readers).Count
        $fErr = $it.inboundFramesInError
        # Datenfluss-Stillstand erkennen (Bytes wachsen nicht mehr)
        if ($prevBytes -ge 0 -and $it.bytesReceived -le $prevBytes) {
          $stallCount++
          LogEvent ("DATENSTILLSTAND #" + $stallCount + ": bytesReceived unveraendert bei " + $mb + " MB")
        } else { $stallCount = 0 }
        $prevBytes = $it.bytesReceived
        if ($prevReady -and ($ready -ne $true)) { LogEvent "STREAM NICHT MEHR READY" }
        if ((-not $prevReady) -and ($ready -eq $true)) { LogEvent "Stream wieder ready" }
        $prevReady = ($ready -eq $true)
      } else {
        LogEvent "Pfad 'live' fehlt in der API-Antwort"
        $prevBytes = -1
      }
    } catch {
      $ready = "api-down"
      if ($prevReady) { LogEvent ("API NICHT ERREICHBAR: " + $_.Exception.Message) }
      $prevReady = $false
    }

    # --- GPU (NVIDIA) ---
    $gpu = ""; $enc = ""; $dec = ""; $gtemp = ""
    try {
      $smi = & nvidia-smi --query-gpu=utilization.gpu,utilization.encoder,utilization.decoder,temperature.gpu --format=csv,noheader,nounits 2>$null
      if ($smi) {
        $p = ($smi | Select-Object -First 1) -split ","
        $gpu = $p[0].Trim(); $enc = $p[1].Trim(); $dec = $p[2].Trim(); $gtemp = $p[3].Trim()
      }
    } catch {}

    # --- Encoder-Fortschritt + Fehlerzeilen aus dem Stream-Log ---
    $errCount = ""; $lastFps = ""; $lastSpeed = ""
    try {
      if (Test-Path $streamLog) {
        $tail = Get-Content $streamLog -Tail 400
        $errCount = @($tail | Select-String -Pattern "Invalid argument|Terminating|error|beendet \(" -SimpleMatch:$false).Count
        $frameLines = @($tail | Select-String -Pattern "ff: frame=")
        if ($frameLines.Count -gt 0) {
          $fl = $frameLines[-1].Line
          if ($fl -match "fps=\s*([0-9.]+)") { $lastFps = $Matches[1] }
          if ($fl -match "speed=([0-9.]+)x") { $lastSpeed = $Matches[1] }
        }
      }
    } catch {}

    "$t;$ffPid;$ffMem;$mtxPid;$mtxMem;$lumMem;$capMem;$ready;$mb;$readers;$fErr;$gpu;$enc;$dec;$gtemp;$errCount;$lastFps;$lastSpeed" | Out-File -FilePath $csv -Append -Encoding ascii
  } catch {
    LogEvent ("Monitor-Fehler: " + $_.Exception.Message)
  }
  Start-Sleep -Seconds $IntervalSec
}
LogEvent "=== Monitor beendet ==="
