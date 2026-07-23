# Baut den Lumora-Native-Installer (Phase 4): Shell bauen -> Staging -> NSIS -> signieren.
# Reproduzierbar; laeuft neben der Electron-App (eigener Ordner/Uninstall-Eintrag).
$ErrorActionPreference = "Stop"
# Repo-Root aus der Script-Lage ableiten (PC-unabhaengig; frueher hartkodiert).
# TrimEnd: ein Laufwerks-Root ("Z:\") kommt MIT Backslash aus Resolve-Path -
# ohne Trim verrutschen alle Substring-Pfadberechnungen um 1 Zeichen (icon->con!).
$root = (Resolve-Path "$PSScriptRoot\..\..").Path.TrimEnd('\')
$shell = "$root\capture-cpp\lumora-shell"
$stage = "$shell\stage"
$version = "3.0.0"   # 3.0.0: erstes offizielles Release der nativen Version (loest die Electron-
                     #        Linie 2.2.x ab). Buendelt die gesamte 0.2.x-Reihe: eigener C++-Stack
                     #        statt Electron/FFmpeg/mediamtx, Streaming-Stabilitaet, Eingabe-Bruecke,
                     #        Speicherleck-/Blackout-Fixes. Download 171 MB -> ~3,5 MB. App durchgaengig
                     #        uebersetzt, Ueber-Dialog + Versionsressource korrekt (vorher 0.1.0).
$prev_0_2_13 = "" # 0.2.13: Aufgabenplanung nach Electron->nativ-Umstieg repariert (alte OSD-
                     #         Broker-Aufgaben werden beim Installieren entfernt + Pfad-Abgleich
                     #         gegen die aktuelle exe), RTSS-FPS-Quelle schliesst dwm.exe aus
                     #         (zeigte sonst die Desktop-Hz statt der Spiel-Framerate), eigener
                     #         FPS-Broker bevorzugt jetzt das Vordergrundfenster statt "wer
                     #         praesentiert am meisten" (gleiches Symptom bei begrenzter fps),
                     #         Eingabe-Bruecke: Achsen/Knoepfe pro Quellgeraet (vid/pid) getrennt
                     #         (Lenken loeste Bremsen aus, wenn Lenkrad + Pedalset dieselbe
                     #         Achsen-Usage nutzten), build-installer prueft cmake-Exit-Code
                     #         (stiller Compile-Fehler lieferte sonst die ALTE exe weiter)
$prev_0_2_12 = "" # 0.2.12: AMD-Blackout-Fixes (Codec-Reconcile-Reihenfolge, AMF INPUT_FULL-Drain,
                     #         Codec-Race Vorschau vs. Push -> Relay-Default h264 + --codec-Spawn-Arg,
                     #         AV1-Faehigkeit persistiert), Relay-Sende-Thread (4K-Framedrops mit
                     #         Zuschauern), AMF SPEED-Preset (4K-Encode neben 4K-Decode), Router-Phase
                     #         parallelisiert (~halbe Wartezeit auf den oeffentlichen Link), Stream-Link
                     #         Auto-Kopieren + deutliche Rueckmeldung (Electron-Paritaet), Kopier-Klick
                     #         waehrend der Vorbereitung wird vorgemerkt statt abgewiesen
$prev_0_2_11 = "" # 0.2.11: Streaming-Stabilitaet (Keyframe-VBV-Deckel, NACK/SSRC-Fix, Regelungs-
                     #         Ueberreaktion entschaerft, AV1 nur mit HW-Decoder), Speicherleck-Fix
                     #         Encoder-Neustart, Fenstergroesse nach Windows-Neustart, schwarzes
                     #         Tuersteher-/Hauptfenster behoben, Eingabe-Bruecke greift nur mit
                     #         angeschlossenem Geraet + Profil-Loeschen fragt nach + Live-Controller-
                     #         Animation, eigener Dialog statt haesslicher System-Boxen, Ueber-Dialog
                     #         auf nativen Stand, Gruppen-Austritt beim Schliessen entblockt
                     # 0.2.10: Reihenfolge Einstellungs-Reiter zurueckgetauscht - Darstellung vor Steuerung
                     # 0.2.9: Reihenfolge Einstellungs-Reiter - Steuerung zwischen Allgemein und Darstellung
                     # 0.2.8: Beta-Schalter "Nativer Aufnahme-Modus" entfernt - der native Weg ist jetzt fest aktiv
                     # 0.2.7: FIX Encoder-Destruktoren (Speicherleck/Absturz), Eingabe-Bruecke visuell + Auto-Kalibrierung
                     # 0.2.6: Komponenten-Update bietet Neustart an (Shell-Tausch -> neue Version laeuft)
                     # 0.2.5: bedarfsgesteuertes AV1/H.264-Encoding, Player-Codec-Anzeige-Fix
                     # 0.2.4: AV1-Doppel-Encode, Eingabe-Bruecke, Komponenten-Updater, Statistik, libjuice-Patch
                     # 0.2.3: Alt-Electron-Version wird IMMER sauber deinstalliert (Parallel-Beta-Reste)
                     # 0.2.2: Auto-Update silent (/S), Update-Erkennung im Installer, Uninstall-Datenfrage, MUI2-Optik
                     # 0.2.1: BF6-HDR-Fix, Gamepad-Fokus, GPU-Name, Bitrate-Presets 35/50
                     # 0.2.0: eigener Relay (mediamtx-Abloesung), native HDR, ETW-FPS, libvpl statisch

# 1) Shell frisch bauen (cmake PC-unabhaengig suchen: VS2022 BuildTools ODER VS2026 Community -
# die Entwicklungs-PCs haben unterschiedliche Toolchains; build\ ist jeweils lokal konfiguriert)
$cmake = @(
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
  "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $cmake) { throw "cmake.exe nicht gefunden (VS2022 BuildTools / VS2026 Community)" }

# app.rc-Versionsressource aus $version patchen, damit die exe (und damit der "Ueber"-Dialog
# via shellVersion()/GetFileVersionInfo) NIE wieder von der Installer-Version abweicht. Genau
# das war lange kaputt: app.rc stand fix auf 0,1,0,0 -> "Ueber" zeigte 0.1.0, egal welche
# Installer-Version. $version ("3.0.0") -> Komma-Form "3,0,0,0" und Punkt-Form "3.0.0.0".
$verParts = ($version -split '\.') + @('0','0','0','0') | Select-Object -First 4
$verComma = $verParts -join ','
$verDot   = $verParts -join '.'
$rcPath = "$shell\app.rc"
$rc = Get-Content $rcPath -Raw
$rc = [regex]::Replace($rc, 'FILEVERSION\s+\d+,\d+,\d+,\d+', "FILEVERSION $verComma")
$rc = [regex]::Replace($rc, 'PRODUCTVERSION\s+\d+,\d+,\d+,\d+', "PRODUCTVERSION $verComma")
$rc = [regex]::Replace($rc, '("FileVersion",\s*")\d+\.\d+\.\d+\.\d+(")', "`${1}$verDot`${2}")
$rc = [regex]::Replace($rc, '("ProductVersion",\s*")\d+\.\d+\.\d+\.\d+(")', "`${1}$verDot`${2}")
[IO.File]::WriteAllText($rcPath, $rc, (New-Object Text.UTF8Encoding($false)))
Write-Output "app.rc auf $verDot gepatcht"

# Alte exe VOR dem Build wegraeumen + Exit-Code pruefen - sonst liefert ein stiller
# Compile-Fehler (Out-Null verschluckt die Meldung) unbemerkt die ALTE exe weiter, und der
# Installer wird faelschlich als "erfolgreich" mit unveraendertem Inhalt neu gebaut (real
# passiert: neuer Code kompilierte nicht, Installer war bit-identisch zum vorherigen Stand).
Remove-Item "$shell\build\Release\lumora_shell.exe" -ErrorAction SilentlyContinue
& $cmake --build "$shell\build" --config Release
if ($LASTEXITCODE -ne 0) { throw "Shell-Build fehlgeschlagen (cmake/msbuild Exit-Code $LASTEXITCODE) - siehe Ausgabe oben" }
if (-not (Test-Path "$shell\build\Release\lumora_shell.exe")) { throw "Shell-Build fehlgeschlagen (keine exe erzeugt)" }

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
