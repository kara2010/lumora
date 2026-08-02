# Auffindbarkeit: Befund, Erledigtes, Vorbereitetes

Stand: 2026-08-03

## Befund (gemessen, nicht vermutet)

- Google hat die Seite im Index, `robots.txt` erlaubt alles, Sitemap verlinkt,
  Titel/Description sauber. **Technisch ist nichts kaputt.**
- **Ausserhalb der eigenen Seite existiert Lumora nirgends**: keine Erwaehnung,
  kein eingehender Link, kein Verzeichniseintrag, 0 GitHub-Sterne.
- Das GitHub-Repository hatte **keine einzige Topic-Markierung** - der Hauptweg,
  ueber den Projekte dort gefunden werden, war ungenutzt.
- Beide READMEs hatten **null Bilder**.
- **Namenskollision**: "Lumora" ist auf itch.io bereits ein Spiel. Die Suche nach
  dem blossen Produktnamen fuehrt nicht zuverlaessig hierher.

### Die unbequeme Kernaussage

Die Seite ist nicht unauffindbar, weil sie schlecht gemacht ist, sondern weil ihr
**eingehende Verlinkungen** fehlen. Fuer Suchmaschinen ist eine Domain ohne Links
praktisch gewichtslos. Bei "Spiele-Launcher" konkurriert Lumora mit Playnite, GOG
Galaxy und LaunchBox, die seit Jahren verlinkt werden - dort ist mit Bordmitteln
nichts zu holen. Weitere Arbeit an Meta-Tags aendert daran nichts.

## Erledigt

- 20 GitHub-Topics gesetzt (game-launcher, game-streaming, webrtc, hdr, gamepad,
  xinput, vigem, osd, fps-counter, stutter, frametime, whep, screen-capture, ...)
- Beide READMEs mit Screenshots: Hero-Bild + 2x2-Raster (OSD, Streaming,
  Ruckler-Analyse, Eingabe-Bruecke) mit erklaerenden Bildunterschriften.
  Bilder werden von lumora-streaming.de geladen - kein Repo-Ballast, bleiben aktuell.
- Live geprueft: 5 Bilder eingebunden, keines kaputt, 20 Topics aktiv.

## Vorbereitet - MUSS VON DIR KOMMEN

Ich lege in fremden Netzwerken keine Konten an und poste dort nichts in deinem
Namen. Das Folgende ist fertig formuliert; du musst es nur einstellen.

### 1. AlternativeTo (hoechster Hebel)

Warum: Genau dort suchen Leute "Alternative zu Playnite / GOG Galaxy / Parsec".
Ein Eintrag bringt einen echten Link UND qualifizierten Verkehr.
<https://alternativeto.net/> - Eintrag anlegen, als Alternative zu **Playnite**,
**GOG Galaxy**, **LaunchBox**, **Parsec**, **Moonlight** verknuepfen.

Kurzbeschreibung (EN, ~250 Zeichen):
> Native Windows game launcher (3.7 MB, no Electron) that unifies Steam, Epic,
> GOG and Xbox in one library, fully gamepad-controlled. Streams a running game
> to any browser via a link (P2P WebRTC), switches HDR per game, turns wheels and
> joysticks into a virtual Xbox pad, and shows FPS/GPU/CPU without Afterburner.

### 2. GitHub-Release anlegen

Aktuell gibt es Tags/Commits, aber keine sichtbaren Releases mit Installer.
GitHub-Releases werden indexiert und von Nutzern erwartet. Installer anhaengen,
Release-Notes aus `_testlab/release-notes/<version>.txt` uebernehmen.

### 3. Reddit - nur mit echtem Anlass, nicht als Werbung

Passende Communities: r/pcgaming, r/software, r/opensource, r/Windows10,
r/simracing (Eingabe-Bruecke!), r/OpenSourceGames.

WICHTIG: Reine Ankuendigungen fliegen dort raus. Was funktioniert, ist ein
Beitrag mit konkretem Nutzen - z. B. in r/simracing: "Habe ein kostenloses
Werkzeug gebaut, das Lenkrad + Pedale als ein virtuelles Xbox-Pad zusammenfasst,
damit sie in Spielen funktionieren, die nur XInput koennen" plus Screenshot.
Das Produkt kommt in einem Nebensatz vor, nicht in der Ueberschrift.

Der staerkste Aufhaenger ist die **Ruckler-Analyse**: "Warum ruckelt mein Spiel"
ist eine Dauerfrage in r/pcgaming und r/buildapc. Ein geteilter Bericht, der
`nvlddmkm.sys` als Verursacher benennt, ist ein Beleg - keine Werbung.
(Siehe `capture-cpp/SHARE-REPORT-PLAN.md`, Idee 1.)

### 4. Weitere Verzeichnisse

- Softpedia, MajorGeeks: nehmen kostenlose Windows-Software auf, geben Links.
- Awesome-Listen auf GitHub (awesome-windows, awesome-gaming): Pull Request.
- Winget-Paket: fuer Entwickler-Publikum sichtbar, ist ein Repository-Eintrag.

## Namensproblem - zum Nachdenken

"Lumora" kollidiert mit einem itch.io-Spiel. Ein Umbenennen ist teuer (Domain,
Signatur-Zertifikat, Installer, bestehende Nutzer) und lohnt bei 139 Downloads
vielleicht noch - spaeter nicht mehr. Alternative ohne Umbenennung: konsequent
mit einem Zusatz auftreten ("Lumora Game Launcher"), damit die Suche eindeutig
wird. In Titel/Description der Startseite steht "Lumora" bisher am ENDE.

## Was ich NICHT empfehle

Weitere Arbeit an Meta-Tags, Keyword-Dichte oder zusaetzlichen Landingpages.
Das Fundament ist in Ordnung; der Engpass sind fehlende Links und fehlende
Erwaehnungen. Dagegen hilft nur, dass Menschen das Ding sehen und erwaehnen.
