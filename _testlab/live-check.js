// Pflicht-Gegenpruefung nach dem Online-Deploy (RELEASE.md 10). Prueft die
// AUSGELIEFERTE Datei, nicht nur Versionszahlen: Content-Disposition + Laenge des
// Download-Endpunkts, sha512 im Alt-Feed gegen die echte Live-Datei, beide Feeds,
// beide Startseiten, das geteilte Berichts-Modul. Cache-Buster an jeder URL.
//
// Aufruf: node _testlab/live-check.js <version>
// Version und Groessen-Text werden aus dem GEBAUTEN Installer abgeleitet, nicht
// von Hand gepflegt: eine im Skript vergessene Zahl meldet sonst einen Fehler,
// den es gar nicht gibt (real passiert - Version hochgezogen, Groesse vergessen).
// Ausserdem liegt die Datei jetzt im Repo statt im Scratchpad: PowerShell-
// Ersetzungen darin haben zweimal die Umlaute zerlegt ([[powershell-zerstoert-utf8]]).
const https = require('https'), crypto = require('crypto'), fs = require('fs'), path = require('path');

const VER = process.argv[2];
if (!/^\d+\.\d+\.\d+$/.test(VER || '')) { console.error('Aufruf: node _testlab/live-check.js <version>'); process.exit(2); }
const LOKAL = path.resolve(__dirname, '..', 'capture-cpp', 'lumora-shell', 'Lumora-Native-Setup-' + VER + '.exe');
if (!fs.existsSync(LOKAL)) { console.error('Gebauter Installer fehlt: ' + LOKAL); process.exit(2); }

const lokal = fs.readFileSync(LOKAL);
const lokalSha256 = crypto.createHash('sha256').update(lokal).digest('hex');
// Groessen-Text wie auf der Website: dezimale MB, zwei Nachkommastellen -> "3,79" / "3.79"
const mbZahl = (lokal.length / 1000000).toFixed(2);
const mbDe = mbZahl.replace('.', ','), mbEn = mbZahl;

const cb = () => 'cb=' + Date.now() + Math.floor(Math.random() * 1e6);
const hol = (url) => new Promise((res, rej) => {
  https.get(url, { headers: { 'User-Agent': 'lumora-release-check', 'Cache-Control': 'no-cache' } }, (r) => {
    if (r.statusCode >= 300 && r.statusCode < 400 && r.headers.location) return hol(new URL(r.headers.location, url).href).then(res, rej);
    const t = []; r.on('data', (d) => t.push(d));
    r.on('end', () => res({ code: r.statusCode, kopf: r.headers, buf: Buffer.concat(t) }));
  }).on('error', rej);
});

let fehler = 0;
const pruef = (was, ok, detail) => { console.log((ok ? 'OK   ' : 'FEHL ') + was + (detail ? '  ' + detail : '')); if (!ok) fehler++; };

(async () => {
  // 1) Download-Endpunkt: liefert er wirklich die neue Datei?
  const d = await hol('https://lumora-streaming.de/download.php?' + cb());
  pruef('download.php liefert Setup ' + VER,
    String(d.kopf['content-disposition'] || '').includes('Lumora-Native-Setup-' + VER + '.exe'),
    String(d.kopf['content-disposition'] || '').trim());
  pruef('download.php Groesse == gebauter Installer', d.buf.length === lokal.length, d.buf.length + ' Bytes');
  pruef('download.php Inhalt byte-identisch', crypto.createHash('sha256').update(d.buf).digest('hex') === lokalSha256);

  // 2) Feeds
  const nu = JSON.parse((await hol('https://lumora-streaming.de/updates/native-update.json?' + cb())).buf.toString('utf8'));
  pruef('native-update.json version', nu.version === VER, nu.version);
  pruef('native-update.json url zeigt auf ' + VER, String(nu.url).endsWith(VER + '.exe'));
  // Umlaute per Codepoint pruefen, nicht per Literal: ein doppelt kodiertes Skript
  // meldete sonst einen Fehler in einer voellig gesunden Datei.
  const hatUmlaut = [...(nu.notes || '')].some((c) => [0xE4, 0xF6, 0xFC, 0xDF, 0xC4, 0xD6, 0xDC].includes(c.codePointAt(0)));
  const hatMojibake = /Ã[ -¿]/.test(nu.notes || '');
  pruef('native-update.json Notizen: Umlaute heil, kein Mojibake', hatUmlaut && !hatMojibake);

  const cj = JSON.parse((await hol('https://lumora-streaming.de/updates/components.json?' + cb())).buf.toString('utf8'));
  pruef('components.json version', cj.version === VER, cj.version);

  const yml = (await hol('https://lumora-streaming.de/updates/latest.yml?' + cb())).buf.toString('utf8');
  pruef('latest.yml version', new RegExp('^version: ' + VER.replace(/\./g, '\\.') + '$', 'm').test(yml));
  const shaY = (yml.match(/sha512:\s*(\S+)/) || [])[1];
  const live = await hol('https://lumora-streaming.de/updates/Lumora-Native-Setup-' + VER + '.exe?' + cb());
  pruef('latest.yml sha512 == Live-Setup', shaY === crypto.createHash('sha512').update(live.buf).digest('base64'));
  pruef('Live-Setup == lokal gebaut', crypto.createHash('sha256').update(live.buf).digest('hex') === lokalSha256);

  // 3) Website
  for (const [name, url, mb] of [['DE', 'https://lumora-streaming.de/?', mbDe], ['EN', 'https://lumora-streaming.de/en/?', mbEn]]) {
    const s = (await hol(url + cb())).buf.toString('utf8');
    pruef(name + '-Startseite softwareVersion', s.includes('"softwareVersion": "' + VER + '"'));
    pruef(name + '-Startseite Changelog nennt ' + VER, s.includes('release-ver">' + VER + '<'));
    pruef(name + '-Startseite Download-Groesse ' + mb + ' MB',
      new RegExp(mb.replace('.', '\\.') + '(&nbsp;| )MB').test(s));
  }
  for (const [was, url] of [['site.css', 'https://lumora-streaming.de/site.css?'], ['sitemap.xml', 'https://lumora-streaming.de/sitemap.xml?']]) {
    const r = await hol(url + cb());
    pruef(was + ' erreichbar', r.code === 200, 'HTTP ' + r.code);
  }
  // 4) Geteiltes Berichts-Modul: App, Werkbank und die Webseite MUESSEN dieselbe
  // Fassung fahren - sonst zeigt der geteilte Bericht andere Farben als die App.
  const modul = await hol('https://lumora-streaming.de/analyze-report.js?' + cb());
  pruef('analyze-report.js live == lokal',
    crypto.createHash('sha256').update(modul.buf).digest('hex')
    === crypto.createHash('sha256').update(fs.readFileSync(path.resolve(__dirname, '..', 'analyze-report.js'))).digest('hex'));

  console.log('\n' + (fehler ? 'LIVE-PRUEFUNG: ' + fehler + ' FEHLER' : 'LIVE-PRUEFUNG: alles gruen'));
  process.exit(fehler ? 1 : 0);
})();
