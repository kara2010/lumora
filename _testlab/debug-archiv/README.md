# Debug-Archiv

Verschobene (NICHT geloeschte) Debug-Artefakte - bei Bedarf wiederverwendbar.

## 2026-07-capture-debug/
Aus den QSV-/AMD-Debug-Sessions (siehe HANDOFF-INTEL-QSV-DEBUG.md / HANDOFF-AMD-DEBUG.md im Repo-Root):
- cap_*.err/.out - Encoder-Reproduktionslaeufe (CLI-Ausgaben)
- dump_nal.ts - TS-Dump fuer NAL-Analyse
- live_index.html + reader.js - WHEP-Grid-Testclient (Debughilfe, nicht von der App genutzt)
- repro-mtx.yml - mediamtx-Config-Spiegel fuer A/B-Tests (mediamtx ist inzwischen abgeloest)
- _pfad-test.txt / _z-pfad-test.txt - Laufwerks-/Pfadtests nach NAS-Umzug
- test_lan.obj - Compiler-Ueberbleibsel eines LAN-Tests

## build-shell-installer.ps1
Aeltere Installer-Script-Variante (0.9.0-Beta) - abgeloest durch
capture-cpp/lumora-shell/build-installer.ps1.
