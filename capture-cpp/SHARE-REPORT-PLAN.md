# Plan: Geteilter Ruckler-Bericht + Umgebungs-Wächter + Controller an den Zuschauer

> Status: **Entwurf** (Design-Stand 2026-08-01, noch kein Code, nichts beschlossen).
> Auslöser: Frage nach Funktionen, die neue Nutzer gewinnen. Drei Ideen, alle drei
> vom Nutzer als interessant markiert. Sie hängen zusammen: 1 und 2 arbeiten auf
> demselben Berichts-JSON, 3 ist ein eigenständiger Ausbau der Streaming-Seite.
>
> Ausgangslage, die die Reihenfolge bestimmt: Die Software kann mehr, als sie an
> Nutzern hat (Stand 1.8.2026: 127 gezählte Downloads). Das knappe Gut ist nicht
> Funktionsumfang, sondern Auffindbarkeit. Idee 1 ist deshalb zuerst dran - sie ist
> die einzige, die sich selbst verbreitet.

---

## Idee 1: Der teilbare Ruckler-Bericht

### Produktversprechen
Ein Knopf im Berichts-Reiter: **"Als Link teilen"**. Lumora lädt den Bericht hoch und
gibt eine kurze URL zurück, z. B. `lumora-streaming.de/b/7f3a9c`. Dahinter liegt genau
die Ansicht, die der Nutzer in der App sieht: Urteil, Kennzahlen, Frametime-Kurve mit
Ruckler-Markern, Flaschenhals-Verteilung, Verdächtigenliste.

### Warum ausgerechnet das
Es gibt eine ständig laufende Unterhaltung im Netz, die genau aus unserem Thema
besteht ("Spiel ruckelt trotz guter Hardware", "Mikroruckler ohne Grund"). Die heute
übliche Antwort ist ein Afterburner-Screenshot und Raten. Mit dem geteilten Bericht
antwortet jemand mit einem Link, der sagt: *DPC-Latenz von `nvlddmkm.sys`, bei 8 von 11
Rucklern beteiligt.* Jeder geteilte Link ist ein Beleg statt einer Behauptung und landet
dort, wo die Frage gestellt wird - im Gegensatz zur Website, die man erst finden muss.

Zweitnutzen: Wer heute Hilfe sucht, muss die Rohdaten abtippen. Ein Link ersetzt das.

### Was schon da ist (deshalb ist der Aufwand klein)
- **Berichts-JSON, Schema v1**, geschrieben in `osd_broker.h` (Ende `runAnalyzeBroker`):
  `version, wall, durS, pid, frames, avgFps, p1LowFps, medianFtMs, p99FtMs, spikes,
  spikesPerMin, presentOnly, kernelOk, errCode, verdictKey, verdictHits, verdictName,
  game, gpu, gpuDriver, context{}, note, ftSeries[≤2000], findings[], aggregate[≤8],
  limit{}`. Das ist bereits ein sauberes, versioniertes Schema - keine Umbauten nötig.
- **Die Zeichnung** ist reines Canvas-2D ohne Fremdbibliotheken: `anDrawChart()` in
  `index.html` (~Z. 3195) plus `anStatTiles()`, `anLimitSection()`, `anVerdict()`.
  Diese Funktionen sind bereits datengetrieben (Eingabe = Berichts-JSON) und lassen sich
  nahezu unverändert auf einer Webseite ausführen.
- **Server-Infrastruktur**: PHP auf dem Webhost, `data/`-Verzeichnis mit eigener
  `.htaccess`, das Muster "kleiner Endpunkt + JSON-Datei" ist mit `download.php` und
  `gruppe.php` etabliert.

### Datenfluss
```
App (Shell)                    Webserver                     Browser (beliebig)
-----------                    ---------                     ------------------
Bericht liegt als JSON in
%APPDATA%\lumora\analyze\

[Als Link teilen]
  -> Vorschau-Dialog:
     zeigt WAS hochgeht
     (Schalter: Prozessnamen
      weglassen / Spielname
      weglassen)
  -> gefiltertes JSON  ---POST--->  bericht.php
                                    - Größe < 256 KB?
                                    - Schema v1? Pflichtfelder?
                                    - Rate-Limit je IP
                                    - ID = 6 Zeichen (zufällig)
                                    - schreibt data/berichte/<id>.json
                                    - Antwort {url, id, loeschToken}
  <---- URL + Löschtoken --------
  URL in Zwischenablage,
  Löschtoken lokal beim Bericht
  gespeichert (Nutzer kann
  später zurückziehen)
                                    b/<id>  (Rewrite in .htaccess)
                                    -> bericht.php?id=<id>
                                    - liefert HTML-Grundgerüst
                                      mit eingebettetem JSON
                                                          -> bericht.js zeichnet
                                                             dasselbe Canvas wie
                                                             die App
```

### Was hochgeht - und was nicht
Der Bericht enthält Prozessnamen, Treibernamen, GPU-Modell, Treiberversion,
Bildschirmauflösung, Windows-Build und den Spielnamen. Das ist für die Diagnose
wertvoll und für die Privatsphäre nicht harmlos: Prozessnamen verraten installierte
Software, und Nutzernamen können in Pfaden stecken.

Regeln (nicht verhandelbar, sonst verspielt das Teilen das Vertrauen, das der Rest der
App aufbaut):
1. **Teilen ist immer eine Einzelentscheidung.** Kein Automatismus, keine Voreinstellung,
   kein "beim nächsten Mal nicht mehr fragen".
2. **Vorschau vor dem Hochladen**, die den tatsächlich zu sendenden Text zeigt - nicht
   eine Beschreibung davon.
3. **Pfade werden immer entfernt**, nur Dateinamen bleiben (`game` enthält heute den
   vollen Pfad - das MUSS beim Teilen auf den Dateinamen gekürzt werden, dort steckt
   regelmäßig der Windows-Benutzername drin).
4. Zwei Schalter im Dialog: *Prozessnamen weglassen* (Verdächtige werden zu
   "Prozess A/B/C", Aussage bleibt) und *Spielname weglassen*.
5. **Löschbar**: Der Löschtoken wird lokal zum Bericht gespeichert, in der Verlaufsliste
   steht dann "geteilt · Link kopieren · Teilen zurückziehen".
6. Keine Anmeldung, kein Konto, keine Kennung, die zwei Berichte desselben Nutzers
   verknüpft.

### Serverteil
- `bericht.php` - eine Datei, zwei Betriebsarten:
  - `POST` (JSON): prüfen, ablegen, `{url, id, delToken}` antworten.
  - `GET ?id=` / über Rewrite `b/<id>`: HTML ausliefern.
  - `DELETE`/`POST ?del=` mit Token: Datei entfernen.
- Ablage: `data/berichte/<id>.json`, daneben `<id>.meta` (Anlagezeit, Löschtoken-Hash).
- **Missbrauchsschutz**: max. 256 KB, gültiges Schema v1, Rate-Limit (z. B. 10/Stunde je
  IP über eine kleine Zählerdatei wie bei `download.php`), keine HTML-Ausgabe aus
  Nutzerdaten ohne Maskierung.
- **Verfallsdatum**: Berichte ohne Abruf nach 12 Monaten löschen (Aufräum-Lauf im
  Endpunkt selbst, kein Cronjob nötig). Muss in der Datenschutzerklärung stehen.
- **Der Endpunkt liegt NICHT hinter dem Deploy-Token.** `sync-endpoint.php` schützt sich
  und `.htaccess` bewusst; ein öffentlicher Schreib-Endpunkt ist eine neue Angriffsfläche
  und braucht deshalb die Grenzen oben, nicht das Deploy-Geheimnis.

### Webseite
- `bericht.js` + `bericht.css`: die aus `index.html` herausgelösten Zeichenfunktionen.
  **Wichtig:** herauslösen, nicht kopieren - sonst driften App-Ansicht und Webansicht
  auseinander (dieselbe Falle wie bei den Lightbox-Kopien, s. `website/lightbox.js`).
  Sauberster Weg: `analyze-report.js` als gemeinsame Datei, die App lädt sie ebenfalls.
- Die Seite ist zweisprachig (Sprache aus `navigator.language`, umschaltbar) und trägt
  einen unaufdringlichen Fußbereich: "Erstellt mit Lumora - kostenlos, quelloffen".
  Das ist der eigentliche Werbeeffekt; er darf den Bericht nicht überlagern.
- `noindex` für einzelne Berichte (fremde Messdaten sind kein Suchmaschinenfutter und
  würden die Domain verwässern), aber eine **indexierbare Sammelseite** "Was Lumora in
  Ruckler-Berichten findet" wäre später ein starker SEO-Baustein.
- Vorschaubild für Chat-Programme (`og:image`): generiert aus Urteil + Kennzahlen. Ohne
  das sieht ein geteilter Link in Discord/WhatsApp nach nichts aus - hier entscheidet
  sich, ob jemand klickt. **Nicht optional, sondern der halbe Nutzen.**

### App-Teil
- Neuer IPC-Kanal `analyze-share` (Bericht-Datei, Optionen) -> Worker-Thread, HTTP-POST
  über die vorhandene HTTP-Client-Stelle, Rückgabe `{url, delToken}`.
- Knopf in `anShowReport()` neben "Als Text kopieren".
- Verlaufsliste zeigt geteilte Läufe an und erlaubt das Zurückziehen.
- Fehlerfälle ehrlich: kein Netz / Server antwortet nicht / Bericht zu groß -> Klartext,
  kein stiller Fehlschlag.

### Aufwand (grobe Hausnummer)
Serverteil klein, Webansicht mittel (das Herauslösen der Zeichenfunktionen ist die
eigentliche Arbeit), App-Teil klein. Das Vorschaubild ist der unterschätzte Posten.

### Verifikation
- Bericht teilen -> Link in einem anderen Browser (nicht angemeldet, anderes Gerät)
  öffnen -> identische Kurve und identisches Urteil wie in der App.
- Datensparsamkeit: hochgeladenes JSON gegen das lokale diffen, Pfade müssen weg sein.
- Zurückziehen -> Link liefert 404.
- Missbrauch: 2 MB schicken, kaputtes JSON schicken, 50× hintereinander schicken.
- Link in Discord einfügen -> Vorschaukarte sieht gut aus.

---

## Idee 2: "Was hat sich verändert?" - der Umgebungs-Wächter

### Produktversprechen
Die Vergleichsansicht sagt heute *dass* Lauf B schlechter ist. Sie soll sagen, *was
sich geändert hat*: "Zwischen Lauf A und B: Grafiktreiber 566.36 -> 572.16, neuer
Hintergrunddienst `XY`, Auflösung gleich. Seitdem 80 % mehr Ruckler."

Das ist die Frage, die Nutzer tatsächlich haben ("seit letzter Woche ruckelt es"), und
kein verbreitetes Werkzeug beantwortet sie.

### Was schon da ist
`analyzeWriteContext()` in `main.cpp` schreibt bereits `game, resolution, hdr,
streaming, winBuild, lumora`; der Bericht ergänzt `gpu` und `gpuDriver`. Die
Vergleichsansicht existiert und warnt schon, wenn die Umgebung abweicht.

### Was fehlt
1. **Mehr Kontext erfassen** (alles billig, alles einmalig beim Messstart):
   Grafiktreiber-Datum, Energieschema, Windows-Spielmodus, HAGS an/aus,
   Bildwiederholrate, laufende Overlay-Programme (Afterburner, Discord, GeForce
   Experience - genau die üblichen Verdächtigen), Anzahl Autostart-Programme.
2. **Vergleichslogik**: Feld-für-Feld-Differenz zweier `context`-Blöcke, sortiert nach
   Erklärkraft (Treiberwechsel schlägt Auflösungsänderung schlägt Kleinkram).
3. **Klartext-Sätze** als Schlüssel + Argumente (wie `verdictKey`), damit DE/EN aus der
   UI kommt.

### Ehrlichkeitsgrenze (wichtig)
Das Ding stellt **Korrelation** fest, nicht Ursache - genau wie die Verdächtigenliste.
Der Wortlaut muss das tragen: "hat sich geändert" statt "ist schuld". Ein Werkzeug, das
falsche Ursachen behauptet, verliert seinen Wert schneller, als es ihn aufgebaut hat.

### Warum nach Idee 1
Es verbessert das Erlebnis vorhandener Nutzer, verbreitet sich aber nicht von selbst.
Kombiniert mit Idee 1 wird es allerdings stark: Ein geteilter Bericht, der einen
Treiberwechsel benennt, ist ein sehr überzeugendes Fundstück.

---

## Idee 3: Den Controller an den Zuschauer weiterreichen

### Produktversprechen
Zwei Bausteine, die es einzeln schon gibt, zusammengesteckt:
- P2P-WebRTC-Strecke mit geringer Verzögerung (Streaming),
- Eingabe-Brücke, die Fremdeingaben in ein virtuelles Gamepad übersetzt (ViGEm).

Ergebnis: Der Zuschauer schaut nicht nur zu, sondern **darf mal ans Steuer** - Couch-Koop
über Entfernung, ohne dass der Gast das Spiel besitzt. Das kann Discord nicht, Steam
Remote Play Together nur für ausdrücklich unterstützte Titel.

### Warum das die spannendste und zugleich teuerste Idee ist
Es ist ein echtes Alleinstellungsmerkmal - und es öffnet eine Fernsteuerung des PCs.
Der Sicherheitsteil ist hier kein Beiwerk, sondern das Feature:
- Der Gast darf **ausschließlich** Gamepad-Ereignisse senden, niemals Tastatur/Maus
  (sonst ist es eine Fernwartung, kein Spielgerät).
- Die Übergabe ist immer aktiv vom Gastgeber erteilt, zeitlich begrenzt, jederzeit mit
  einer Taste beendbar (Notbremse muss auch bei Vollbild greifen - dafür existiert der
  fokusunabhängige Gamepad-Hotkey-Poll bereits).
- Eingaben werden ignoriert, sobald das Spiel nicht mehr im Vordergrund ist - sonst
  tippt der Gast im Zweifel im Datei-Explorer herum.
- Sichtbare Anzeige im OSD, wer gerade steuert.

### Offene Fragen vor jedem Code
- Eingabeverzögerung: Zusatzverzögerung des Datenkanals messen, bevor irgendetwas gebaut
  wird. Über ~80 ms ist es kein Spielgefühl mehr. **Erst messen, dann entscheiden.**
- Wie kommt der Gast an einen Controller? Browser-Gamepad-API - Achtung, in der App
  selbst ist `getGamepads` im WebView2 tabu (killt den Xbox-Dongle-Reconnect, s.
  Memory `chromium-gamepad-monitor-falle`); auf der **Zuschauerseite im normalen
  Browser** ist das unproblematisch, muss aber sauber getrennt bleiben.
- Rechtlich/moralisch: Fernsteuerung eines fremden PCs ist ein Vertrauensversprechen.
  Die Voreinstellung ist "aus", und die Einladung muss unmissverständlich sein.

### Empfehlung
Nicht als Nächstes. Als übernächstes Großprojekt - und mit einem Messtag ("wie viel
Verzögerung kostet der Datenkanal?") als Vorbedingung, genau wie beim Analyzer die
<1-%-Overhead-Bedingung vorab galt.

---

## Reihenfolge

1. **Idee 1** - kleinster Aufwand, einziger Selbstverbreitungseffekt, nutzt vorhandene
   Bausteine. Vorschaubild nicht weglassen.
2. **Idee 2** - baut auf denselben Daten auf, macht die Vergleichsansicht erst wertvoll.
3. **Idee 3** - eigenes Großprojekt, Vorbedingung ist eine Verzögerungsmessung.

Vor 1 gehört allerdings noch der offene Punkt aus dem Analyzer-Plan erledigt: der
**A/B-Selbsttest** (presentOnly gegen voll, p99-Delta < 2 %). Wir würden sonst eine
Funktion bewerben, deren Abnahmekriterium nie geprüft wurde.
