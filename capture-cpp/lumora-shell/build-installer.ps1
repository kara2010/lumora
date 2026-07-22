# Baut den Lumora-Native-Installer (Phase 4): Shell bauen -> Staging -> NSIS -> signieren.
# Reproduzierbar; laeuft neben der Electron-App (eigener Ordner/Uninstall-Eintrag).
$ErrorActionPreference = "Stop"
# Repo-Root aus der Script-Lage ableiten (PC-unabhaengig; frueher hartkodiert).
# TrimEnd: ein Laufwerks-Root ("Z:\") kommt MIT Backslash aus Resolve-Path -
# ohne Trim verrutschen alle Substring-Pfadberechnungen um 1 Zeichen (icon->con!).
$root = (Resolve-Path "$PSScriptRoot\..\..").Path.TrimEnd('\')
$shell = "$root\capture-cpp\lumora-shell"
$stage = "$shell\stage"
$version = "0.2.7"   # 0.2.7: FIX Encoder-Destruktoren (Speicherleck/Absturz), Eingabe-Bruecke visuell + Auto-Kalibrierung
                     # 0.2.6: Komponenten-Update bietet Neustart an (Shell-Tausch -> neue Version laeuft)
                     # 0.2.5: bedarfsgesteuertes AV1/H.264-Encoding, Player-Codec-Anzeige-Fix
                     # 0.2.4: AV1-Doppel-Encode, Eingabe-Bruecke, Komponenten-Updater, Statistik, libjuice-Patch
                     # 0.2.3: Alt-Electron-Version wird IMMER sauber deinstalliert (Parallel-Beta-Reste)
                     # 0.2.2: Auto-Update silent (/S), Update-Erkennung im Installer, Uninstall-Datenfrage, MUI2-Optik
                     # 0.2.1: BF6-HDR-Fix, Gamepad-Fokus, GPU-Name, Bitrate-Presets 35/50
                     # 0.2.0: eigener Relay (mediamtx-Abloesung), native HDR, ETW-FPS, libvpl statisch

# 1) Shell frisch bauen
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build "$shell\build" --config Release | Out-Null
if (-not (Test-Path "$shell\build\Release\lumora_shell.exe")) { throw "Shell-Build fehlgeschlagen" }

# 2) Staging aufbauen
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$shell\build\Release\lumora_shell.exe" "$stage\lumora-shell.exe"

# UI-Assets (was das WebView2 laedt) - genau die Dateien, die die Shell via app.lumora mappt
foreach ($f in "index.html","styles.css","player.html","osd.html","doorman.html","icon.ico","icon-64.png") {
  if (Test-Path "$root\$f") { Copy-Item "$root\$f" $stage }
}
# UI-Bilder/Assets (Logos etc.), die index.html/styles.css referenzieren
foreach ($f in Get-ChildItem "$root\*.png","$root\*.svg" -ErrorAction SilentlyContinue) {
  if ($f.Length -lt 500KB) { Copy-Item $f.FullName $stage }   # Screenshots (mehrere MB) NICHT ins Paket
}

# Native Binaries (nur die, die die Shell wirklich startet - FFmpeg/C#-Helfer entfallen).
# lumora-media-relay.exe ist der EIGENE C++-Relay (capture-cpp/lumora-relay).
# KEIN mediamtx.exe im Paket (52 MB -> Installer waere doppelt so gross); der Fallback
# liegt archiviert auf dem NAS (Fileshare) und greift via useLegacyRelay nur, wenn er
# manuell in bin\ gelegt wird. libvpl.dll entfaellt (statisch gelinkt).
New-Item -ItemType Directory -Force "$stage\bin" | Out-Null
foreach ($b in "lumora-capture-native.exe","lumora-media-relay.exe","lumora-elevate.exe") {
  if (Test-Path "$root\bin\$b") { Copy-Item "$root\bin\$b" "$stage\bin" }
}
# Sensor-Module (OSD) neben die Shell (wie in der Electron-Struktur).
# HDRCmd.exe entfaellt (eigener Code: launch_game.h setHDR),
# PresentMon.exe entfaellt (eigener ETW-Consumer: etw_present.h).
foreach ($b in "AMDFamily17.bin","IntelMSR.bin") {
  if (Test-Path "$root\$b") { Copy-Item "$root\$b" $stage }
}
# Lizenztexte (Distributionspflicht)
foreach ($f in Get-ChildItem "$root\*-LICENSE.txt","$root\*LICENSE*.txt" -ErrorAction SilentlyContinue) { Copy-Item $f.FullName $stage }

# WebView2-Evergreen-Bootstrapper (Win10-Absicherung; ~2 MB, laedt bei Bedarf nach)
$bootstrap = "$stage\MicrosoftEdgeWebview2Setup.exe"
if (-not (Test-Path "$shell\MicrosoftEdgeWebview2Setup.exe")) {
  Invoke-WebRequest "https://go.microsoft.com/fwlink/p/?LinkId=2124703" -OutFile "$shell\MicrosoftEdgeWebview2Setup.exe"
}
Copy-Item "$shell\MicrosoftEdgeWebview2Setup.exe" $bootstrap

$stageSize = [math]::Round((Get-ChildItem $stage -Recurse | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Output "Staging: $stageSize MB"

# 2b) EIGENE EXE signieren - PFLICHT: Smart App Control / SmartScreen verlangen einen
# bestaetigten Herausgeber PRO ausfuehrbarer Datei, nicht nur fuer den Installer.
# Fremd-Binaries (lumora-elevate, lumora-media-relay, PresentMon, HDRCmd) sind bereits signiert.
$signtool = "$root\_testlab\tools\signtool\signtool.exe"
if (Test-Path $signtool) {
  foreach ($exe in "$stage\lumora-shell.exe", "$stage\bin\lumora-capture-native.exe", "$stage\bin\lumora-media-relay.exe", "$stage\bin\lumora-elevate.exe") {
    if (Test-Path $exe) {
      $st = (Get-AuthenticodeSignature $exe).Status
      if ($st -ne "Valid") {
        & $signtool sign /sha1 EC6B6B6FDEBDB88941519F15E9570994CE3E14E3 /fd sha256 /tr http://time.certum.pl /td sha256 $exe | Out-Null
      }
      Write-Output ("Signatur {0}: {1}" -f (Split-Path $exe -Leaf), (Get-AuthenticodeSignature $exe).Status)
    }
  }
} else { Write-Warning "signtool fehlt - EXE bleiben UNSIGNIERT (Smart App Control blockiert sie!)" }

# 3) NSIS-Installer bauen
$makensis = (Get-ChildItem "$env:LOCALAPPDATA\electron-builder\Cache\nsis" -Recurse -Filter makensis.exe | Select-Object -First 1).FullName
if (-not $makensis) { throw "makensis.exe nicht gefunden" }
Push-Location $shell
& $makensis "/DVERSION=$version" "/DSRCDIR=stage" installer.nsi
Pop-Location
$out = "$shell\Lumora-Native-Setup-$version.exe"
if (-not (Test-Path $out)) { throw "NSIS-Build fehlgeschlagen" }

# 4) Signieren (Certum, wie alle unsere EXEs)
$signtool = "$root\_testlab\tools\signtool\signtool.exe"
if (Test-Path $signtool) {
  & $signtool sign /sha1 EC6B6B6FDEBDB88941519F15E9570994CE3E14E3 /fd sha256 /tr http://time.certum.pl /td sha256 $out | Select-Object -Last 1
  $sig = (Get-AuthenticodeSignature $out).Status
  Write-Output "Signatur: $sig"
}
$mb = [math]::Round((Get-Item $out).Length / 1MB, 1)
Write-Output "FERTIG: $out ($mb MB)"

# 5) Update-Feed erzeugen (native-update.json) - Rollout = diese 2 Dateien hochladen:
#    website\updates\Lumora-Native-Setup-<version>.exe + website\updates\native-update.json
#    Bestandskunden ziehen das Update dann automatisch (Shell: setupAutoUpdate -> /S-Install).
#    Release-Notes optional aus _testlab\release-notes\<version>.txt (===EN===-Trenner wie latest.yml).
$updDir = "$root\website\updates"
New-Item -ItemType Directory -Force $updDir | Out-Null
Copy-Item $out $updDir -Force
$notes = ""; $notesEn = ""
$notesFile = "$root\_testlab\release-notes\$version.txt"
if (Test-Path $notesFile) {
  $raw = [IO.File]::ReadAllText($notesFile)
  $parts = $raw -split "===EN===", 2
  $notes = $parts[0].Trim()
  if ($parts.Count -gt 1) { $notesEn = $parts[1].Trim() }
}
$feed = @{ version = $version
           url = "https://lumora-streaming.de/updates/Lumora-Native-Setup-$version.exe"
           notes = $notes; notesEn = $notesEn } | ConvertTo-Json
[IO.File]::WriteAllText("$updDir\native-update.json", $feed, (New-Object Text.UTF8Encoding($false)))   # BOM-frei (Feed-Parser!)
Write-Output "Update-Feed: $updDir\native-update.json (v$version)"

# 6) Komponenten-Manifest (components.json) + Dateiablage fuer den Komponenten-Updater
#    (update_components.h): SHA-256 je Staging-Datei; Rollout = updates\components.json
#    + updates\components\<version>\ hochladen. Bestandskunden tauschen dann nur die
#    geaenderten Dateien (atomar, signaturgeprueft) - der Basis-Installer bleibt unberuehrt.
$compDir = "$updDir\components\$version"
Remove-Item -Recurse -Force $compDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $compDir | Out-Null
$files = @()
foreach ($f in Get-ChildItem $stage -Recurse -File) {
  if ($f.Name -eq "MicrosoftEdgeWebview2Setup.exe") { continue }   # Bootstrapper: nur Erstinstallation
  $rel = $f.FullName.Substring($stage.Length + 1)
  $sha = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()
  $dst = Join-Path $compDir $rel
  New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
  Copy-Item $f.FullName $dst
  $files += @{ path = $rel; sha256 = $sha; size = $f.Length }
}
$manifest = @{ version = $version
               baseUrl = "https://lumora-streaming.de/updates/components/$version/"
               files = $files } | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText("$updDir\components.json", $manifest, (New-Object Text.UTF8Encoding($false)))
Write-Output "Komponenten-Manifest: $updDir\components.json ($($files.Count) Dateien)"
