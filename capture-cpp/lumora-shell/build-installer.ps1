# Baut den Lumora-Native-Installer (Phase 4): Shell bauen -> Staging -> NSIS -> signieren.
# Reproduzierbar; laeuft neben der Electron-App (eigener Ordner/Uninstall-Eintrag).
$ErrorActionPreference = "Stop"
# Repo-Root aus der Script-Lage ableiten (PC-unabhaengig; frueher hartkodiert).
# TrimEnd: ein Laufwerks-Root ("Z:\") kommt MIT Backslash aus Resolve-Path -
# ohne Trim verrutschen alle Substring-Pfadberechnungen um 1 Zeichen (icon->con!).
$root = (Resolve-Path "$PSScriptRoot\..\..").Path.TrimEnd('\')
$shell = "$root\capture-cpp\lumora-shell"
$stage = "$shell\stage"
$version = "3.2.1"   # 3.2.1: Gestartetes Spiel kommt zuverlaessig vor Lumora in den Vordergrund.
                     #        3.1.1 speicherte den Zustand korrekt, wendete ihn aber im Regelfall
                     #        des Nutzers nie an: bei Autostart mit --minimized ruft wWinMain kein
                     #        ShowWindow auf, und das spaetere Oeffnen per Tray/Hotkey zeigte das
                     #        Fenster ueber SW_SHOW in Normalgroesse. Jetzt zieht showMainWindow den
                     #        gespeicherten Zustand beim ERSTEN Zeigen nach (danach nicht mehr, damit
                     #        ein bewusst verkleinertes Fenster nicht wieder aufgerissen wird).
$prev_3_1_1 = ""  # 3.1.1: OSD-Vorschau in den Einstellungen wirkt jetzt wirklich - sie lief bis
                     #        zur letzten Zeile und wurde dort verworfen (osdEnsureVisible pruefte
                     #        nur osdEnabled, nicht die Vorschau) - und greift jetzt bei JEDER
                     #        Aenderung (Design, Werte, FPS-Quelle, Grafikkarte), nicht nur an den
                     #        Reglern; die einmalige OSD-Einrichtung (Dialog + UAC) wird dabei
                     #        bewusst NICHT angestossen. Fenster kam nach einem Windows-Neustart
                     #        nicht maximiert hoch, wenn es maximiert-minimiert abgelegt war
                     #        (WPF_RESTORETOMAXIMIZED wurde nicht ausgewertet). Positions-Wahl im
                     #        Analyse-Reiter nutzt jetzt dieselbe Darstellung wie das Overlay
                     #        (2x2-Raster statt Viererreihe, eine gemeinsame Stilquelle).
$prev_3_1_0 = ""  # 3.1.0: Ruckler-Blackbox - neue Analyse-Funktion findet die Ursache von
                     #        Mikro-Rucklern (Kernel-ETW: Hintergrundprozesse, Treiber-DPC/ISR-
                     #        Latenzen, GPU-Throttling/VRAM, Datentraeger, Programmstarts) waehrend
                     #        einer expliziten Mess-Session, mit eigenem OSD, Klartext-Bericht,
                     #        Verlauf + Vergleich zweier Laeufe; dazu Flaschenhals-Erkennung
                     #        (GPU-/CPU-/Einzelkern-Limit, FPS-Limiter, Throttling, VRAM) als
                     #        Live-Badge + Verteilung im Bericht. Gamepad-Hotkey fuer Start/Stop
                     #        der Messung. Menue konsolidiert (Darstellung/Steuerung sind jetzt
                     #        Abschnitte in Allgemein). Gaming-OSD: sanftes Ein-/Ausblenden
                     #        repariert (DComp-Visual-Opacity scheiterte still, jetzt CSS-basiert),
                     #        Fensterinhalt konnte nach Vordergrund-Holen horizontal verrutschen
                     #        (fehlendes overflow-x auf drei Scroll-Containern) - behoben. Mehrere
                     #        Uebersetzungsluecken in der Eingabe-Bruecke geschlossen.
$prev_3_0_5 = ""  # 3.0.5: Xbox-Controller (Wireless-Dongle) verbindet sich bei laufendem Lumora
                     #        zuverlaessig neu (UI-Gamepad auf nativen gp-state-Push, Chromiums
                     #        Gamepad-Monitor gestubbt; XInput-Leerplatz-Probe nur nach
                     #        WM_DEVICECHANGE); OSD blockiert keine Desktop-Klicks mehr (Chromium-
                     #        Zwischenfenster click-through); Spielende-Erkennung event-getrieben
                     #        (HDR in ~1-3s zurueck); Doppelstart-Sperre; Live-Spielzeit auf dem
                     #        Start-Knopf; Update-Suche-URL repariert (/updates/-Pfad fehlte);
                     #        QSV-Bitrate-Fallback fuer aeltere Intel-Generationen (Reset -14).
$prev_3_0_4 = ""  # 3.0.4: Update-Hinweise zuverlaessig bei minimiertem Autostart: auch das stille
                     #        Komponenten-Update meldet sich per Tray-Balloon + Nachlieferung beim
                     #        naechsten Fenster-Oeffnen; Downgrade-Reinstall-Erkennung (veraltetes
                     #        componentsVersion machte den Updater blind).
$prev_3_0_3 = ""  # 3.0.3: OSD-Live-Editor komplett neu: eigenes windowed WebView2-Fenster mit
                     #        Desktop-Schnappschuss (DXGI Duplication + HDR->SDR, GDI-Fallback) statt
                     #        Composition-Input-Weiterleitung (scheiterte an DPI/Pointer-Capture);
                     #        Bedienleiste neu gestaltet; Balken als Standard-Design.
$prev_3_0_2 = ""  # 3.0.2: Hotkey-Poll in eigenen Thread entkoppelt (schnelles Fenster-/OSD-Toggeln
                     #        per Tastatur+Gamepad geht nicht mehr verloren/verklemmt); Fenster kommt per
                     #        Hotkey sofort bedienbar in den Vordergrund (AttachThreadInput gegen die
                     #        Foreground-Sperre + Refocus); OSD schaltet sofort pro Druck (ensureOsdSetup
                     #        async statt synchron-PowerShell) + schnelleres Ausblenden; Gamepad LB/RB
                     #        blaettert die Reiter (index.html); Update-Balloon bei minimiertem Autostart.
$prev_3_0_1 = ""  # 3.0.1: OSD-Overlay auf Panel-Groesse verkleinert (blockierte als Vollbild-Overlay
                     #        auf manchen Systemen die Desktop-Symbole, auch nach dem Deaktivieren);
                     #        Edit-Modus-Ende SWP_FRAMECHANGED-Fix; Relay/Capture tragen jetzt das App-Icon.
$prev_3_0_0 = ""  # 3.0.0: erstes offizielles Release der nativen Version (loest die Electron-
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

# 0) Uebersetzung vollstaendig? (RELEASE.md Schritt 3 - jetzt automatisch statt von Hand)
# Schluessel im Woerterbuch I18N_EN ist der DEUTSCHE SATZ selbst. Aendert jemand den
# deutschen Text, findet der Schluessel nichts mehr und die englische Oberflaeche zeigt
# an dieser Stelle STILL Deutsch - ohne Fehler, ohne Warnung. Genau so gingen 3.1.0 bis
# 3.2.0 mit 7 Luecken raus. Darum ZUERST pruefen und bei Luecken abbrechen: lieber ein
# fehlgeschlagener Bau als eine halb uebersetzte Version beim Nutzer.
$node = (Get-Command node -EA SilentlyContinue).Source
if (-not $node) { throw "node.exe nicht gefunden - fuer den Uebersetzungs-Check benoetigt" }
& $node "$root\_testlab\i18n-check.js" "$root\index.html" "$root\_testlab\i18n-ignore.txt"
if ($LASTEXITCODE -ne 0) { throw "Uebersetzungs-Check fehlgeschlagen (s. Liste oben) - Bau abgebrochen" }

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
foreach ($f in "index.html","styles.css","player.html","osd.html","analyze-osd.html","doorman.html","icon.ico","icon-64.png") {
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

# 7) latest.yml (electron-updater-Format) - Auto-Update-Pfad der ALTEN Electron-Linie
#    (2.2.x, main.js). Bestandsnutzer, die noch nicht auf nativ umgestiegen sind, holen
#    sich hierueber den aktuellen Installer. Wurde bis 3.1.1 NICHT vom Build erzeugt und
#    musste jedes Mal von Hand nachgezogen werden - zweimal in Folge stand sie deshalb
#    beim Deploy noch auf der VORversion (der Feed haette Altnutzer auf ein veraltetes
#    Setup gezeigt). Jetzt faellt sie automatisch mit an.
#    sha512 ist base64-kodiert (nicht hex!) - genau so erwartet es electron-updater.
$setupName = Split-Path $out -Leaf
$sha512 = [Convert]::ToBase64String(
  [Security.Cryptography.SHA512]::Create().ComputeHash([IO.File]::ReadAllBytes($out)))
$size = (Get-Item $out).Length
# Invariante Kultur + UTC: sonst schreibt eine deutsche Locale z.B. Komma-Trennung
# oder lokale Zeit in den Zeitstempel.
$relDate = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ", [Globalization.CultureInfo]::InvariantCulture)
$yml = @"
version: $version
files:
  - url: $setupName
    sha512: $sha512
    size: $size
path: $setupName
sha512: $sha512
releaseDate: '$relDate'
"@
# BOM-frei wie die anderen Feed-Dateien - ein BOM laesst YAML-Parser scheitern (siehe BUILD.md).
[IO.File]::WriteAllText("$updDir\latest.yml", $yml, (New-Object Text.UTF8Encoding($false)))
Write-Output "Alt-Feed (Electron): $updDir\latest.yml (v$version, $size Bytes)"
