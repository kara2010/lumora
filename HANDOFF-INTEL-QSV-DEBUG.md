# GELÖST (2026-07-21): QSV-Encoder-Init schlägt auf Intel-Hardware fehl

**Status: BEHOBEN.** Root Cause gefunden und gefixt in `capture-cpp/lumora-capture/main.cpp`
(`struct QsvEncoder`). Zusammenfassung, Details unten in der ursprünglichen Übergabe stehen
gelassen als Beweis-/Debug-Historie.

## Root Cause

`MFXVideoENCODE_Init` schlägt auf diesem Intel-Gerät (Hybrid-GPU: Intel UHD Graphics + NVIDIA
RTX 2060) **ausschließlich** dann mit `status=-15` (`MFX_ERR_INVALID_VIDEO_PARAM`) fehl, wenn
`par.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY` (D3D11-Zero-Copy) gesetzt ist — unabhängig von
JEDEM getesteten `mfxVideoParam`-Feld (CodecProfile/Level, LowPower, BufferSizeInKB, GOP,
AsyncDepth, TargetUsage, RateControlMethod/CQP, NumRefFrame — alle einzeln isoliert getestet,
`status=-15` blieb identisch). Auch Adapter-Mismatch, fehlendes
`D3D11_CREATE_DEVICE_VIDEO_SUPPORT`, fehlende `ID3D10Multithread::SetMultithreadProtected(TRUE)`
und ein komplett frisches/unbenutztes D3D11-Device wurden ausgeschlossen. Mit
`MFX_IOPATTERN_IN_SYSTEM_MEMORY` (sonst identische Session/Params) initialisiert der Encoder
zuverlässig. Fazit: eine echte Einschränkung/ein Bug dieser konkreten Intel-Treiber-/
oneVPL-Runtime-Version (`ApiVersion 1.30`, Impl `"mfxhw64"`) im D3D11-Zero-Copy-Encode-Pfad,
kein App-seitiger Parameterfehler.

Zusätzlich (zweiter Bug, nach dem IOPattern-Wechsel aufgetreten): die moderne
"Internal-Allocation"-Convenience-API `MFXMemory_GetSurfaceForEncode` liefert bei
`SYSTEM_MEMORY` auf dieser HARDWARE-Session `status=-6` (`MFX_ERR_INVALID_HANDLE`), sowohl mit
als auch ohne vorherigem `MFXVideoCORE_SetHandle`. Init selbst war erfolgreich — nur diese
Convenience-Funktion ist auf diesem Treiber für SYSTEM_MEMORY nicht nutzbar.

## Fix

- `IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY` statt `VIDEO_MEMORY`.
- Kein `MFXVideoCORE_SetHandle` mehr (bei SYSTEM_MEMORY nicht nötig, verursachte mit
  `GetSurfaceForEncode` sonst den -6-Fehler).
- Kein Zero-Copy mehr für QSV: `ID3D11Texture2D staging` (STAGING, CPU_ACCESS_READ) +
  `ctx->CopyResource` + `ctx->Flush()` (wichtig — ohne Flush kann `Map()` sehr lange blockieren)
  + `ctx->Map()` liest die NV12-Daten pro Frame auf die CPU.
  Kein Aufruf mehr von `MFXMemory_GetSurfaceForEncode` (siehe zweiter Bug oben) — stattdessen
  klassische, selbstverwaltete `mfxFrameSurface1` mit `Data.Y`/`Data.UV` auf einen eigenen
  Heap-Puffer (`surfBuf`), `Data.Pitch = surfInfo.Width`. Funktioniert unabhängig von der
  GetSurfaceForEncode-Einschränkung, mit jeder Session.
- NVENC/AMF unverändert Zero-Copy (dort kein Problem reproduzierbar).
- Zusätzlich hinzugefügt (spec-konform, auch wenn für sich allein nicht ausreichend):
  `mfxImplDescription.AccelerationMode`-Filter auf `MFX_ACCEL_MODE_VIA_D3D11`,
  `ID3D10Multithread::SetMultithreadProtected(TRUE)` auf dem D3D11-Device.

## Verifiziert (auf echter Hardware, 2026-07-21)

- Bildschirm-Capture UND Fenster-Capture (`--window`) jeweils real getestet: Encoder-Init
  erfolgreich, Prozess läuft normal (kein Neustart-Loop mehr), sauberer Exit.
- Erzeugter Stream via `ffprobe` verifiziert: valides H.264 (`profile=High`), korrekte
  Auflösung/Framerate, alle angeforderten Frames dekodierbar.
- Gebaute exe deployed nach `X:\bin\lumora-capture-native.exe` (bzw. dem aktuellen Laufwerksbuchstaben)
  und `%LOCALAPPDATA%\Programs\Lumora\bin\lumora-capture-native.exe`.

## Nachtrag (2026-07-21, gleicher Tag): Framerate im echten Stream instabil

Nach dem Init-Fix lief der Stream im echten Betrieb (`%TEMP%\lumora-stream.log`), aber die
Framerate schwankte spürbar (~50–66fps statt konstant 60, fiel über die Zeit hinter Echtzeit
zurück) — im Gegensatz zu NVENC im selben Log, das exakt 60,0fps hält. Per Timing-Messung
(`QSV-DEBUG`-Instrumentierung, wieder entfernt) direkt in `encode()` gefunden: `ctx->Map()`
(GPU-Readback der Staging-Textur) kostete im Schnitt 5–6ms (Spitzen bis 12ms),
`MFXVideoCORE_SyncOperation` (Warten auf HW-Encode) im Schnitt 7–8ms (Spitzen bis 13ms) — beide
SOFORT nacheinander pro Frame ausgeführt, macht durchschnittlich ~14,5ms und in Spitzen >16,7ms
(das 60fps-Budget), sobald beide gleichzeitig langsam waren.

**Fix:** Pipelining in `QsvEncoder::encode()` — ein `EncodeFrameAsync`-Submit bleibt bis zum
NÄCHSTEN `encode()`-Aufruf offen; erst dort wird synchronisiert (`AsyncDepth=2` in `init()`).
Der Sync-Wait fällt dadurch praktisch weg, weil die GPU die Kodierung des vorherigen Frames
im Hintergrund längst erledigt hat, während der nächste Frame gelesen/vorbereitet wird.

**Dabei gefundener zweiter Bug** (nicht Performance, sondern Korrektheit): `mfxFrameSurface1
surf` war eine STACK-lokale Variable in `encode()`. Weil `EncodeFrameAsync` jetzt asynchron
über den Funktionsaufruf hinaus läuft (Sync erst beim nächsten Aufruf), las der Treiber
teilweise aus bereits ungültigem/überschriebenem Stack-Speicher — Symptom: `ffprobe` meldete
"missing picture in access unit" / "no frame!" im erzeugten Stream. Fix: `surf` ist jetzt ein
Member (persistiert zwischen Aufrufen), analog zu `surfBuf`/`bsBuf`/`pendingBs`.

**Verifiziert:** 600/600 Frames über 10s exakt bei 60,0fps (jede Sekunde +60, wie bei NVENC),
`ffprobe` meldet keine Fehler mehr, valides H.264. Bildschirm- UND Fenster-Capture getestet.

## Nachtrag 2 (2026-07-21, gleicher Tag): Live-Bitrate-Änderung wirkungslos

Nutzer meldete: Bitrate im laufenden Stream geändert, "hat aber irgendwie nicht so ganz
funktioniert". Ursache: `QsvEncoder` hatte **kein** `setBitrate()` überschrieben und fiel
dadurch auf den No-Op-Default der `Encoder`-Basisklasse zurück (`virtual void
setBitrate(int kbit) {}`). `main()` ruft `encoder->setBitrate(k)` einmal pro Sekunde auf, wenn
sich `%TEMP%\lumora-bitrate.txt` geändert hat, und loggt "Bitrate live -> X kbit" UNABHÄNGIG
vom tatsächlichen Encoder-Ergebnis — für QSV passierte also schlicht nichts, obwohl der Log
Erfolg suggerierte. NVENC (`nvEncReconfigureEncoder`) und AMF (`SetProperty`) hatten das
bereits korrekt implementiert.

**Fix:** `QsvEncoder::setBitrate()` ergänzt, nutzt `MFXVideoENCODE_Reset(session, &par)` (dafür
`mfxVideoParam par` von einer lokalen Variable in `init()` zu einem Member gemacht — Reset
erwartet den vollen Parametersatz, nicht nur das geänderte Feld). Ein noch offenes Pipelining-
Submit (s. oben) wird vor dem Reset synchronisiert/verworfen, da Reset keine ausstehenden
Async-Operationen erlaubt.

**Verifiziert:** Live-Test mit `%TEMP%\lumora-bitrate.txt` auf 20000 dann 3000 kbit gesetzt,
Stream lief ohne Fehler/Absturz weiter (`ffprobe`: 1200/1200 Frames valide). Tatsächliche
Bitrate im erzeugten Stream per `ffprobe`-Paketgrößen nachgemessen: stieg bei Erhöhung sofort
sichtbar an, sank bei Absenkung über ca. 5-10s (mehrere GOP-Zyklen) auf den neuen Zielwert -
das ist normales BRC-Einschwingverhalten (CBR braucht ein paar GOPs zum Konvergieren), kein Bug.

## Nachtrag 3 (2026-07-21, gleicher Tag): Stream bei 25 Mbit komplett eingefroren

Nutzer meldete: Stream gestartet (Bildschirm), Vorschau zeigt dauerhaft "Stream ist offline".
Log zeigte Encoder-Init erfolgreich (kein status=-15 mehr), aber DANACH **keine einzige**
"Frames live"-Zeile und `mediamtx`-UDP-Timeouts alle 15s, ohne Absturz/Neustart-Loop - der
Prozess lief, produzierte aber sichtbar nichts. Der zuletzt aktive Stream lief mit **25 Mbit**
(4K-Preset, vom vorherigen Bitrate-Test übernommen) statt der bisher getesteten 8 Mbit.

Reproduziert per CLI mit `--bitrate 25`: `MFXVideoENCODE_EncodeFrameAsync` liefert bei JEDEM
Frame `status=-5` (`MFX_ERR_NOT_ENOUGH_BUFFER`), obwohl `bsBuf` mit ~4.17MB
(1x Bitrate/Sekunde + 1MB) reichlich gross erschien - die tatsaechlich erzeugten Frames waren
winzig (IDR ~184KB, P-Frames <4KB, nachgemessen NACH dem Fix). Der Treiber prueft `bs.MaxLength`
offenbar gegen einen INTERNEN VBV/CPB-Puffergroessen-Schaetzwert (abhaengig von TargetKbps,
nicht von der tatsaechlichen Ausgabegroesse) - bei 8 Mbit reichte die alte Formel zufaellig
gerade noch, bei 25 Mbit nicht mehr. Da `st != MFX_ERR_NONE` in `encode()` nur `pendingSyncp`
zuruecksetzt (kein Log, kein Absturz) und das Ausbleiben von Output den restlichen Loop nicht
stoppt, verhaelt sich das exakt wie ein "hängender" Prozess ohne jede Fehlermeldung - reproduzier-
und erkennbar NUR durch gezieltes Debug-Logging von `EncodeFrameAsync`'s Rueckgabewert.

**Fix:** `bsBuf`-Groesse von `1x Bitrate/Sekunde + 1MB` auf `3x Bitrate/Sekunde + 2MB` erhoeht.
Empirisch validiert (2x reichte in einem Zwischentest noch nicht ganz, 3x zuverlaessig).

**Verifiziert:** 8/25/50 Mbit real getestet, alle liefern jetzt `status=0` durchgehend, kein
`-5` mehr. 600/600 Frames bei 25 Mbit per `ffprobe` als valides H.264 bestaetigt, konstant 60fps.

## Performance-Hinweis für die Zukunft

QSV nutzt jetzt CPU-Readback statt Zero-Copy (eine zusätzliche GPU→CPU-Kopie pro Frame,
inzwischen per Pipelining/AsyncDepth=2 entschärft). Auf diesem iGPU/Auflösungs-Niveau
(1920x1080@60) mit ~1 Frame Zusatzlatenz stabil, aber bei höheren Auflösungen/Frameraten ggf.
erneut die Map/Sync-Zeiten prüfen. Ein Treiber-Update könnte den VIDEO_MEMORY-Pfad in Zukunft
reparieren — dann ließe sich zu echtem Zero-Copy zurückwechseln.

---

# Ursprüngliche Übergabe (Debug-Historie, vor der Lösung)

**Arbeitsverzeichnis:** `X:\` (`\\synology\Entwicklung\HDR-Launcher`, gleiche NAS-Kopie wie auf dem AMD-PC).
**Ziel:** Der native Stream (C++-Capture-Helfer `lumora-capture-native.exe`) funktioniert auf NVIDIA und AMD (nach mehreren Fixes, siehe unten), aber **auf Intel/QSV schlägt die Encoder-Initialisierung immer fehl** — Bildschirm- UND Fenster-Streaming betroffen. Fehler direkt auf dem Intel-Gerät finden und fixen. Kein Signieren, kein Release, kein Installer nötig — nur bauen und testen.

---

## 1. Symptom (real beobachtet)

Player zeigt sofort "Stream ist offline", egal ob Bildschirm oder Fenster gestreamt wird. Der Helfer-Prozess startet, meldet aber sofort einen Fehler und beendet sich (Endlos-Neustart-Schleife alle 500ms–4s).

## 2. Log-belegter Fund (aus `%TEMP%\lumora-stream.log`, NICHT geraten)

```
nat: Quelle: Bildschirm 0
nat: SOURCE: 1920x1080 -> 1920x1080 (Bild 1920x1080 @ 0,0)
nat: QSV-DEBUG: nach Query: Width=1920 Height=1088 CropW=1920 CropH=1080 TargetUsage=4 RateControlMethod=1 IOPattern=1 TargetKbps=8000 MaxKbps=8000 BufferSizeInKB=0 GopPicSize=60 GopRefDist=1 IdrInterval=0 AsyncDepth=1 FourCC=842094158 ChromaFormat=1 PicStruct=1 FrameRate=60/1 NumSlice=0 CodecProfile=0 CodecLevel=0 NumRefFrame=0
nat: QSV-DEBUG: MFXVideoENCODE_Init fehlgeschlagen, status=-15
nat: FEHLER: Encoder-Init (QSV) fehlgeschlagen
nat beendet (2) -> Neustart in 500ms
```

**`status=-15` = `MFX_ERR_INVALID_VIDEO_PARAM`** (oneVPL/Intel Media SDK). `MFXVideoENCODE_Query()` selbst meldet KEINEN Fehler/keine Warnung (kein `QSV-DEBUG: MFXVideoENCODE_Query meldet status=...` in der Zeile davor) — erst `MFXVideoENCODE_Init()` lehnt die Parameter ab. Das ist reproduzierbar bei JEDEM Versuch (Bildschirm 1920x1080, Fenster 1408x742, Fenster 1294x741, Fenster 1920x1020 — alle scheitern identisch mit `status=-15`).

**Wichtig:** Die alte Electron/FFmpeg-Version (`h264_qsv` über FFmpeg-CLI, komplett anderer Code) lief auf genau diesem Intel-System einwandfrei. Das Problem ist also NICHT die Hardware/der Treiber generell, sondern spezifisch der neue native oneVPL/MediaSDK-Code in `capture-cpp/lumora-capture/main.cpp`.

## 3. Was schon versucht wurde (und NICHT geholfen hat)

Beide Änderungen in `QsvEncoder::init()` (`capture-cpp/lumora-capture/main.cpp`, Suche nach `struct QsvEncoder`):

1. **`par.mfx.MaxKbps` zusätzlich zu `TargetKbps` gesetzt** (Verdacht: manche Intel-Treiber verlangen bei CBR beide Werte). Kein Effekt — weiterhin `status=-15`.
2. **`mfxExtCodingOption2`-Ext-Buffer (RepeatPPS) komplett entfernt** (auskommentiert, `par.NumExtParam` nicht mehr gesetzt). Ergebnis noch nicht zurückgemeldet — das ist der Stand, mit dem diese Übergabe beginnt (Hash `6a576c29...` in `X:\bin\lumora-capture-native.exe`, bereits deployed).

**Falls Punkt 2 (Ext-Buffer weg) den Fehler behoben hat:** Dann war `mfxExtCodingOption2`/`RepeatPPS` der Auslöser. Fix dauerhaft machen (Ext-Buffer weglassen oder durch eine andere Methode für periodische SPS/PPS-Wiederholung ersetzen, z.B. `MFX_EXTBUFF_CODING_OPTION` statt `_OPTION2`, oder gar keine — SPS/PPS wird ohnehin beim ersten IDR gesendet).

**Falls Punkt 2 NICHT geholfen hat:** Weiter unten nachschauen (Abschnitt 4).

## 4. Verbleibende Hypothesen (auf dem Intel-Gerät zu prüfen, NICHT geraten — jede braucht neuen Log-Beweis)

Alle Basis-Parameter im Dump oben sehen laut oneVPL-Spezifikation gültig aus (Width/Height 16-aligned, CropW/H korrekt, TargetUsage=4=BALANCED gültig, RateControlMethod=1=CBR gültig, IOPattern=1=VIDEO_MEMORY gültig, FourCC=NV12 korrekt, ChromaFormat=1=YUV420 korrekt, PicStruct=1=PROGRESSIVE korrekt). Auffällig: **`CodecProfile=0` und `CodecLevel=0`** (beide nie explizit gesetzt, `MFX_PROFILE_UNKNOWN`/`MFX_LEVEL_UNKNOWN`) — das ist normalerweise gültig (Encoder soll automatisch wählen), aber möglicherweise verlangt DIESE Intel-Treiber-/Runtime-Generation explizite Werte. Nächster Test: `par.mfx.CodecProfile = MFX_PROFILE_AVC_BASELINE;` und ein passendes `par.mfx.CodecLevel` (z.B. `MFX_LEVEL_AVC_42`) explizit setzen.

Weitere Kandidaten, falls das auch nicht hilft:
- **Adapter-Mismatch:** Der D3D11-Device (`dev`) wird explizit gegen den Intel-Adapter erzeugt (`D3D11CreateDevice(pick.get(), ...)`, `pick` = per VendorID 0x8086 gefundener Adapter, s. Zeile mit `useQsv ? intelA : ...`). Die oneVPL-Session (`MFXCreateSession(loader, 0, &session)`) wählt aber Implementierung **Index 0** aus der Loader-Enumeration — falls das System mehrere GPUs hat (z.B. Intel-iGPU + eine dedizierte Karte), könnte Index 0 eine ANDERE Implementierung als die zum `dev`-Adapter passende sein. Prüfen: `MFXEnumImplementations` mit Adapter-/LUID-Filter statt blind Index 0, oder `mfxImplDescription.VendorImplID`/`DeviceID` filtern.
- **BufferSizeInKB=0**: bei manchen Treibern muss dieser Wert für CBR explizit gesetzt werden (nicht 0 = "auto").
- **oneVPL-Runtime-Version/Dispatcher**: prüfen, welche tatsächliche Implementierung geladen wird (`MFXQueryImpl`/`MFXQueryVersion` nach `MFXCreateSession` loggen) — falls versehentlich eine Software- statt Hardware-Implementierung geladen wird (trotz `MFX_IMPL_TYPE_HARDWARE`-Filter), könnte das die Fehlerursache sein.

## 5. Arbeitsregeln (wichtig, aus der Erfahrung auf dem AMD-PC)

- **Nicht raten.** Jeder Fix braucht Log-Beweis (die `QSV-DEBUG:`-Zeilen sind bereits eingebaut, bei Bedarf weitere `printf`s ergänzen). Nicht mehrere Fix-Versuche ohne neuen Beweis dazwischen probieren.
- Nach jeder Code-Änderung: **neu bauen → `build\Release\lumora_capture.exe` nach `bin\lumora-capture-native.exe` UND in den `bin`-Ordner der installierten App kopieren → echten Stream-Start testen** (nicht nur `--list`).
- **Windows Defender / Smart App Control (WDAC) kann frisch gebaute, unsignierte `.exe` blockieren** ("Eine Anwendungssteuerungsrichtlinie hat diese Datei blockiert" / Event-ID 3077 in `Microsoft-Windows-CodeIntegrity/Operational`). Auf dem AMD-PC trat das mehrfach auf; ein erneuter Rebuild (neuer Hash) hat es meist behoben. Falls das auch hier auftritt: `& "<pfad>\lumora_capture.exe" --list` direkt testen (nicht nur über die App), um das von einem echten Encoder-Bug zu unterscheiden.
- Reasoning/Denken auf Deutsch. Dateinamen ASCII, UI-Text darf Umlaute.
- `mtx: 2026/07/21 ... ERR [path live] [MPEG-TS source] read udp 127.0.0.1:8558: i/o timeout` im Log ist NUR ein Folge-Symptom (kein Encoder liefert Daten, weil Init scheitert) — nicht die Ursache.

## 6. Datei-/Code-Landkarte

- **Capture-Helfer:** `capture-cpp/lumora-capture/main.cpp`
  - `struct QsvEncoder` (Suche danach) — hier liegt der Bug. `init()` enthält bereits ausführliches `QSV-DEBUG:`-Logging.
  - `struct AmfEncoder`, `struct NvencEncoder` — funktionieren (auf AMD verifiziert), NICHT die Fehlerquelle.
  - Adapter-Auswahl (`useQsv`/`intelA`/`pick`) ~Zeile 635–643.
- **Bereits auf AMD verifizierte, encoder-übergreifende Fixes (NICHT anfassen, betreffen auch QSV):**
  - GOP = 1s statt 2s (`GopPicSize = fps` bei QSV, entsprechend bei AMF/NVENC).
  - Minimiert-Fenster-Fix (`GetWindowPlacement` statt WGC-Platzhaltergröße) + Resize-Restart bei deutlicher Größenänderung (encoder-agnostisch, betrifft auch QSV mit).
- **Logs auf dem Gerät:** `%TEMP%\lumora-stream.log`.
- **Build:** `cd capture-cpp\lumora-capture`, `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` (falls `build\` fehlt/PC-fremd ist — absolute Pfade im Cache sind PC-spezifisch, im Zweifel `build\` löschen und neu konfigurieren), `cmake --build build --config Release`. Gebauter Helfer landet in `build\Release\lumora_capture.exe`.
- **Installierte App (Ziel zum Testen):** analog zum AMD-PC unter `%LOCALAPPDATA%\Programs\Lumora\bin\lumora-capture-native.exe`.

## 7. Aktueller Stand bei Übergabe

`X:\bin\lumora-capture-native.exe` (Hash `6a576c29...`) hat den `mfxExtCodingOption2`-Ext-Buffer entfernt — **Ergebnis auf dem Intel-Gerät noch nicht getestet/zurückgemeldet**. Das ist der nächste Schritt: diese Version (oder frisch von diesem Stand aus weiterbauen) auf dem Intel-PC testen und Log prüfen.
