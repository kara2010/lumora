# Lumora

Ein store-übergreifender Game-Launcher für Windows mit automatischem HDR,
Game-Streaming und Gaming-OSD. Lumora findet installierte Spiele aus mehreren
Stores, zeigt sie in einer einheitlichen, schön kuratierten Bibliothek und
schaltet beim Spielstart automatisch HDR ein.

Download & Infos: <https://lumora-streaming.de/>

## Features

- **Store-übergreifend** – findet Spiele von Steam, Microsoft Store / Xbox,
  EA, Epic, GOG u. a. und scannt automatisch alle Laufwerke.
- **Automatisches HDR** – aktiviert HDR beim Spielstart und schaltet danach
  zurück (via `HDRCmd.exe`).
- **Game-Streaming per Link** – bis 4K/60 fps direkt an jeden Browser
  (FFmpeg-Hardware-Encoder → mediamtx → WebRTC/WHEP), Gruppen-Streams mit
  Raumcodes, adaptive Bitrate, nahtloser Qualitätswechsel.
- **Gaming-OSD** – FPS, GPU- und CPU-Werte im Spiel, ohne Afterburner & Co.
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

## Technik

Electron-App (`main.js` Backend, `index.html`/`styles.css` Renderer),
NSIS-Installer via electron-builder. Nutzerdaten liegen unter
`%APPDATA%\lumora\` (`games.json`, `prefs.json`, `app-settings.json`).

### Entwicklung

```powershell
npm install
npm start                 # App lokal starten
npx electron-builder --win   # Installer bauen -> dist\Lumora Setup <version>.exe
```

Hilfs- und Diagnose-Skripte liegen in [`_testlab/`](_testlab/) (Deploy,
Launcher-Check, Artwork-Check) und sind vom Build ausgeschlossen.

Die gebündelten Binaries (`bin/`: FFmpeg LGPL-Build, mediamtx,
`lumora-capture` aus [`capture/`](capture/) per `dotnet publish`) sind nicht
im Repo – Bezugsquellen stehen in der [`.gitignore`](.gitignore).

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
