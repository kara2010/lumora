# Lumora – Testlabor

Ausgelagerte Diagnose-/Test-Skripte. **Nicht Teil des Builds** (der `build.files`-Block
in `package.json` packt nur `main.js`, `index.html`, `styles.css`, `icon.ico`,
`icon-64.png`, `package.json` ins asar – dieser Ordner bleibt außen vor).

## Werkzeuge

| Datei | Zweck | Aufruf (aus diesem Ordner) |
|---|---|---|
| `deploy.ps1` | asar bauen + in die installierte App kopieren (mit Sicherheits-Checks) | `./deploy.ps1` bzw. `./deploy.ps1 -Verify "muster"` |
| `gamecheck.ps1` | Alles zu einem Spiel: Launcher-Typ, Steam-AppID + Running, Xbox-AUMID, Exe-Startbarkeit, „läuft gerade?" (3 Methoden), letzte Log-Zeilen | `./gamecheck.ps1 -Name "Assetto"` |
| `artwork.js` | Verfügbare Cover/Hero/Hintergrund-Bilder (Steam + MS Store) inkl. Maße | `node artwork.js "Forza Horizon 6"` |

Spielzeit-Live-Log der App: `%APPDATA%\lumora\playtime-log.txt`
(`LAUNCH … / STARTED nach +Xs / ENDED – Dauer Ys / TIMEOUT …`).

---

## Erkenntnisse / Lehren (für künftige Spiele & Launcher)

### Spielzeit-Erfassung
1. **`tasklist` kürzt Image-Namen auf 25 Zeichen** in der Standardausgabe → immer
   `/FO CSV` nutzen, sonst schlägt der `includes(name)`-Check bei langen Exe-Namen
   fehl (Bug: „Arkanoid-EternalBattle.exe" → Spielzeit 9 s).
2. **Steams Registry `…\Apps\<appid>\Running` ist UNZUVERLÄSSIG** – kann `0` sein,
   obwohl das Spiel läuft (live an AC EVO bestätigt). Nicht allein darauf verlassen.
3. **Zuverlässig ist die Prozess-Erkennung:** (a) per Exe-Name via `tasklist /FO CSV`
   und (b) per Install-Ordner via `Get-Process | Where Path -like '<ordner>\*'`.
   Die Ordner-Methode fängt auch Launcher-Exes, umbenannte/Handoff-Prozesse.
4. Monitor läuft **im Speicher von Lumora** → wird Lumora während des Spielens
   beendet, geht die Session verloren. Spielzeit wird erst **beim Beenden** des
   Spiels verbucht, nicht live.
5. Schwere Spiele starten langsam → **Start-Timeout großzügig** (aktuell 5 Min),
   sonst gibt der Monitor auf, bevor der Prozess auftaucht.

### Starten je Launcher
6. **Steam:** über `steam://rungameid/<appid>` starten – die Exe direkt zu starten
   bringt den DRM-Fehler „User has not permission to run this product" und stört die
   Erfassung. AppID via `steamAppIdForExe` (über die `appmanifest_*.acf`).
7. **Xbox/Game Pass (UWP):** Exe ist **nicht direkt startbar** (Zugriff verweigert) →
   zwingend über die **AUMID**: `explorer.exe shell:appsFolder\<AUMID>`. AUMID via
   `Get-StartApps` (Match über den `XboxGames\<Ordner>`-Namen).
8. **GOG/EA/Rockstar/…:** Exe direkt startbar (evtl. wegklickbares DRM-Popup); die
   Spielzeit greift über den Prozess-Monitor. Protokoll-Start nur bei Bedarf + Test.

### Deploy
9. Die **laufende App sperrt `app.asar`** → **erst Lumora beenden, dann kopieren**,
   sonst schlägt die Kopie still fehl und die App lädt weiter den alten Stand.
10. **Niemals `asar extract-file <asar> package.json`** – electron-builder packt eine
    minimierte package.json; das Extrahieren überschreibt die Projekt-package.json
    und zerstört den `build`-Block. (Für main.js/index.html/styles.css ist es ok.)

### Arbeitsweise
11. **Erst bestehenden Code analysieren**, dann ändern (Namens-/CSS-Kollisionen, z. B.
    `setCoverMode` doppelt).
12. **Gegen das echte System verifizieren, nicht ableiten** – live messen schlägt
    jede Annahme (Registry-Flag, tasklist-Kürzung, UWP-Zugriff, AUMID …).
