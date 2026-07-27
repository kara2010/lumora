# Plan: Ruckler-Blackbox + Flaschenhals-Monitor ("Warum ruckelt mein Spiel?")

> Status: **beschlossen, naechstes Feature** (Design-Stand 2026-07-27, noch kein Code).
> Ausloeser: random Mikro-Ruckler in Forza Horizon auf RTX 5090 + 9800X3D - Hardware
> kann es nicht sein, klassische Tools (Task-Manager) sehen nichts. Ziel: Lumora
> benennt die Verdaechtigen automatisch.
> Kernbedingung (nicht verhandelbar): **Das Monitoring darf selbst keine Last erzeugen.**

## 0. Produktversprechen / USP
Zwei Gesichter desselben Datenstroms:
1. **Live-Flaschenhals-Anzeige** (Dauerzustand): "GPU-limitiert (97% busy)" /
   "CPU-limitiert: Kern 4 am Anschlag" / "GPU drosselt: Power-Limit" - optional als
   OSD-Feld direkt im Spiel.
2. **Ruckler-Blackbox** (Ereignis): Bei jeder Frame-Time-Spitze wird das +-200-ms-Fenster
   eingefroren und ausgewertet: welcher Prozess/Treiber/Zustand war GENAU in dem Moment
   auffaellig. Ergebnis = priorisierte Verdaechtigenliste ("bei 8 von 10 Rucklern hatte
   XY eine CPU-Spitze"), kein Urteil - Korrelation, ehrlich als solche benannt.

Kein Launcher/Overlay am Markt beantwortet "warum ruckelt es". Die Suchanfragen dazu
("Spiel ruckelt trotz guter Hardware", "CPU oder GPU Flaschenhals") haben riesiges
Volumen und miserable Antworten (LatencyMon + CapFrameX + ETW-Handarbeit) -> eigene
Landingpage DE/EN, echtes Auffindbarkeits-Asset.

## 1. Geerdeter Ist-Stand (was wir SCHON haben)
- **ETW-Present-Consumer** (`etw_present.h`, PresentMon-Ersatz): jeder Present mit
  Zeitstempel -> Frame-Times sind exakt messbar, inkl. welcher Prozess praesentiert.
  Laeuft im **elevated FPS-Broker** (geplante Aufgabe, Shared Memory `Local\LumoraOSDFps`).
- **Sensorik**: NVML (GPU-Takt/Power/Temp/VRAM - inkl. ThrottleReasons-API), ADL,
  PDH (CPU gesamt + pro Kern moeglich), DXGI QueryVideoMemoryInfo (VRAM-Nutzung),
  PawnIO-Sensor-Broker (CPU-Temp/-Power). Alles in `main.cpp`/`lubroker`.
- **Elevated-Broker-Infrastruktur**: geplante Aufgaben ohne UAC-Prompt pro Sitzung,
  Shared-Memory-Uebergabe an die Shell. Kernel-ETW-Sessions (DPC/ISR) brauchen genau
  diese Rechte - der Unterbau existiert.
- **OSD + UI**: Anzeigeflaeche (OSD-Feld) und Settings-/Reiter-Struktur vorhanden.

## 2. Architektur
```
[elevated Broker (bestehender FPS-Broker, erweitert oder Schwester-Session)]
  ETW-Kernel-Session (NT Kernel Logger oder System-Provider):
    - PERF_DPC / PERF_ISR            -> DPC/ISR-Dauer + TREIBERNAME (Modul)
    - CSwitch (Context Switch)       -> CPU-Zeit pro Prozess in feiner Aufloesung
    - DiskIo                         -> Disk-Bursts (Latenz, Prozess)
    - Process Start/Stop             -> "wer ist genau jetzt gestartet?"
  ETW-Present (existiert)            -> Frame-Times des Vordergrund-Spiels
        |
        v  (lockfreier Ringpuffer, feste Groesse ~10 s, vorallokiert)
  Spike-Detektor: FrameTime > k * gleitender Median (k~3, konfigurierbar)
        |
        v  nur im Spike-Fall
  Fenster-Analyse (+-200 ms aus dem Ringpuffer):
    - Top-CPU-Prozesse im Fenster (CSwitch-Aggregat) vs. Baseline davor
    - DPC/ISR-Spitzen im Fenster (Treibername!)
    - GPU: Taktsprung? ThrottleReason? VRAM-Sprung? (NVML-Proben aus dem Puffer)
    - Disk-Burst? Prozessstart?
        |
        v
  Befund-Datensatz -> Shared Memory / Datei -> Shell -> UI ("Ruckler-Protokoll")
```
- **Live-Flaschenhals** (Gesicht 1) ist eine 1-Hz-Aggregation aus demselben Puffer:
  GPU-Busy vs. FrameTime (GPU- vs. CPU-Limit), Kern-Max vs. CPU-Gesamt
  (Single-Core-Limit), NVML-ThrottleReasons (Power/Thermal), VRAM-Fuellstand.
- Shell bleibt Orchestrator; Broker sammelt und analysiert; UI zeigt an.
  Kommunikation wie gehabt ueber Shared Memory + Dateien (bestehende Muster).

## 3. Konkrete Datenquellen (verifizierbar, kein Neuland)
| Signal | Quelle | Kosten |
|---|---|---|
| Frame-Times + Prozess | ETW Present (EXISTIERT) | laeuft schon |
| GPU-Busy je Frame | ETW DxgKrnl (gleicher Provider-Kreis wie Present; Intel-PresentMon-"GPU Busy"-Methode) | gering, gleiche Session |
| CPU-Zeit je Prozess (fein) | ETW CSwitch | moderat (haeufige Events) - NUR aggregieren, nie einzeln loggen |
| DPC/ISR mit Treibername | ETW PERF_DPC/PERF_ISR | gering |
| Disk-Bursts | ETW DiskIo | gering |
| Prozessstarts | ETW Process | minimal |
| GPU Takt/Power/Temp/VRAM/ThrottleReasons | NVML (EXISTIERT), 10-Hz-Probe | minimal |
| CPU je Kern | PDH \Prozessorinformationen(*)\Prozessorzeit | minimal |
| RAM/Commit | GlobalMemoryStatusEx | minimal |

## 4. Overhead-Budget (die Kernbedingung)
- ETW ist Kernel-seitig gepuffert; der Produzent schreibt unabhaengig vom Konsumenten.
  Unser Konsument: eigener Thread, `THREAD_PRIORITY_BELOW_NORMAL`, Batch-Reads.
- **CSwitch ist der einzige Mengen-Risiko-Provider** (~10-100k Events/s unter Last):
  im Callback NUR aufaddieren (Prozess-ID -> Ticks), keine Allokationen, keine Strings,
  keine Logs. Ringpuffer vorallokiert. Zielbudget: **<1% CPU eines Kerns, 0 Alloc/Frame**.
- Analyse (teurer Teil) laeuft NUR beim Spike und nur ueber den kleinen Fensterausschnitt.
- Messbarer Selbsttest als Abnahmekriterium: Frame-Time-Verteilung eines Spiels MIT vs.
  OHNE aktivem Monitor vergleichen (unsere eigene Messtechnik!) - Abweichung muss im
  Rauschen liegen, sonst kein Release. ("Messen statt raten" gilt auch fuer uns selbst.)
- Feature ist opt-in (Schalter), Blackbox nur bei aktivem Monitoring/Spiel.

## 5. UI/UX
- **OSD-Feld "Limit"** (optional): GPU / CPU / CPU-Kern / PWR / TEMP / VRAM - kompakt.
- **Neuer Reiter "Analyse"** (Arbeitstitel) in der Shell:
  - Live-Ampel: aktueller Flaschenhals + kurze Erklaerung in Klartext.
  - Ruckler-Protokoll: Liste der Spikes (Zeit, Dauer, Spiel) -> aufklappbar die
    Verdaechtigen ("svchost.exe CPU-Spitze", "nvlddmkm.sys DPC 800us", "GPU-Takt
    -400 MHz: Power-Limit"). Haeufung ueber die Session aggregieren ("8/10 Ruckler: X").
  - Export als Text (Forum-/Support-tauglich).
- Sprache: Klartext-Deutsch/Englisch, keine ETW-Fachbegriffe in der Standardansicht.

## 6. Etappen
1. **MVP - Spike-Erkennung + vorhandene Quellen** (Shell/Broker, ohne neue ETW-Provider):
   Frame-Spikes aus dem bestehenden Present-Strom, korreliert mit NVML (Takt/Throttle/
   VRAM) + PDH je Kern + Top-Prozesse per Toolhelp-Delta (grob, 4-Hz-Probe).
   Liefert bereits: GPU-vs-CPU-Limit, Throttling, grobe Prozess-Verdaechtige. ~1-2 Sitzungen.
2. **Kernel-ETW**: DPC/ISR (Treibernamen!) + CSwitch (feine Prozess-Attribution) +
   DiskIo in den Broker; Ringpuffer + Fensteranalyse. Das ist der eigentliche
   LatencyMon-Killer. ~2-4 Sitzungen inkl. Overhead-Nachweis.
3. **GPU-Busy je Frame** (DxgKrnl) fuer die saubere GPU/CPU-Limit-Aussage nach
   Intel-PresentMon-Vorbild. ~1-2 Sitzungen.
4. **UI-Reiter + OSD-Feld + Export**, Website-Landingpage DE/EN. ~1-2 Sitzungen.
- Validierung durchgehend am realen Fall: Forza-Ruckler auf dem 5090/9800X3D-System.

## 7. Risiken / ehrliche Grenzen
- **Korrelation != Kausalitaet**: Wir liefern Verdaechtige mit Haeufigkeits-Evidenz,
  keine Beweise. UI-Sprache entsprechend ("wahrscheinlich", "8/10 Faelle").
- **Spielinterne Ursachen** (Shader-Kompilierung, Asset-Streaming) haben keinen
  externen Taeter - Befund dann ehrlich: "kein Systemstoerer gefunden -> vermutlich
  spielintern" (auch das spart dem Nutzer die halbe Suche).
- **CSwitch-Menge**: einziges echtes Performance-Risiko - Gegenmittel s. Abschnitt 4;
  notfalls CSwitch weglassen (DPC/ISR + Toolhelp-Delta bleiben aussagekraeftig).
- **Nur eine NT-Kernel-Logger-Session systemweit** (klassischer Logger): moderne
  System-Provider-Sessions (Win10 1709+) statt des Legacy-Loggers nutzen, sonst
  Konflikt mit LatencyMon & Co.
- **AMD-GPUs**: ThrottleReasons ist NVML (NVIDIA); fuer ADL/AMD zunaechst nur
  Takt-/Temp-Heuristik - Feature degradiert sauber statt zu fehlen.
- **Elevation**: Kernel-ETW nur im elevated Broker - Muster existiert, aber die
  Blackbox funktioniert ohne Elevation nur in der MVP-Stufe (Etappe 1).

## 8. Offene Entscheidungen
- Name des Features (Arbeitstitel "Ruckler-Blackbox" / "Analyse") - auch fuers Marketing.
- OSD-Feld ab MVP oder erst mit Etappe 3 (saubere GPU-Busy-Basis)?
- Protokoll-Persistenz: nur Session oder Historie ueber Neustarts?
