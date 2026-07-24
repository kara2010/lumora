# Lumora

🇩🇪 [Deutsche Version](README.de.md)

A native cross-store game launcher for Windows with game streaming, an input
bridge, automatic HDR and a gaming OSD. Lumora finds installed games from
multiple stores, shows them in a unified, curated library and, on request,
streams a running game to any browser via a link. As of version 3.0.0 it runs
entirely on its own native C++ code — no Electron, no FFmpeg, no third-party
streaming server — for a download of about 3.5 MB.

Download & info: <https://lumora-streaming.de/>

## Features

- **Cross-store** – finds games from Steam, Microsoft Store / Xbox, EA, Epic,
  GOG and more, scanning all drives automatically.
- **Game streaming via link** – stream a running game straight to any browser
  (own C++ capture + hardware encoder → own WebRTC/WHEP relay). Full HD
  (1080p60) by default, up to 4K; AV1 or H.264 depending on the viewer's
  decoder; direct peer-to-peer connection (including an IPv6 direct path);
  group streams with room codes; optional access control that asks before
  anyone may watch.
- **Input bridge** – turns a joystick, wheel or flight stick into a virtual
  Xbox 360 controller (via the signed open-source driver ViGEmBus), so devices
  that report as DirectInput work in games that only accept XInput. Visual
  mapping, live input monitor, auto-calibration, axis↔trigger and button↔axis
  remapping, separate devices (wheel + pedals) and per-game profiles.
- **Automatic HDR** – enables HDR when a game launches and switches it back
  afterwards (native, no external tool).
- **Gaming OSD** – FPS, GPU and CPU readings in-game, without Afterburner & co.
  (own ETW-based frame timing).
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
- **Signed** – all executables and the installer are digitally signed
  (Certum Code Signing).

## Technology

As of 3.0.0 Lumora is a set of native C++ helpers (see [`capture-cpp/`](capture-cpp/)):

- **lumora-shell** – the app itself; hosts the web UI (`index.html` /
  `styles.css`) in a WebView2 window and owns tray, hotkeys, autostart,
  launcher scanning and orchestration.
- **lumora-capture-native** – screen capture, scaling / HDR and hardware
  encoding (NVENC / AMF / QSV, AV1 & H.264, Opus audio), published as
  MPEG-TS over UDP.
- **lumora-media-relay** – the built-in WebRTC/WHEP relay (libdatachannel),
  which replaced the former mediamtx server.
- **lumora-elevate** – a small one-shot helper for the few operations that
  need elevation.

The input bridge uses a system-wide virtual controller via
[ViGEmBus](https://github.com/nefarius/ViGEmBus) (installed on request from
the official source, signature verified). User data lives under
`%APPDATA%\lumora\`. The signed NSIS installer is produced by
[`capture-cpp/lumora-shell/build-installer.ps1`](capture-cpp/lumora-shell/build-installer.ps1).

### Development

Each helper builds with CMake (Visual Studio 2022, x64), e.g. the shell:

```powershell
cmake -S capture-cpp/lumora-shell -B capture-cpp/lumora-shell/build -G "Visual Studio 17 2022" -A x64
cmake --build capture-cpp/lumora-shell/build --config Release
```

The full signed installer (shell + staged helpers) is built via:

```powershell
cd capture-cpp/lumora-shell
powershell -NoProfile -ExecutionPolicy Bypass -File ./build-installer.ps1
```

Helper and diagnostic scripts live in [`_testlab/`](_testlab/) and are not
part of the build. Bundled helper binaries under `bin/` are not committed;
they are built from [`capture-cpp/`](capture-cpp/). The former Electron entry
points (`main.js`, electron-builder) remain in the tree as the legacy path
that 3.0.0 supersedes.

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
