# Lumora bauen & entwickeln

Kurzüberblick, wie das Projekt lokal läuft, getestet und als Release gebaut wird.
Windows 10/11, PowerShell.

## Voraussetzungen (einmalig installieren)
```powershell
winget install --id OpenJS.NodeJS.LTS -e      # Node.js (Electron, electron-builder)
winget install --id Microsoft.DotNet.SDK.8 -e # .NET 8 SDK (Capture-Helfer)
```
Danach **PowerShell neu öffnen** (PATH). Prüfen: `node -v`, `dotnet --version`.

## 1. Abhängigkeiten holen
```powershell
npm install
```
Zieht Electron, electron-builder, electron-updater, koffi + das `asar`-Tool.

## 2. Binaries besorgen  (NICHT im Repo – .gitignore, teils >100 MB)
Sie gehören in den Ordner **`bin\`** des Repos:
- **ffmpeg.exe** (LGPL): github.com/BtbN/FFmpeg-Builds → `ffmpeg-master-latest-win64-lgpl.zip`, daraus `bin\ffmpeg.exe`. **Wichtig: die *lgpl*-Variante** (nicht gpl).
- **mediamtx.exe**: github.com/bluenviron/mediamtx/releases → Windows amd64.
- **lumora-capture.exe**: selbst bauen (nächster Schritt).

Schnellste Alternative: die drei Dateien aus einer **installierten Lumora** kopieren:
`%LOCALAPPDATA%\Programs\lumora\resources\bin\`  →  `bin\`.

## 3. Capture-Helfer bauen  (nur nach Änderungen an `capture\Program.cs`)
```powershell
dotnet publish capture -c Release -r win-x64 --self-contained -p:PublishSingleFile=true -o bin
```
Ergebnis: `bin\lumora-capture.exe` (self-contained .NET 8; ~180 MB).

## 4. Entwickeln & testen – zwei Wege

**A) Direkt aus dem Quellcode (am schnellsten):**
```powershell
npm start
```
Startet Electron direkt aus dem Repo. Binaries werden aus `bin\`, sonstige Ressourcen
(PresentMon.exe, HDRCmd.exe …) aus dem Repo-Root gelesen. Ideal zum schnellen Iterieren.

**B) In eine installierte Lumora deployen (näher am echten Zustand):**
```powershell
._testlab\deploy.ps1
```
Baut die `app.asar` und kopiert sie + Binaries in die installierte App
(`…\Programs\lumora`). **Beendet Lumora vorher automatisch** (sonst Dateisperre).
Danach Lumora neu starten. Optional prüfen: `._testlab\deploy.ps1 -Verify "suchtext"`.

## 5. Release-Installer bauen
```powershell
# 1) Version hochsetzen in package.json  ("version": "x.y.z")
# 2) Release-Notes anlegen: _testlab\release-notes\<version>.txt
._testlab\release.ps1
```
Baut den NSIS-Installer (electron-builder), schreibt `latest.yml` (BOM-frei) mit den
Notes, aktualisiert `website\download.php` auf die neue Datei und legt alles in
`website\updates\` ab. **Hochzuladen:** die 3 Dateien aus `website\updates\` +
`website\download.php` + `website\index.html`.

## Wichtige Fallstricke
- **Lumora vor deploy/release beenden** – ein laufender Prozess sperrt `app.asar`
  (deploy.ps1/release.ps1 erledigen das selbst).
- **latest.yml/download.php nie mit `Set-Content -Encoding UTF8`** schreiben (BOM →
  Update-Parser/Umlaute kaputt). Skripte nutzen `[IO.File]::WriteAllText(…UTF8Encoding($false))`.
- **Streaming immer über die volle Kette testen**, nicht nur den Encoder:
  mediamtx muss „stream is available and online, **2 tracks (H264, Opus)**" melden,
  FFmpeg `frame=` steigt, `speed≈1.0` über ≥15 s. (Testskripte: siehe frühere `scratchpad/mon.js`/`brk.js`.)
- Builds sind **unsigniert** (kein Zertifikat) – SmartScreen-Warnung ist normal.
- Der Autostart-/Rebrand-Kram und weitere Details stehen in `_testlab\HANDOFF-AMD.md`.

## Projektstruktur (Kurz)
- `main.js` – Electron-Hauptprozess (Launcher, OSD, **Streaming**: `bcDetectEncoder`,
  `bcEncoderArgs`, `bcBuildFfmpegArgs`, `bcStartFfmpeg`).
- `index.html` / `styles.css` – Oberfläche (Renderer).
- `player.html` – WHEP-Player, den Zuschauer im Browser öffnen.
- `capture\Program.cs` – C#/.NET Windows-Graphics-Capture-Helfer (Fenster-Aufnahme + Ton).
- `_testlab\` – Dev-Skripte (deploy/release) + Übergabe-Notizen.
- `website\` – öffentliche Seite + `download.php` + `updates\` (Auto-Update-Feed).
