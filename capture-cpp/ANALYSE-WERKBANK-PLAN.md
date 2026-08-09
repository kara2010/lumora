# Analyse-Werkbank — eigenständige Oberfläche für die Ruckler-Analyse

Stand: 2026-08-04 · Branch `feature/analyse-werkbank` · Vorgänger-Doc: `BOTTLENECK-PLAN.md`

## Warum

Die Analyse ist Lumoras Alleinstellungsmerkmal, wohnt aber als Reiter zwischen
„Overlay" und „Stream" — die Verpackung sagt „Einstellung", der Inhalt ist das
Feature. Konkrete Defizite des Reiters:

- **Kein Zoom.** Die Frametime-Kurve ist ein statisches Canvas in Panelbreite;
  echte Analyse lebt vom Hineinzoomen („was ist in diesen 200 ms passiert?").
- **Daten reicher als Darstellung.** Der Broker sammelt CPU-Buckets, DPC/ISR je
  Treiber, Disk, Prozessstarts, GPU-Throttle — gezeigt wird eine Verdächtigen-
  Liste. Die zeitliche KORRELATION (Kernaussage!) wird nie visuell.
- **Vergleich ist eine Tabelle**, überzeugender wären überlagerte Kurven.

Ziel: eigenes Fenster **„Analyse-Werkbank"** mit Profiler-Leitmetapher
(zoombare Timeline + synchronisierte Spuren + Beweis-Panel), in der
„Messgeräte"-Designsprache von analyze-osd/bericht.css. Jeder Screenshot
daraus ist ein Marketing-Asset.

## Ist-Stand der Daten (gemessen, nicht vermutet)

- `allFtT_`/`allFt_` (stutter_analyzer.h:389): volle Frametime-Serie über die
  GANZE Session existiert bereits im Speicher (~2 MB bei 30 min/144 fps).
  Im Report landet nur die Eindampfung `ftSeries(2000)` (max-Binning).
- Alle übrigen Signale leben NUR in ~10-s-Ringen (frames 4096, dpcs 16384,
  disks 4096, procs 256, sensors 128 @10 Hz, cpu 256 @50 ms). Für Spuren über
  die ganze Session fehlen Akkumulatoren.
- Namensauflösung existiert als Callbacks (`cfg_.pidName`, `cfg_.driverName`).
- Report-Writer: osd_broker.h ~Z.260-352, schreibt atomar (.neu + MoveFileEx).
- Bericht-Rendering geteilt über `analyze-report.js` (App + Website) — die
  Werkbank nutzt dieselben Bausteine (esc/verdict/Farben), zeichnet die
  Timeline aber selbst (eigenes Modul, s.u.).

## Datenmodell v2 (Etappe 1 — der eigentliche Umbau)

Report-JSON bekommt `"version": 2` plus neue Felder. **Alte v1-Reports bleiben
lesbar**: Werkbank fällt dann auf `ftSeries` (2000 Punkte, ohne Zoom in die
Tiefe) zurück und blendet die fehlenden Spuren aus.

### ftFull — volle Frametime-Serie, kompakt
```json
"ftFull": { "t0": 0.34, "q": [712, 698, 4123, ...], "sync": [[512, 3.72], ...] }
```
- `q`: Frametimes in **10-µs-Einheiten** (Integer, JSON-kompakt; 7,12 ms = 712).
- Zeitachse rekonstruiert kumulativ: `t[i+1] = t[i] + q[i]/100000` — die
  Frametime IST das Present-Delta, die Rekonstruktion also strukturell exakt.
- Quantisierungsdrift (max 10 µs/Frame): `sync`-Stützpunkte alle 512 Frames
  `[index, t_sekunden]` ziehen die Achse wieder gerade.
- Größe: 30 min @144 fps ≈ 260k Werte ≈ 1,3 MB JSON. Ok für lokale Reports.

### tracks — Spuren über die ganze Session (NEUE Akkumulatoren im Analyzer)
Feste Kadenz **250 ms** (Kompromiss Auflösung/Größe; Spike-Detail liefern die
±200-ms-Fenster der findings, nicht die Spuren):
```json
"tracks": {
  "hz": 4,
  "cpu":  { "totalPct": [..], "maxCorePct": [..], "maxCore": [..], "topPid": [..], "topPct": [..] },
  "gpu":  { "clockMHz": [..], "loadPct": [..], "vramMB": [..], "throttle": [..] },
  "dpc":  { "maxUs": [..], "maxDrv": [..], "count": [..] },
  "disk": { "kb": [..], "maxLatMs": [..] },
  "procEvents": [[t, pid, 1|0], ...],
  "limits": [..]        // Limit-Klassifikation je Sekunde (Index = Sekunde)
}
```
- Arrays gleicher Länge (Index = Bucket), fehlend = -1. Integers wo möglich.
- `procEvents` als Ereignisliste (selten), nicht als Spur.
- Namens-Tabellen einmalig am Ende aufgelöst:
  `"names": { "procs": {"1234": "MsMpEng.exe"}, "drivers": ["nvlddmkm.sys", ...] }`
- RAM-Kosten Akkumulatoren: 30 min = 7200 Buckets × ~40 B ≈ 300 KB. Unkritisch.
- CSwitch-Callback bleibt UNANGETASTET (0 Allokationen — Overhead-Garantie!);
  die 250-ms-Verdichtung zieht ihre Werte aus den vorhandenen 50-ms-CpuBuckets
  im Analyse-Worker (BELOW_NORMAL), nicht im ETW-Thread.

### Größen-Disziplin
Gesamtreport v2 ≈ 2 MB (heute ~50 KB). Bewusst akzeptiert (lokal). Der
GETEILTE Bericht (falls je reaktiviert) schickt weiterhin NUR das v1-Subset —
`anShareClean` schneidet `ftFull`/`tracks`/`names` ab (Serverlimit 256 KB
bleibt Wächter).

## Fenster (Etappe 1b)

- `analyze-werkbank.html` (Root) + eigenes **windowed** WebView2-Fenster in
  main.cpp nach osd-edit-Muster: eigene Globals (g_wbHwnd/Ctrl/Wv), normale
  Fensterdeko (resizable, min 1100×700), KEIN Topmost, KEIN Click-Through.
- **Sechstes Geschwister-Fenster** → Querverweis-Kommentare an ALLEN Stellen
  nachziehen (bisher 5), ARCHITEKTUR.md ergänzen (Konsolidierung bleibt
  aufgeschoben, die Werkbank macht sie dringlicher).
- Mappings wie Hauptfenster: `app.lumora` → Programmordner, `data.lumora` →
  %APPDATA%\lumora. Die Werkbank lädt Reports DIREKT per
  `fetch('https://data.lumora/analyze/report-… .json')` — kein IPC-Umweg für
  Megabyte-Payloads.
- IPC: `analyze-werkbank <file|null>` öffnet/fokussiert das Fenster (Datei als
  Query-Parameter beim Navigate). Fenster schließen = verstecken (schneller
  Wiederauf), Prozessende räumt ab.
- Reiter wird schlank: Start/Stop/Hotkey/Status + Verlauf bleibt, Berichts-
  DETAILANSICHT bekommt Knopf **„In der Werkbank öffnen"**. Nichts entfernen,
  was per Gamepad gebraucht wird (Messen bleibt Sofa-tauglich; Auswerten ist
  Maus/Tastatur — Gamepad-Navigation der Werkbank bewusst NICHT in v1).
- **build-installer.ps1**: analyze-werkbank.html in die Staging-Liste UND den
  Ressourcen-Wächter auf alle ausgelieferten HTML-Dateien ausweiten (er prüft
  bisher nur index.html — exakt die Lücke, die den 3.3.0-Bootfehler baute).

## Timeline (Etappe 2)

Eigenes Modul `werkbank-timeline.js` (nur Werkbank; analyze-report.js bleibt
die gemeinsame Quelle für Bericht/Website und wird NICHT aufgebläht):

- Canvas-2D, DPR-scharf, keine externen Libs (offline/CSP).
- **Viewport** {t0,t1} als einzige Wahrheit; Mausrad zoomt auf Cursorposition,
  Ziehen = Pan, Doppelklick = Gesamtansicht. Alle Spuren teilen den Viewport.
- **LOD-Rendering**: min/max-Binning pro Pixelspalte aus ftFull (vorberechnete
  Pyramide: 1×, 8×, 64×, 512× — Auswahl nach Zoomstufe), damit 260k Punkte
  flüssig bleiben. Spuren (7200 Punkte) brauchen kein LOD.
- Spurleisten unter der Hauptkurve: CPU (total + max-Kern), GPU (Takt+Last),
  DPC (max µs, Treiberfarbe), Disk, Prozess-Ereignisse als Marker.
- Spike-Marker aus findings: klickbar → Etappe 3-Panel; Hover = Kurzinfo.
- Achsen: ms links (Hauptkurve), Zeit unten (mm:ss), 60/30-fps-Hilfslinien
  MIT Einheit (Lehre aus dem Berichts-Fehler).

## Beweis-Panel, Lauf-Browser, Vergleich (Etappe 3)

- **Beweis-Panel** rechts: beim Spike-Klick ±200-ms-Zoom + Verdächtige des
  findings mit Evidenztext; Buttons „vorheriger/nächster Ruckler".
- **Lauf-Browser** links: alle Sessions (`analyze-list`-IPC wie im Reiter),
  Mini-Kennzahlen (Datum, Spiel, Dauer, R/min, Urteil-Farbpunkt), Notiz.
- **Vergleich**: zweiten Lauf anhaken → beide Kurven überlagert (B halbtransparent),
  Delta-Band p50/p99, Klartext-Urteil (vorhandene Vergleichslogik wiederverwenden).
  Warnung bei abweichender meta (anderes Spiel/Auflösung/Streaming) wie bisher.
- i18n: alle neuen Texte beidseitig; i18n-check.js prüft nur index.html →
  auf analyze-werkbank.html ausweiten (eigenes I18N_EN dort oder gemeinsame
  Quelle — Entscheidung in Etappe 1b, Voreinstellung: gemeinsames Muster wie
  analyze-osd.html es heute macht).

## Reihenfolge + Abnahme

1. **Etappe 1**: Datenmodell v2 im Broker/Analyzer. Abnahme: echter Messlauf
   auf dem 5090-System erzeugt v2-Report; Rekonstruktion ftFull == Stichprobe
   gegen ftSeries; Reportgröße < 4 MB; Overhead-Selbstcheck unverändert
   (CSwitch-Pfad unberührt); alte v1-Reports weiter ladbar im Reiter.
2. **Etappe 1b**: Fenster öffnet aus dem Reiter, lädt einen Lauf, zeigt
   Kennzahlen-Kopf (noch ohne Timeline). Installer-Wächter erweitert.
3. **Etappe 2**: Timeline + Spuren, flüssig bei 260k Punkten (Ziel: Pan/Zoom
   ohne sichtbares Ruckeln auf dem 5090-System), Spike-Marker klickbar.
4. **Etappe 3**: Beweis-Panel + Browser + Vergleich. i18n-Gate grün.

Release erst nach echtem Forza-Lauf-Test; Version dann 3.3.0 (echtes Minor).

## Risiken

- **Reportgröße**: v2 ~2 MB. Falls Sessions > 1 h üblich werden: ftFull ab
  500k Frames auf 20-µs-Quantisierung + halbe Sync-Dichte (im Writer kapseln).
- **Teilen-Feature schlummert**: anShareClean MUSS v2-Felder strippen, sonst
  läuft ein reaktivierter Upload ins 256-KB-Limit (Test dafür gleich in
  Etappe 1 mitschreiben, nicht erst bei Reaktivierung).
- **6. WebView2-Fenster** erhöht den Konsolidierungsdruck (ARCHITEKTUR.md).
- **Ringe ≠ Session**: Die ±200-ms-Spike-Fenster bleiben die einzige
  Feindetail-Quelle unterhalb 250 ms — bewusst (Overhead-Garantie <1 %).
