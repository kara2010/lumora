# Lumora

🇬🇧 [English version](README.md)

Ein nativer, store-übergreifender Game-Launcher für Windows mit Game-Streaming,
einer Eingabe-Brücke, automatischem HDR und Gaming-OSD. Lumora findet
installierte Spiele aus mehreren Stores, zeigt sie in einer einheitlichen,
kuratierten Bibliothek und streamt auf Wunsch ein laufendes Spiel per Link an
jeden Browser. Seit Version 3.0.0 läuft es durchgängig auf eigenem, nativem
C++-Code – ohne Electron, ohne FFmpeg, ohne fremden Streaming-Server – bei
einem Download von rund 3,5 MB.

Download & Infos: <https://lumora-streaming.de/>

## Features

- **Store-übergreifend** – findet Spiele von Steam, Microsoft Store / Xbox,
  EA, Epic, GOG u. a. und scannt automatisch alle Laufwerke.
- **Game-Streaming per Link** – ein laufendes Spiel direkt an jeden Browser
  streamen (eigene C++-Aufnahme + Hardware-Encoder → eigenes WebRTC/WHEP-Relay).
  Standardmäßig Full HD (1080p60), auf Wunsch bis 4K; AV1 oder H.264 je nach
  Decoder des Zuschauers; direkte Peer-to-Peer-Verbindung (inklusive
  IPv6-Direktweg); Gruppen-Streams mit Raumcodes; auf Wunsch Zutrittsschutz,
  der vor jedem Zuschauer erst fragt.
- **Eingabe-Brücke** – macht aus Joystick, Lenkrad oder Flightstick einen
  virtuellen Xbox-360-Controller (über den signierten Open-Source-Treiber
  ViGEmBus), damit Geräte, die sich als DirectInput melden, auch in Spielen
  funktionieren, die nur XInput akzeptieren. Visuelle Zuordnung, Live-Monitor
  der Eingaben, Auto-Kalibrierung, Achse↔Trigger und Knopf↔Achse, getrennte
  Geräte (Lenkrad + Pedale) und Profile pro Spiel.
- **Automatisches HDR** – aktiviert HDR beim Spielstart und schaltet danach
  zurück (nativ, ohne externes Werkzeug).
- **Gaming-OSD** – FPS, GPU- und CPU-Werte im Spiel, ohne Afterburner & Co.
  (eigene ETW-basierte Frame-Messung).
- **Gamepad-Steuerung** – die gesamte Oberfläche ist per Xbox-Controller
  bedienbar (Couch-Gaming am TV).
- **Cover & Artwork** – Cover, Hero-Banner, Beschreibung, Genre und
  Release-Jahr werden automatisch geladen (Steam, Microsoft Store, optional
  [SteamGridDB](https://www.steamgriddb.com)); passt etwas nicht, wählst du
  selbst aus mehreren Quellen oder legst ein eigenes Bild ab.
- **Bibliothek** – Favoriten anpinnen, Poster- oder Hero-Ansicht, sortieren
  nach Name, Spielzeit oder zuletzt gespielt.
- **Spielzeit-Tracking** – zuverlässige Erfassung pro Spiel, launcher-übergreifend.
- **Komfort** – Autostart, Start im Infobereich (Tray), Akzentfarbe.
- **Signiert** – alle Programmdateien und der Installer sind digital signiert
  (Certum Code Signing).

## Technik

Seit 3.0.0 besteht Lumora aus nativen C++-Helfern (siehe [`capture-cpp/`](capture-cpp/)):

- **lumora-shell** – die App selbst; bettet die Web-Oberfläche (`index.html` /
  `styles.css`) in ein WebView2-Fenster ein und übernimmt Tray, Hotkeys,
  Autostart, Launcher-Scan und Orchestrierung.
- **lumora-capture-native** – Bildschirmaufnahme, Skalierung / HDR und
  Hardware-Encoding (NVENC / AMF / QSV, AV1 & H.264, Opus-Audio), ausgegeben
  als MPEG-TS über UDP.
- **lumora-media-relay** – das eingebaute WebRTC/WHEP-Relay (libdatachannel),
  das den früheren mediamtx-Server ersetzt.
- **lumora-elevate** – ein kleiner Einmal-Helfer für die wenigen Vorgänge,
  die erhöhte Rechte brauchen.

Die Eingabe-Brücke nutzt einen systemweiten virtuellen Controller über
[ViGEmBus](https://github.com/nefarius/ViGEmBus) (auf Wunsch von der offiziellen
Quelle installiert, Signatur geprüft). Nutzerdaten liegen unter
`%APPDATA%\lumora\`. Den signierten NSIS-Installer erzeugt
[`capture-cpp/lumora-shell/build-installer.ps1`](capture-cpp/lumora-shell/build-installer.ps1).

### Entwicklung

Jeder Helfer baut mit CMake (Visual Studio 2022, x64), z. B. die Shell:

```powershell
cmake -S capture-cpp/lumora-shell -B capture-cpp/lumora-shell/build -G "Visual Studio 17 2022" -A x64
cmake --build capture-cpp/lumora-shell/build --config Release
```

Den vollständigen signierten Installer (Shell + gestagete Helfer) baut:

```powershell
cd capture-cpp/lumora-shell
powershell -NoProfile -ExecutionPolicy Bypass -File ./build-installer.ps1
```

Hilfs- und Diagnose-Skripte liegen in [`_testlab/`](_testlab/) und sind vom
Build ausgeschlossen. Die gebündelten Helfer-Binaries unter `bin/` sind nicht
im Repo – sie werden aus [`capture-cpp/`](capture-cpp/) gebaut. Die früheren
Electron-Einstiegspunkte (`main.js`, electron-builder) bleiben als Alt-Pfad im
Baum, den 3.0.0 ablöst.

## Website

Der Ordner [`website/`](website/) enthält die Downloadseite
(lumora-streaming.de/).

## Autor

Lumora wird entwickelt und gepflegt von **Karsten Radermacher**
(GitHub: [kara2010](https://github.com/kara2010)).
Copyright © 2026 Karsten Radermacher.

## Lizenz

Lumora ist freie Software unter der **GNU Affero General Public License v3.0**
([LICENSE](LICENSE)). Das bedeutet: Du darfst den Code nutzen, verändern und
weitergeben – aber jede abgeleitete oder darauf aufbauende Software (auch als
Netzwerkdienst betrieben) muss ihren vollständigen Quellcode unter derselben
Lizenz offenlegen.

**Marke:** Der Name „Lumora", das Logo und das Erscheinungsbild sind **nicht**
Teil der Lizenz. Forks müssen unter eigenem Namen und Branding auftreten und
dürfen nicht den Eindruck erwecken, das offizielle Lumora zu sein oder mit ihm
in Verbindung zu stehen.

## Beiträge

Fehlerberichte und Vorschläge sind als GitHub-Issues willkommen!
**Pull Requests werden derzeit nicht angenommen** – Lumora bleibt bewusst ein
Ein-Personen-Projekt, damit der Autor die alleinige Urheberschaft (und damit
volle Lizenzhoheit über künftige Versionen) behält.
