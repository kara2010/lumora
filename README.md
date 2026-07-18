# Lumora

🇩🇪 [Deutsche Version](README.de.md)

A cross-store game launcher for Windows with automatic HDR, game streaming
and a gaming OSD. Lumora finds installed games from multiple stores, shows
them in a unified, beautifully curated library and automatically enables HDR
when a game starts.

Download & info: <https://lumora-streaming.de/>

## Features

- **Cross-store** – finds games from Steam, Microsoft Store / Xbox, EA, Epic,
  GOG and more, automatically scanning all drives.
- **Automatic HDR** – enables HDR when a game launches and switches it back
  off afterwards (via `HDRCmd.exe`).
- **Game streaming via link** – up to 4K/60 fps straight to any browser
  (FFmpeg hardware encoder → mediamtx → WebRTC/WHEP), group streams with
  room codes, adaptive bitrate, seamless quality switching.
- **Gaming OSD** – FPS, GPU and CPU readings in-game, without Afterburner & co.
- **Gamepad control** – the entire UI can be driven with an Xbox controller
  (couch gaming on the TV).
- **Covers & artwork** – covers, hero banners, description, genre and release
  year are fetched automatically (Steam, Microsoft Store, optionally
  [SteamGridDB](https://www.steamgriddb.com)); if something doesn't fit, you
  pick from multiple sources yourself or drop in your own image.
- **Library** – pin favorites, poster or hero view, sort by name, playtime
  or last played.
- **Playtime tracking** – reliable per-game tracking across launchers.
- **Convenience** – autostart, start in the system tray, accent color.

## Technology

Electron app (`main.js` backend, `index.html`/`styles.css` renderer),
NSIS installer via electron-builder. User data lives under
`%APPDATA%\lumora\` (`games.json`, `prefs.json`, `app-settings.json`).

### Development

```powershell
npm install
npm start                 # run the app locally
npx electron-builder --win   # build the installer -> dist\Lumora Setup <version>.exe
```

Helper and diagnostic scripts live in [`_testlab/`](_testlab/) (deploy,
launcher check, artwork check) and are excluded from the build.

The bundled binaries (`bin/`: FFmpeg LGPL build, mediamtx, `lumora-capture`
built from [`capture/`](capture/) via `dotnet publish`) are not part of the
repo – sources for obtaining them are listed in [`.gitignore`](.gitignore).

## Website

The [`website/`](website/) folder contains the download page
(lumora-streaming.de/).

## Author

Lumora is developed and maintained by **Karsten Radermacher**
(GitHub: [kara2010](https://github.com/kara2010)).
Copyright © 2026 Karsten Radermacher.

## License

Lumora is free software under the **GNU Affero General Public License v3.0**
([LICENSE](LICENSE)). That means you may use, modify and redistribute the
code – but any derived software or software built on top of it (including
when operated as a network service) must publish its complete source code
under the same license.

**Trademark:** The name "Lumora", the logo and the visual identity are **not**
part of the license. Forks must use their own name and branding and must not
give the impression of being the official Lumora or being affiliated with it.

## Contributing

Bug reports and suggestions are welcome as GitHub issues!
**Pull requests are currently not accepted** – Lumora deliberately remains a
one-person project so the author retains sole authorship (and with it full
licensing authority over future versions).
