# Plan: Steam-Deck-Integration (Decky-Plugin, LAN)

> Status: **in Planung** (kein Code). Zuletzt besprochen 2026-07-25.
> Getroffene Entscheidungen: **Renderer = Decky-Plugin (Stufe B) direkt**, **Reichweite = erst nur LAN**.
> Offene Entscheidungen: siehe Abschnitt 9.

## Geerdeter Ist-Stand (verifiziert)
- Zuschauer sehen den Stream über `player.html` (Lumoras HTTP-Server auf Port **8787**, WHEP/WebRTC dahinter, Proxy vor mediamtx/Relay).
- OSD-Werte sind heute **rein host-lokal**: FPS/Sensoren im Windows-Shared-Memory (`Local\LumoraOSDFps` / `Local\LumoraOSDSense`), die Shell schiebt sie in die lokale `osd.html`.
- Der **gestreamte Videoframe enthält das OSD NICHT** (separates DComp-Overlay-Fenster, nicht Teil der Aufnahme). Der Deck bekommt die Werte aktuell also gar nicht.
- Relevante Quellen: `capture-cpp/lumora-shell/main.cpp` (`readMahm`, `readSenseCpu`, FPS aus `g_fpsShm`, HTTP-Handler um `BROADCAST_PORT` 8787), Settings `osdFields`/`osdTheme`.

## 1. Zielbild
Generisches Decky-Plugin auf dem Deck zeigt die Host-OSD-Werte. **Lumora = einzige Quelle der Wahrheit**: welche Felder, Reihenfolge, Labels, Theme. Das Plugin rendert stur, was Lumora liefert. LAN-only in Stufe 1.

## 2. Architektur (zwei entkoppelte Teile)
```
[Windows-Host: Lumora]                         [Steam Deck: Decky-Plugin]
 Sensoren/Broker (existiert)                     Python-Backend: pollt /osd (2-4 Hz)
   -> Shared Memory (existiert)                      |
   -> NEU: /osd-Endpoint (Port 8787) --- LAN --->  React-Frontend: rendert generisch
     { meta, config, values } als JSON                (QAM-Panel / Overlay)
```
- Host baut nur einen Datenpunkt, kein Deck-Wissen.
- Deck rendert nur, kein Lumora-Wissen ausser dem Schema.

## 3. Datenvertrag (Herzstueck - hier steckt "generisch + konfigurierbar")
`GET /osd` liefert JSON, z. B.:
```jsonc
{
  "meta":   { "host": "KARA-PC", "ver": "3.0.2", "ts": 172, "streamActive": true },
  "config": {                       // aus Lumoras osdFields/osdTheme (Settings)
    "theme": "bar",
    "fields": [                     // Reihenfolge = Anzeige-Reihenfolge
      { "id": "fps",     "label": "FPS", "unit": "",   "warn": 45, "crit": 30 },
      { "id": "cpuTemp", "label": "CPU", "unit": "C",  "warn": 80, "crit": 90 },
      { "id": "gpuLoad", "label": "GPU", "unit": "%" }
    ]
  },
  "values": { "fps": 118, "cpuTemp": 62, "gpuLoad": 74 }   // nur IDs aus config.fields
}
```
- `config` steuert die Darstellung, `values` sind austauschbare Zahlen. Feldauswahl in Lumora aendern -> naechster Poll -> Deck zieht nach, ohne Plugin-Update.
- Trennung config/values erlaubt sparsame Updates (config selten, values 2-4 Hz).

## 4. Lumora-Seite (reutzt Bestehendes)
- **Endpoint** `/osd` im bestehenden HTTP-Server auf 8787 (neben `player.html`/`/whep`).
- **values** aus den vorhandenen Lesern (`readMahm`, `readSenseCpu`, FPS aus `LumoraOSDFps`).
- **config** aus Settings `osdFields` + `osdTheme` -> Konfiguration genau dort, wo das lokale OSD schon eingestellt wird.
- **Broker-Bedarf:** Werte gibt es nur, wenn die Broker laufen. Ein verbundener Deck-Client muss den Datenfluss anwerfen (analog `brokersEnsure`, aber durch Deck-Nachfrage getriggert) - auch wenn das lokale OSD aus ist.
- **Pairing/Sicherheit (LAN):** Host-IP im Plugin eintragen ODER mDNS-Discovery (`_lumora._tcp`). Kleiner Token in URL/Header, damit nicht jedes WLAN-Geraet mitliest.

## 5. Steam-Deck-Seite (das neue, groessere Stueck)
- Decky-Plugin = eigenes Projekt: TS/React-Frontend (`@decky/ui`) + Python-Backend (`main.py`), `plugin.json`.
- Backend pollt `/osd`, cached, reicht ans Frontend (Decky-Bridge).
- Frontend rendert generisch aus `config.fields` (Label/Unit/Warn-Schwellen -> Farbe).
- Distribution: Decky-Store-Einreichung (Review) ODER manuelles Sideload. Decky-Loader ist Community, nicht Valve - muss auf dem Deck installiert sein.

## 6. Zwei ehrliche Knackpunkte
1. **Wo erscheint das Overlay im Gaming Mode?** Einfacher Weg = Quick-Access-Menue (...-Knopf -> Panel). Ein immer sichtbares Overlay ueber dem laufenden Stream (wie die Deck-eigene Performance-Anzeige) ist mit Decky deutlich aufwaendiger (Gamescope-Layer). Realistisch fuer v1: QAM-Panel.
2. **Wie laeuft der Lumora-Stream auf dem Deck?** Kein nativer Lumora-Linux-Client. Pragmatisch: `player.html` in einem Browser (als Nicht-Steam-Spiel), Vollbild im Gaming Mode; Decky-Plugin legt Stats daneben. Anmerkung: Laeuft der Stream ohnehin im Browser, braechte ein Stats-Overlay *in* `player.html` (Stufe A) fast dasselbe fuer einen Bruchteil des Aufwands - Mehrwert des Plugins liegt v. a. im QAM-Panel/nativen Gefuehl.

## 7. Zum Testen noetig
- Physisches Steam Deck mit installiertem Decky Loader (VM reicht kaum - Gamescope/Gaming Mode).
- Node/pnpm + Decky-CLI-Toolchain auf dem Dev-PC.

## 8. Grobe Phasen (bei Umsetzung)
1. Datenvertrag festzurren (Schema + `/osd`-Endpoint, Broker-on-demand, Token).
2. Plugin-Geruest (Decky-Hello-World auf dem Deck lauffaehig).
3. Generischer Renderer (QAM-Panel aus `config.fields`).
4. Pairing/Discovery (IP-Eingabe -> optional mDNS).
5. Politur (Reconnect, Stale-Werte, Warn-Farben, Icon/Store-Eintrag).

## 9. Offene Entscheidungen
- **Feld-Quelle:** Deck zeigt dieselben Felder wie das lokale OSD, oder eigenes "Deck-Profil" (knapper)?
- **Overlay-Flaeche:** QAM-Panel fuer v1 ok, oder Immer-sichtbar-Overlay ein Muss (dann groesserer Gamescope-Aufwand)?
- **Discovery:** manuelle IP-Eingabe fuer v1, oder gleich mDNS?
- **Stream-Konsum auf dem Deck:** Browser-als-Nicht-Steam-Spiel als Annahme ok, oder anderer Weg?
