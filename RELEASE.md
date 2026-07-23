# Release-Prozess Lumora (native Version)

Checkliste für ein neues Release. Alle Schritte sind lokal ausführbar; **online geht
nur nach ausdrücklicher Freigabe** (Website-Deploy, Update-Feed). Faktenbasiert
arbeiten – nach jedem Schritt das Ergebnis prüfen, nicht raten.

## 1. Version festlegen
- [ ] `capture-cpp/lumora-shell/build-installer.ps1`: `$version` setzen + Changelog-Kommentar.
- [ ] `package.json`: `"version"` angleichen (Alt-Electron-Feld, nicht load-bearing, aber konsistent halten).
- [ ] `app.rc` wird beim Build automatisch aus `$version` gepatcht (FILEVERSION/PRODUCTVERSION
      + FileVersion/ProductVersion-Strings). **Nicht mehr von Hand** – früher stand sie fix auf
      0.1.0 und der „Über"-Dialog zeigte dauerhaft die falsche Version.

## 2. Release-Notes
- [ ] `_testlab/release-notes/<version>.txt` schreiben, Format: DE-Text, dann `===EN===`, dann EN-Text
      (build-installer speist das in `native-update.json` ein).

## 3. App durchgängig übersetzt?
- [ ] Übersetzungslücken prüfen: Skript vergleicht alle `window.tr(...)`-Aufrufe und sichtbaren
      HTML-Textknoten in `index.html` gegen `I18N_EN` (entity-dekodiert + Whitespace normalisiert
      wie `applyI18n()` zur Laufzeit). 0 Lücken = ok.
- [ ] Neue Dialoge/Panels (z. B. lumoraModal, Eingabe-Brücke) besonders beachten.

## 4. Über-Dialog synchron
- [ ] `index.html` #aboutOverlay listet **genau** die real verbauten Third-Party-Komponenten
      (grep `capture-cpp/third_party`, CMakeLists) – deckungsgleich mit der Transparenz-Tabelle
      der Website. EN-Übersetzungen der Listeneinträge ergänzen.

## 5. Website (LOKAL vorbereiten, nicht deployen)
- [ ] **Version**: `softwareVersion` (schema.org) in `website/index.html` + `website/en/index.html`.
- [ ] **Download-Größe**: Download-Button in beiden index.html auf die **gemessene** Installer-Größe
      (DE „x,y MB", EN „x.y MB"). Nicht die alte Electron-Größe (171 MB) stehen lassen.
- [ ] **Changelog** („Was sich getan hat" / „What's changed"): neuen Versionsblock oben einfügen,
      `aktuell`/`current`-Tag vom Vorgänger entfernen und auf die neue Version setzen.
      **Nur die 5 neuesten Versionen sichtbar** – die Grenze `<div class="changelog-rest" id="changelogRest">`
      steht direkt vor der 6.-neuesten Version. Ältere gehören ins Archiv. Den Zähler im Button
      („Ältere Versionen anzeigen (N)" / „Show older versions (N)") **und** im JS `toggleChangelog()`
      auf die Archiv-Anzahl setzen. DE und EN identisch halten.
- [ ] **Neue Feature-/Landingpage** nötig? DE + EN anlegen, in Nav-Dropdown + Drawer aller
      Themenseiten verlinken, hreflang-Alternates setzen, Sprachumschalter DE↔EN.
- [ ] **Sitemap** `website/sitemap.xml`: `lastmod` aller geänderten Seiten auf das Release-Datum,
      neue Seiten (DE + EN) ergänzen, Vollständigkeit prüfen (auch portfreigabe!). XML-Wohlgeformtheit testen.

## 6. Binaries aktuell?
- [ ] `bin/lumora-capture-native.exe`, `bin/lumora-media-relay.exe`, `bin/lumora-elevate.exe`:
      Zeitstempel gegen die jeweilige Quelle prüfen. build-installer baut **nur die Shell** neu –
      Capture/Relay müssen vorher separat gebaut und nach `bin/` kopiert sein.
- [ ] Auf einem PC ohne Zertifikat sind frisch gebaute Binaries **unsigniert**; build-installer
      signiert die gestageten Kopien neu, sofern auf diesem PC ein gültiges Certum-Token da ist.

## 7. Bauen (auf einem PC mit Certum-Token)
- [ ] `cd capture-cpp/lumora-shell && powershell -NoProfile -ExecutionPolicy Bypass -File ./build-installer.ps1`
- [ ] **CMake-Falle bei geteiltem `build/`**: der CMake-Cache ist maschinenspezifisch. Kommt der
      Ordner von einem anderen PC (anderes Laufwerk/Generator), zuerst neu konfigurieren:
      `cmake -S <shell> -B <shell>\build -G "Visual Studio 17 2022" -A x64` (nach Löschen von
      CMakeCache.txt + CMakeFiles).

## 8. Signaturen verifizieren (PFLICHT)
- [ ] Alle vier EXE **und** der Installer: `Get-AuthenticodeSignature` == `Valid`
      (lumora-shell, lumora-capture-native, lumora-media-relay, lumora-elevate, Setup-exe).
- [ ] Installer-Größe notieren – muss zur Website-Download-Angabe (Schritt 5) passen.

## 9. Ausliefern
- [ ] Installer auf `\\synology\Fileshare\`.
- [ ] Optional lokal per `/S` installieren und `Über`-Version + Registry-DisplayVersion prüfen.
- [ ] **NICHT automatisch online**: Website-Deploy (`_testlab/plan-website-release.json`) und
      Update-Feed (`website/updates/native-update.json` + `components.json`) erst nach Freigabe
      hochladen. Sonst bewirbt die Seite Unveröffentlichtes bzw. Bestandskunden ziehen ungetestet.
