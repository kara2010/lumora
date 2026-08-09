// Statische Abnahme der Oberflaechen-Dateien. Prueft genau die Fehler, die ein
// Umbau hinterlaesst und die man dem Markup NICHT ansieht:
//   1) Syntax jedes <script>-Blocks mit dem echten JS-Parser. Ein Syntaxfehler
//      bricht den GANZEN Block ab - die App bleibt dann ewig im Boot-Schirm
//      haengen, weil dessen Ausblenden die letzte Zeile des Blocks ist.
//   2) Waisen in der Uebersetzungstabelle: zweizeilige Eintraege, bei denen nur
//      eine der beiden Zeilen entfernt wurde.
//   3) Kennungen/Handler: von JS gesuchte id= und inline-Handler ohne Funktion.
//   4) Doppelt vergebene id= im Markup.
// Beendet sich mit Code 1, sobald etwas nicht stimmt - taugt als Bau-Sperre.
const fs = require('fs'), vm = require('vm'), path = require('path');

// Wurzel per Argument uebersteuerbar - so laesst sich die Sperre gegen einen
// bekannt kaputten Stand gegenpruefen (sonst weiss man nie, ob sie ueberhaupt
// anschlaegt).
const WURZEL = process.argv[2] ? path.resolve(process.argv[2]) : path.resolve(__dirname, '..');
const DATEIEN = ['index.html', 'osd.html', 'analyze-osd.html', 'analyze-werkbank.html',
  'player.html', 'doorman.html'].filter(f => fs.existsSync(path.join(WURZEL, f)));

// Absichtlich ins Leere greifende Kennungen: der Knopf ist beim Umbau der
// Gruppen-Oberflaeche entfallen, der Zugriff wird bewusst defensiv gehalten.
const BEKANNT = { 'index.html': ['groupStartBtn'] };

let fehler = 0;
const melde = (d, txt) => { fehler++; console.log('FEHLER  ' + d + ': ' + txt); };

for (const datei of DATEIEN) {
  const s = fs.readFileSync(path.join(WURZEL, datei), 'utf8');

  // 1) Syntax je Block
  let nr = 0;
  for (const m of s.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/g)) {
    nr++;
    const startZeile = s.slice(0, m.index).split('\n').length;
    try { new vm.Script(m[1], { filename: 'b' + nr }); }
    catch (e) {
      const lm = (e.stack || '').match(/b\d+:(\d+)/);
      const abs = lm ? startZeile + Number(lm[1]) : startZeile;
      melde(datei, 'Skriptblock ' + nr + ' laesst sich nicht lesen - Zeile ~' + abs + ': ' + e.message);
    }
  }

  // 2) Waisen in der Uebersetzungstabelle
  const ti = s.indexOf('const I18N_EN = {');
  if (ti >= 0) {
    const z = s.split('\n');
    const von = s.slice(0, ti).split('\n').length;
    const bis = s.slice(0, s.indexOf('\n}', ti)).split('\n').length;
    const NURKEY = /^\s*(['"])(?:\\.|(?!\1)[^\\])*\1\s*:\s*$/;
    const KOMPLETT = /^\s*(['"])(?:\\.|(?!\1)[^\\])*\1\s*:\s*\S/;
    const NURWERT = /^\s*(['"])(?:\\.|(?!\1)[^\\])*\1\s*,\s*$/;
    let erwarteWert = 0;
    for (let i = von; i < bis; i++) {
      const l = z[i];
      if (!l.trim() || /^\s*\/\//.test(l)) continue;
      if (erwarteWert) {
        if (NURWERT.test(l)) { erwarteWert = 0; continue; }
        melde(datei, 'Zeile ' + erwarteWert + ': Uebersetzung ohne Wert');
        erwarteWert = 0;
      }
      if (NURKEY.test(l)) { erwarteWert = i + 1; continue; }
      if (KOMPLETT.test(l)) continue;
      if (NURWERT.test(l)) melde(datei, 'Zeile ' + (i + 1) + ': Wert ohne Schluessel (Waise)');
    }
  }

  // 3)+4) Kennungen und Handler
  const markup = ti >= 0 ? s.slice(0, ti) : s;
  const ids = new Map();
  for (const m of s.matchAll(/\sid="([^"]+)"/g)) ids.set(m[1], (ids.get(m[1]) || 0) + 1);
  for (const [k, n] of ids) if (n > 1) melde(datei, 'id="' + k + '" ist ' + n + 'x vergeben');
  for (const m of s.matchAll(/getElementById\(\s*(['"])([^'"]+)\1\s*\)/g))
    if (!ids.has(m[2]) && !(BEKANNT[datei] || []).includes(m[2]))
      melde(datei, 'getElementById("' + m[2] + '") - die Kennung gibt es im Markup nicht');
  const fn = new Set();
  for (const m of s.matchAll(/\bfunction\s+([A-Za-z_$][\w$]*)\s*\(/g)) fn.add(m[1]);
  for (const m of s.matchAll(/\b(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*(?:async\s*)?[({]/g)) fn.add(m[1]);
  // Auch am Fenster-Objekt haengende Funktionen zaehlen: window.foo = () => {}
  for (const m of s.matchAll(/\bwindow\.([A-Za-z_$][\w$]*)\s*=/g)) fn.add(m[1]);
  for (const m of markup.matchAll(/on(?:click|change|input)="([A-Za-z_$][\w$]*)\(/g))
    if (!fn.has(m[1]) && !/^(window|document|console)$/.test(m[1])) melde(datei, 'Handler ' + m[1] + '() gibt es nicht');
}

console.log(fehler ? 'ui-Pruefung: ' + fehler + ' Fund(e)' : 'ui-Pruefung: ok - ' + DATEIEN.length + ' Dateien, Syntax/Tabelle/Kennungen sauber');
process.exit(fehler ? 1 : 0);
