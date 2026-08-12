// Abnahme des GETEILTEN Berichts-Moduls analyze-report.js (App-Reiter, Werkbank und
// die Webseite benutzen dieselbe Datei). Geprueft wird das, was hier real schiefging:
// verdictColor nimmt den Schluessel ODER den ganzen Bericht - die Werkbank rief es mit
// dem Objekt auf, ein Objekt ist nie === 'clean', und so war JEDER Lauf-Punkt rot,
// ohne dass irgendein Test oder Bau-Schritt das gemerkt haette.
const fs = require('fs'), vm = require('vm'), path = require('path');

const WURZEL = path.resolve(__dirname, '..');
const quelle = fs.readFileSync(path.join(WURZEL, 'analyze-report.js'), 'utf8');
const sandkasten = { window: {}, document: { createElement: () => ({ style: {} }) } };
sandkasten.self = sandkasten; sandkasten.globalThis = sandkasten;
vm.createContext(sandkasten);
try { new vm.Script(quelle, { filename: 'analyze-report.js' }).runInContext(sandkasten); }
catch (e) { console.log('FEHLER analyze-report.js laesst sich nicht laden: ' + e.message); process.exit(1); }

const R = sandkasten.window.LumoraReport || sandkasten.LumoraReport;
let fehler = 0;
const pruef = (was, ok, detail) => {
  console.log((ok ? 'OK   ' : 'FEHL ') + was + (detail ? '  ' + detail : ''));
  if (!ok) fehler++;
};

pruef('LumoraReport wird bereitgestellt', !!R);
if (!R) { process.exit(1); }

// 1) verdictColor: gleiche Farbe, egal ob Schluessel oder Bericht uebergeben wird
const FARBEN = { clean: '#3fbf6a', 'game-internal': '#d8a13a', driver: '#ff6b6b',
  process: '#ff6b6b', vram: '#ff6b6b', disk: '#ff6b6b' };
for (const [k, erwartet] of Object.entries(FARBEN)) {
  const perSchluessel = R.verdictColor(k);
  const perObjekt = R.verdictColor({ verdictKey: k, game: 'x.exe', spikes: 3 });
  pruef('verdictColor("' + k + '") == ' + erwartet, perSchluessel === erwartet, perSchluessel);
  pruef('verdictColor({verdictKey:"' + k + '"}) liefert dasselbe', perObjekt === perSchluessel,
    'Objekt=' + perObjekt + ' Schluessel=' + perSchluessel);
}
// Die drei Klassen muessen UNTERSCHEIDBAR bleiben - sonst faellt ein Fallback in
// den Standardfall und alles sieht gleich aus (genau das Symptom).
const klassen = new Set(['clean', 'game-internal', 'driver'].map((k) => R.verdictColor({ verdictKey: k })));
pruef('gruen / bernstein / rot sind drei verschiedene Farben', klassen.size === 3, [...klassen].join(' '));

// 2) verdict(): Klartext-Satz je Urteil, nie leer
R.setTranslator((t) => t);
for (const k of ['clean', 'driver', 'process', 'gpu-throttle', 'vram', 'disk', 'proc-start', 'game-internal']) {
  const s = R.verdict({ verdictKey: k, verdictName: 'test.sys', verdictHits: 2, spikes: 5 });
  pruef('verdict("' + k + '") liefert Text', typeof s === 'string' && s.trim().length > 5, (s || '').slice(0, 40));
}

// 3) KIND/LIMIT-Tabellen: die UI schlaegt darin nach, leere Eintraege waeren blinde Labels
pruef('KIND-Tabelle gefuellt', R.KIND && Object.keys(R.KIND).length >= 7);
pruef('LIMIT-Tabelle gefuellt', R.LIMIT && Object.keys(R.LIMIT).length >= 6);

console.log(fehler ? '\nbericht-Pruefung: ' + fehler + ' Fund(e)' : '\nbericht-Pruefung: ok');
process.exit(fehler ? 1 : 0);
