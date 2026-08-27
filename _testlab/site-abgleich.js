// Vergleicht JEDE lokale Website-Datei mit dem Live-Stand (sha256). Vor einem Deploy
// muss der Unterschied zum LIVE-Stand vollstaendig bekannt sein - nicht nur die Dateien
// der aktuellen Teilaufgabe. PHP liefert gerendertes Ergebnis statt Quelltext und wird
// getrennt aufgefuehrt. Zusaetzlich geprueft: analyze-report.js aus dem REPO-Wurzel-
// verzeichnis (bericht.php bindet es ein; es lag ausserhalb von website/ und fiel
// deshalb einmal aus dem Abgleich - die Live-Seite fuhr eine veraltete Fassung).
const fs = require('fs'), path = require('path'), https = require('https'), crypto = require('crypto');
const W = path.resolve(__dirname, '..', 'website');
const BASIS = 'https://lumora-streaming.de/';

const dateien = [];
(function sammle(d) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) { if (e.name !== 'updates') sammle(p); }
    else dateien.push(p);
  }
})(W);

const hol = (url) => new Promise((res) => {
  https.get(url + '?cb=' + Date.now(), { headers: { 'User-Agent': 'lumora-abgleich' } }, (r) => {
    const t = []; r.on('data', (d) => t.push(d));
    r.on('end', () => res({ code: r.statusCode, buf: Buffer.concat(t) }));
  }).on('error', () => res({ code: 0, buf: Buffer.alloc(0) }));
});
const sha = (b) => crypto.createHash('sha256').update(b).digest('hex');

(async () => {
  const anders = [], fehlt = [], sonst = [], php = [];
  let gleich = 0;
  for (const p of dateien) {
    const rel = path.relative(W, p).split(path.sep).join('/');
    if (/\.php(\.example)?$/i.test(rel) || /^deploy\./i.test(rel)) { php.push(rel); continue; }
    const r = await hol(BASIS + rel.split('/').map(encodeURIComponent).join('/'));
    if (r.code === 404) { fehlt.push(rel); continue; }
    if (r.code !== 200) { sonst.push(rel + ' (HTTP ' + r.code + ')'); continue; }
    if (sha(fs.readFileSync(p)) === sha(r.buf)) gleich++; else anders.push(rel);
  }
  // Wurzel-Datei, die die Website mit ausliefert
  const wr = await hol(BASIS + 'analyze-report.js');
  if (wr.code === 200 && sha(wr.buf) !== sha(fs.readFileSync(path.resolve(__dirname, '..', 'analyze-report.js'))))
    anders.push('(Wurzel) analyze-report.js');
  else if (wr.code === 200) gleich++;

  console.log('gleich   : ' + gleich);
  console.log('ANDERS   : ' + anders.length); anders.forEach((x) => console.log('    ' + x));
  console.log('LIVE 404 : ' + fehlt.length); fehlt.forEach((x) => console.log('    ' + x));
  console.log('anderer Code: ' + sonst.length); sonst.forEach((x) => console.log('    ' + x));
  console.log('nicht vergleichbar (PHP/Deploy): ' + php.length);
})();
