# Lumora

Ein store-übergreifender Game-Launcher für Windows mit automatischem HDR.
Lumora findet installierte Spiele aus mehreren Stores, zeigt sie in einer
einheitlichen, schön kuratierten Bibliothek und schaltet beim Spielstart
automatisch HDR ein.

## Features

- **Store-übergreifend** – findet Spiele von Steam, Microsoft Store / Xbox,
  EA, Epic, GOG u. a. und scannt automatisch alle Laufwerke.
- **Automatisches HDR** – aktiviert HDR beim Spielstart und schaltet danach
  zurück (via `HDRCmd.exe`).
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

## Website

Der Ordner [`website/`](website/) enthält die Downloadseite
(lumora.kara-webdesign.de/).
