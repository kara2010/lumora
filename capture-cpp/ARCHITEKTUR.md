# Architektur-Notizen Lumora (nativ)

Stand: 2026-08-03. Ergebnis einer vollstaendigen Durchsicht nach der Fehlerserie
Ende Juli / Anfang August 2026.

## Wie das Projekt aufgebaut ist

```
Shell (UI + Orchestrierung)  ->  lumora-shell.exe
  ├─ Capture (Encode)         ->  bin/lumora-capture-native.exe   eigener Prozess
  ├─ Relay (WebRTC/WHEP)      ->  bin/lumora-media-relay.exe      eigener Prozess
  ├─ Rechte-Helfer            ->  bin/lumora-elevate.exe          eigener Prozess
  └─ Broker (FPS/Sensor/Analyse) -> lumora-shell.exe --*-broker   geplante Aufgaben
```

**Diese Trennung ist bewusst und hat sich bewaehrt.** Ein Relay-Absturz reisst die
Oberflaeche nicht mit, der Mess-Broker hat eine eigene Crash-/Perf-Domaene, und die
Nahtstellen sind wenige und explizit:

| Vertrag | Konkret |
|---|---|
| Steuerdateien (%TEMP%) | lumora-bitrate.txt, lumora-codec.txt, lumora-hdr.txt, lumora-source.txt |
| Shared Memory | LumoraOSDFps, LumoraOSDSense, LumoraOSDAnalyze, LumoraShellSingleton |
| HTTP-Kontrolle (Relay) | /v3/paths/get/live, /v3/webrtcsessions/list, /v3/webrtcsessions/kick/, /v3/config/codec, /v3/config/ice |
| UI <-> Shell | 82 IPC-Kanaele ueber WebMessage (SHIM_JS emuliert ipcRenderer) |

Keine dieser Entscheidungen wuerde ich zurueckdrehen.

## Vorgemerkt: WebView2-Fenster zusammenfuehren

**Der einzige belegte Strukturfehler.** Sechs Stellen erzeugen ein WebView2-Fenster
nach demselben Muster; zwischen Gaming-OSD und Analyse-OSD sind ~34 von ~70 Zeilen
woertlich identisch. Mit der Analyse-Werkbank (2026-08-04, sechstes Fenster,
ANALYSE-WERKBANK-PLAN.md) ist der Konsolidierungsdruck bewusst weiter gestiegen -
die Werkbank ist ein normales resizables Fenster ohne Composition/Click-Through
und waere beim Zusammenfuehren der einfachste Fall.

Betroffen: `createDoormanWindow`, `createOsdWindow`, `createAnalyzeOsdWindow`,
`createOsdEditWindow`, `createWerkbankWindow`, Hauptfenster in `wWinMain`.

**Was es real gekostet hat** (nicht theoretisch):

| Commit | Fehler |
|---|---|
| 67a44c7 | Klick-durch nur im Gaming-OSD gefixt, Analyse-OSD blieb kaputt |
| db76994 | Transparenz/Groesse griffen beim ERSTEN Oeffnen nur in einem der beiden |
| 64dcbc3 | Live-Vorschau nachtraeglich vom Analyse-OSD ins Gaming-OSD nachgezogen |
| ca881ac | Positions-Wahl nachtraeglich angeglichen |
| f556844 | recentFt gefixt, die zweite Lesestelle (stats()) blieb kaputt |

**Warum noch nicht gemacht:** In diesen fuenf Stellen stecken teuer erkaufte Details -
WS_EX_NOREDIRECTIONBITMAP ohne SetLayeredWindowAttributes, der Gamepad-Stub im Shim
(sonst fliegt der Xbox-Dongle raus), Composition- vs. normaler Controller,
Click-Through ueber Chromium-Zwischenfenster, Sichtbarkeit vor/nach dem ersten
ShowWindow. Ein Fehler beim Zusammenfassen trifft alle sechs Fenster gleichzeitig,
und OSD-Overlays lassen sich nicht per Screenshot pruefen - das muss ein Mensch am
Bildschirm sehen.

**Wann angehen:** Wenn am OSD ohnehin gearbeitet wird - dann ist der Umbau Teil der
Arbeit statt zusaetzliches Risiko. Aufwand: knapper Tag plus vollstaendige Durchprobe
aller sechs Fenster (Gaming-OSD im Spiel, Analyse-OSD waehrend einer Messung, Editor,
Werkbank, Tuersteher beim Streamen, Hauptfenster).

**Bis dahin:** Querverweise stehen an allen sechs Stellen im Code (Suchbegriff
`GESCHWISTER-FENSTER`).

## Bewusst NICHT aufgeraeumt

`index.html` (6.094 Zeilen, 80 % JavaScript) und `main.cpp` (5.600 Zeilen) sind gross.
Aber: **Kein Fehler der Juli/August-Serie kam von der Dateigroesse.** Datenordner,
PowerShell-Timeout, VC++-Laufzeit, Xbox-Icons - durchweg FEHLENDER Code, nicht schlecht
organisierter. Eine Aufteilung haette davon nichts verhindert, kostet aber Zeit und
bringt Regressionsrisiko. Ohne belegten Nutzen kein Umbau.

## Stolperstein: Capture/Relay landen NICHT automatisch im Release

`build-installer.ps1` baut **nur die Shell** neu. Capture, Relay und Elevate werden
aus `bin/` KOPIERT, nicht gebaut. Wer eine dieser Komponenten aendert, muss das neue
Binary von Hand nach `bin/` legen (Vorgaenger sichern) - sonst committet man den Fix,
aber das naechste Release enthaelt still das ALTE Binary.

Konkret schon passiert: der fps==0-Fix im Capture (c60c990) und der Relay-Toolset-
Neubau. Ablauf beim naechsten Mal:

```
# Capture nach Aenderung:
cmake --build capture-cpp/lumora-capture/build-vs22 --config Release
cp bin/lumora-capture-native.exe bin/lumora-capture-native.vor-<grund>.exe   # sichern
cp capture-cpp/lumora-capture/build-vs22/Release/lumora_capture.exe bin/lumora-capture-native.exe
# (Umbenennung capture -> capture-native macht der Kopierschritt, nicht der Build)
```

Der Zeitstempel-Check aus RELEASE.md Schritt 6 faengt das ab - aber nur, wenn man ihn
macht. Besser waere, build-installer die Capture-/Relay-Binaries selbst bauen zu lassen
(dann kann nichts driften). Vorgemerkt, nicht dringend.

## Offene Risiken

- **ViGEmBus ist archiviert** (Nov. 2023, Markenrechtsstreit, kein benannter Nachfolger).
  Die Eingabe-Bruecke haengt daran. Kein Handlungsdruck, solange Windows die Signatur
  akzeptiert - aber es gibt niemanden mehr, der bei einer Aenderung neu signiert.
- **IPC-INVENTAR.md ist veraltet** (Stand 19.7., 18 von 82 Kanaelen fehlen). Es war die
  Umstiegs-Checkliste und hat seinen Zweck erfuellt. Entweder generieren statt pflegen -
  oder als erledigt markieren.

## Die eigentliche Lehre aus der Fehlerserie

Was uns Fehler gekostet hat, war **nicht** Struktur, sondern stillschweigende Annahmen
ueber die Umgebung: "der Datenordner existiert" (tat er nur, weil Electron ihn angelegt
hatte), "die Laufzeit ist da", "das Icon steckt in der Exe". Vier Code-Durchsichten
haben davon keinen einzigen gefunden - sie pruefen den Code gegen sich selbst.

Gefunden wurden sie durch **feindliche Zustaende**: Ordner loeschen, frische Installation
ohne Vorgeschichte, Rechte entziehen. Das gehoert vor jedes Release, nicht noch eine
Code-Durchsicht.

Was daraus schon umgesetzt ist:
- Uebersetzungs-Check bricht den Build ab (`_testlab/i18n-check.js`, Schritt 0)
- `--test-datadir`, `--test-runcapture`, `--test-analyze`, `--test-icon` als Selbsttests
- writeFile legt fehlende Ordner an und protokolliert Fehlschlaege mit Fehlercode
