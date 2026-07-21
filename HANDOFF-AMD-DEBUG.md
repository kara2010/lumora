# Übergabe: AMD/Intel-Streaming reparieren (lokale Session am betroffenen PC)

**Arbeitsverzeichnis ab sofort:** `\\synology\Entwicklung\HDR-Launcher` (komplette Kopie vom Entwickler-PC).
**Ziel:** Der native Stream (C++-Shell + Capture-Helfer) funktioniert auf NVIDIA, aber NICHT auf AMD/Intel. Fehler direkt auf dem Gerät finden und fixen. **Kein Signieren, kein Release, kein Installer nötig** — nur bauen und testen.

---

## 1. Symptome (real beobachtet)

- **AMD, Bildschirm-Stream:** Lokale Vorschau ist inzwischen stabil, aber der **Player zeigt nur schwarzes Bild** und die **Verbindung zum Stream bricht ab** (Reconnect-Schleife).
- **AMD, Fenster-Stream (Firefox):** Vorschau zeigt **„Stream ist offline"**.
- **NVIDIA-PC:** beides einwandfrei (Bildschirm + Fenster).

## 2. Log-belegte Fakten (aus `%TEMP%\lumora-stream.log`, NICHT geraten)

Beim Bildschirm-Stream:
- Aufnahme lebt durchgehend: `60 … 1140 Frames live (19s, AMF)` — kein Einbruch.
- mediamtx wird gefüttert (keine `mtx:`-Fehler im Zeitraum).
- **`viewer 1→0` nach ~8 s, dann `viewer 0→1`** — Verbindung fällt weg und reconnectet.

→ **Nicht** Aufnahme, **nicht** mediamtx. Der **AMF-H.264-Bitstream** ist für den WebRTC/WHEP-Decoder über die Zeit unbrauchbar (Bild erst ok, dann schwarz + Abbruch). NVENC liest synchron → auf NVIDIA nie aufgetreten.

## 3. Was schon versucht wurde (und NICHT gereicht hat)

Beides in `capture-cpp/lumora-capture/main.cpp`, gebaut+getestet, Symptom blieb:
1. **Mindest-Encodergröße** (bei ~Z.656): winzige Fensterquellen (218×28) ließen AMF-Init in Schleife scheitern → jetzt seitenverhältnis-erhaltend auf min. 256×144 hochgezogen. (Behebt den `Encoder-Init fehlgeschlagen`-Loop; ob der Fenster-Weg damit sichtbar streamt, ist NICHT bestätigt.)
2. **AMF-Async-Race-Fix** in `struct AmfEncoder` (~Z.138): statt die geteilte `nv12`-Textur direkt zu wrappen (`CreateSurfaceFromDX11Native`), bekommt AMF pro Frame eine eigene Surface via `context->AllocSurface(AMF_MEMORY_DX11, NV12, w,h)` + `CopyResource`. Dazu explizit CBR, `LOWLATENCY_MODE=true`, `FILLER_DATA=false`. **Hat den schwarzen Player NICHT behoben** → Race war (allein) nicht die Ursache.

## 4. Verbleibende Hypothesen (auf dem Gerät zu prüfen)

Der AMF-Bitstream ist vermutlich malformiert oder für WHEP inkompatibel. Kandidaten:
- **Profil/Level** passt nicht zur WHEP-SDP-Aushandlung (Browser lehnt nach dem ersten Keyframe ab).
- **NAL-/AU-Struktur**: AMF liefert evtl. mehrere Buffer pro Frame oder eine andere Annex-B-Struktur als NVENC; der MPEG-TS-Muxer (main.cpp ~Z.757–768, `for (auto& au : aus)`) wickelt jeden Buffer als eigene PES-AU mit eigenem AUD/PTS — das kann eine kodierte Einzelbild-AU zerreißen.
- **PCR/PTS-Timing** aus dem AMF-Pipeline-Versatz.

## 5. DER entscheidende On-Device-Schritt (bricht die Rate-Schleife)

Auf dem AMD-PC den Helfer **isoliert** laufen lassen und den TS **ohne mediamtx** dumpen, dann mit dem **schon vorhandenen** `bin/ffmpeg.exe` analysieren:

```bat
REM 5 Sekunden AMF-Bildschirm-Encode in eine Datei (kein Netzwerk, kein mediamtx):
bin\lumora-capture-native.exe --encoder amf --monitor 0 --fps 60 --bitrate 12 --frames 300 --tsout dump.ts

REM Was ist wirklich drin? Profil/Level/Auflösung:
bin\ffmpeg.exe -i dump.ts

REM Alle Frames dekodieren und JEDEN Fehler ausgeben (das ist der Beweis):
bin\ffmpeg.exe -v error -i dump.ts -f null -
```

- Ist `dump.ts` sauber dekodierbar → Problem liegt in mediamtx/WHEP-Pfad, nicht im Encoder.
- Wirft ffmpeg Dekodier-Fehler / falsches Profil → der AMF-Bitstream/Muxer ist die Ursache. Dann NVENC-Dump (`--encoder ... ` auf dem NVIDIA-PC bzw. Referenz) vergleichen: `ffmpeg -i` Seite an Seite. Der Unterschied ist der Fix.

Helfer-Argumente (aus `main.cpp` Z.567–582): `--encoder auto|nvenc|amf|qsv`, `--fps`, `--bitrate` (MBit), `--mtx-host`, `--mtx-port` (Default 9998), `--frames N`, `--tsout DATEI`, `--hwnd <id>`, `--window`, `--monitor <idx>`, `--scale <höhe>`, `--audio`. `--list` listet Fenster.

## 6. Build-Setup am neuen PC

**WICHTIG — Build-Caches sind PC-spezifisch:** die `build/`-Ordner enthalten absolute Pfade des alten PCs (`C:\Users\kara\Documents\HDR-Launcher`). Vor dem ersten Bauen löschen und neu konfigurieren:

```bat
cd \\synology\Entwicklung\HDR-Launcher\capture-cpp\lumora-capture
rmdir /s /q build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

(Analog für `capture-cpp\lumora-shell`, falls die Shell selbst gebaut werden muss.)
Der gebaute Helfer landet in `build\Release\lumora_capture.exe` → zum Testen nach `bin\lumora-capture-native.exe` kopieren (das startet die installierte/laufende App).

**Prebuilt statische Libs:** `third_party\opus\build\Release\opus.lib` und `third_party\libvpl\build\Release\vpl.lib` werden gelinkt (siehe `lumora-capture/CMakeLists.txt`). Wenn sie auf der NAS-Kopie fehlen, opus/libvpl einmal nachbauen (CMake, jeweils `build/Release`).

## 7. Minimale Tool-Liste für den AMD-PC

Nur das Nötigste zum Bauen + Debuggen (kein Signieren/Packen):

1. **Claude Code CLI** — für die lokale Session.
2. **Visual Studio 2022 Build Tools** mit Workload **„Desktop development with C++"** — enthält:
   - MSVC v143 Compiler (`cl.exe`),
   - **Windows 11 SDK** (≥ 10.0.22000 — für Windows.Graphics.Capture / D3D11 / Media Foundation),
   - **C++ CMake-Tools** (liefert `cmake.exe`).
   - Download: aka.ms/vs/17/release/vs_BuildTools.exe
3. *(optional)* **Git für Windows** — für Diffs/Commits (Repo liegt auf NAS, nicht zwingend).

**Schon im Repo/bin vorhanden, NICHT extra installieren:**
- Alle SDK-Header: `capture-cpp/third_party/` (AMF, nv-codec-headers, opus, libvpl, webview2, json).
- **ffmpeg zum Analysieren:** `bin/ffmpeg.exe` (für die Diagnose in Schritt 5).
- mediamtx: `bin/mediamtx.exe`.
- AMF-Laufzeit steckt im AMD-Grafiktreiber (auf dem Gerät vorhanden).
- WebView2-Runtime: auf Win11 vorinstalliert (ggf. prüfen).

**Nicht nötig:** Node.js, NSIS, signtool, electron-builder.

## 8. Datei-/Code-Landkarte

- **Capture-Helfer:** `capture-cpp/lumora-capture/main.cpp`
  - `struct AmfEncoder` ~Z.138 (AMD), `struct NvencEncoder` ~Z.103 (Referenz, funktioniert), `struct QsvEncoder` ~Z.168 (Intel).
  - Ausgabegröße/Mindestgröße: ~Z.656.
  - Render-/Encode-Schleife + MPEG-TS-Muxing: ~Z.722–792 (`for (auto& au : aus)` = eine PES-AU pro Encoder-Buffer).
  - Argument-Parser: ~Z.567–582.
- **Native Shell (startet Helfer, WHEP-Vorschau, mediamtx):** `capture-cpp/lumora-shell/main.cpp`.
- **Logs auf dem Gerät:** `%TEMP%\lumora-stream.log`, `lumora-bitrate.txt`, `lumora-source.txt`, `lumora-hdr.txt`.

## 9. Arbeitsregeln (wichtig)

- **Nicht raten.** Jeder Fix braucht Log-/ffmpeg-Beweis. Nicht zwei Fix-Versuche ohne neuen Beweis dazwischen.
- Reasoning/Denken auf Deutsch.
- Dateinamen rein ASCII (keine Umlaute); UI-Text darf Umlaute.
- Nach jeder Helfer-Änderung: neu bauen → nach `bin/` kopieren → **installierte/laufende App** testen (nicht nur den Build).
