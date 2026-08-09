// Prueft alle relativen Verweise der Website gegen die Dateien auf der Platte.
// Anlass: die englischen Seiten liegen in en/, die PHP-Endpunkte aber im Wurzel-
// verzeichnis. Ein vergessenes "../" zeigt dann auf /en/download.php - eine Seite,
// die es nie gab. Auffaellig wird das erst Wochen spaeter in Bings Site Scan.
const fs = require('fs'), path = require('path');

const WURZEL = path.resolve(__dirname, '..', 'website');
// updates/ ist der Update-Feed mit archivierten Kopien der APP-Oberflaeche - dort
// stehen in href/src JS-Vorlagen statt Pfaden. Keine Seiten, kein Pruefgegenstand.
const AUSSEN = /^(node_modules|\.git|updates)$/;
const dateien = [];
(function sammle(d) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) { if (!AUSSEN.test(e.name)) sammle(p); }
    else if (/\.html$/i.test(e.name)) dateien.push(p);
  }
})(WURZEL);

let fehler = 0;
for (const datei of dateien) {
  const s = fs.readFileSync(datei, 'utf8');
  const ordner = path.dirname(datei);
  const gesehen = new Set();
  for (const m of s.matchAll(/(?:href|src|action)="([^"#][^"]*)"/g)) {
    let ziel = m[1].trim();
    if (/^(https?:|mailto:|tel:|data:|javascript:|#|\/\/)/i.test(ziel)) continue;
    if (/\$\{|'\s*\+|\+\s*'/.test(ziel)) continue;   // zur Laufzeit gebauter Pfad
    ziel = ziel.split('#')[0].split('?')[0];
    if (!ziel) continue;
    if (gesehen.has(ziel)) continue;
    gesehen.add(ziel);
    const abs = ziel.startsWith('/') ? path.join(WURZEL, ziel) : path.resolve(ordner, ziel);
    if (fs.existsSync(abs)) continue;
    if (fs.existsSync(abs + '/index.html') || fs.existsSync(path.join(abs, 'index.html'))) continue;
    fehler++;
    console.log('FEHLER  ' + path.relative(WURZEL, datei).replace(/\\/g, '/')
      + ': "' + ziel + '" -> ' + path.relative(WURZEL, abs).replace(/\\/g, '/') + ' gibt es nicht');
  }
}
console.log(fehler ? 'Link-Pruefung: ' + fehler + ' toter Verweis(e) in ' + dateien.length + ' Seiten'
                   : 'Link-Pruefung: ok - ' + dateien.length + ' Seiten, kein toter relativer Verweis');
process.exit(fehler ? 1 : 0);
