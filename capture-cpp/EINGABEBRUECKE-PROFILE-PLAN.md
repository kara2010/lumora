# Eingabe-Brücke: Tastatur-Modus + teilbare Profile

Stand: 2026-08-05 · Branch `feature/eingabebruecke-profile`

## Warum (Nutzer-Anforderung)

GTA2 (1999) mit dem Xbox-Pad spielen. Dazu zwei Bausteine:
1. **Neuer Brücken-Modus Gamepad → Tastatur.** Die Brücke übersetzt heute nur in
   EINE Richtung: DirectInput/HID → virtueller Xbox-Controller (ViGEm), also
   *alte Geräte für neue Spiele*. GTA2 kennt kein XInput; ein virtuelles Pad
   nützt ihm nichts. Was es versteht, ist die Tastatur.
2. **Export-/importierbare Profile**, damit eine Community Konfigurationen
   austauschen kann, ohne dass wir Konten, Server oder Moderation brauchen.

Damit wird die Brücke vom Einbahn-Übersetzer zum **Universal-Adapter**: alte
Geräte für neue Spiele UND neue Geräte für alte Spiele. Eigenes Verkaufsargument.

## Belegte Fakten (gemessen, nicht angenommen)

- Installation: `E:\Games\Grand Theft Auto 2`, `gta2.exe` vom 27.04.2004
  (Rockstar-Freeware-Neuauflage der 1999er-Version), Registry-Zweig
  `HKCU\SOFTWARE\DMA Design Ltd\GTA2` existiert NOCH NICHT (nie gestartet).
- **GTA2 arbeitet auf Scancode-Ebene.** Beleg: `data\Keyboard\*_KB.cfg` ist
  eine Tabelle mit 257 Einträgen, Index = PS/2-Set-1-Scancode
  (0x01 ESCAPE, 0x0E BSPACE, 0x0F TAB, 0x1C RETURN, 0x1D L_CTRL, 0x2A LSHIFT,
  0x39 SPACE, 0xC8/0xCB/0xCD/0xD0 = E0-präfixierte Pfeiltasten). Pro Sprache
  eine Datei (ENG/GER/FRE/ITA/POR/SPA) - nur die ANZEIGENAMEN unterscheiden
  sich, die Scancode-Positionen sind identisch.
  → Folge fürs Senden: `SendInput` mit `KEYEVENTF_SCANCODE` (+ `EXTENDEDKEY`
  für die Pfeiltasten), NICHT mit virtuellen Keycodes. Ein VK-basierter
  Ansatz scheitert bei DirectInput-Ära-Spielen typischerweise still.
- Die Standard-BELEGUNG (welche Taste welche Aktion) steht weder in den
  KB.cfg noch in den .gxt-Textarchiven. Sie wird beim ersten Spielstart in die
  Registry geschrieben → **von dort auslesen, nicht raten** (s. Etappe 4).

## Profilformat v1 (`.lumoraprofil`)

JSON, bewusst klein und menschenlesbar. Bestehende `input-profiles.json`
(Struktur: `axes`, `buttons`, `axisToButton`, `buttonToAxis`, je mit
`vid`/`pid`) wird NICHT ersetzt, sondern um den Tastatur-Modus erweitert -
alte Profile bleiben gültig.

```json
{
  "lumoraProfil": 1,
  "name": "Grand Theft Auto 2",
  "spiel": "gta2.exe",
  "modus": "tastatur",
  "autor": "",
  "notiz": "Pfeiltasten-Steuerung, Feuer auf RT",
  "tasten": [
    { "quelle": "LX-", "scancode": 75, "ext": true,  "schwelle": 0.40, "loesen": 0.25 },
    { "quelle": "A",   "scancode": 57, "ext": false }
  ]
}
```
- `quelle`: Xbox-Knopf (`A`,`B`,`X`,`Y`,`LB`,`RB`,`START`,`BACK`,`DU`…) oder
  Achsenrichtung (`LX-`,`LX+`,`LY-`,`LY+`,`RX±`,`RY±`,`LT`,`RT`).
- `scancode` + `ext`: exakt das, was `SendInput` braucht.
- **Hysterese Pflicht** bei Achsen: `schwelle` drücken, `loesen` (kleiner)
  loslassen. Ohne das flattert die Taste am Übergang - der klassische Fehler
  bei Analog→Digital.
- **Datensparsamkeit**: KEINE Pfade, keine Geräte-Seriennummern, kein
  Benutzername. Ein Profil ist gefahrlos teilbar (gleiche Disziplin wie beim
  geteilten Bericht).

## Etappen

1. **Tastatur-Modus in der Brücke** (`input_bridge.h`): Xbox-Zustand (XInput
   lesen, das Pad ist ein echtes Xbox-Pad) → Scancode-Ausgabe per SendInput.
   Kantenerkennung + Hysterese, Auto-Loslassen aller Tasten beim Deaktivieren
   und beim Fokusverlust (sonst „klebt" eine Pfeiltaste, wenn man Alt-Tabbt -
   genau die Fehlerklasse aus der Geräte-Abzieh-Panne).
2. **Profil-Datei-Ebene**: Export/Import über den Datei-Dialog
   (`.lumoraprofil`), Validierung beim Import (Schema, Wertebereiche,
   unbekannte Felder ignorieren), Haus-Profile mitliefern.
3. **UI im Eingabe-Reiter**: Modusumschalter, Profil-Liste mit
   Export/Import/Duplizieren, **Mapping-Assistent** („Drücke jetzt die Taste
   für *Feuer*") statt Formular.
4. **GTA2-Profil**: Spiel einmal starten lassen → Registry
   `HKCU\SOFTWARE\DMA Design Ltd\GTA2` auslesen → echte Standardbelegung ins
   Profil übernehmen. Danach Test mit echtem Pad im echten Spiel: Fahren,
   Lenken, Schießen, Ein-/Aussteigen, Waffenwechsel.
5. **Anbindung an die Bibliothek**: Profil an einen Spieleintrag hängen
   (die Auto-Aktivierung pro Spiel existiert bereits), Overlay-Hinweis
   „Profil GTA2 aktiv" beim Einschalten.

## Risiken / bewusste Entscheidungen

- **Fokus**: Tasten dürfen nur fließen, wenn das Zielspiel im Vordergrund ist.
  Sonst tippt das Pad in fremde Fenster. Prüfung im Sende-Pfad, nicht nur beim
  Aktivieren.
- **Kein Anti-Cheat-Thema**: SendInput ist normale Windows-Eingabe; die Brücke
  zielt auf Einzelspieler-Klassiker. (ViGEm-Bewertung s. frühere Recherche.)
- **Beide Modi gleichzeitig** wäre technisch möglich, aber verwirrend →
  bewusst exklusiv pro Profil (`modus`).
- **Scancode-Tabelle**: die 257-Zeilen-Datei aus dem Spiel ist die Referenz für
  die Anzeigenamen im Assistenten (deutsch aus `GER_KB.cfg`) - nicht selbst
  erfundene Namen.
