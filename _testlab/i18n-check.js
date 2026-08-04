// Uebersetzungsluecken in index.html finden (RELEASE.md Schritt 3).
// Laeuft automatisch als Schritt 0 von build-installer.ps1 und BRICHT DEN BAU AB,
// wenn eine Luecke bleibt - so kann keine Version mehr mit halb uebersetzter
// Oberflaeche rausgehen (Anlass: 3.1.0 bis 3.2.0 hatten 7 stille Luecken).
//
// Warum das noetig ist: Schluessel ist der DEUTSCHE SATZ selbst
// (tr('Spielzeit zuruecksetzen')). Aendert jemand ein Komma im deutschen Text,
// findet der Schluessel nichts mehr - und die englische Oberflaeche zeigt an
// dieser Stelle STILL Deutsch. Kein Fehler, keine Warnung, nur dieser Check.
//
// Aufruf:  node _testlab/i18n-check.js [index.html] [i18n-ignore.txt] [minKeys]
// minKeys: Plausibilitaetsschwelle "Parser kaputt?" - Standard 100 (index.html hat
// >500 Schluessel); kleinere Seiten (analyze-werkbank.html) uebergeben ihren Wert.
// Exit 0 = keine Luecken, Exit 1 = Luecken (Liste auf stdout), Exit 2 = Aufrufproblem.
const fs = require('fs');
const path = require('path');

const htmlPath = process.argv[2] || path.join(__dirname, '..', 'index.html');
const ignorePath = process.argv[3] || path.join(__dirname, 'i18n-ignore.txt');
const minKeys = parseInt(process.argv[4], 10) || 100;

let src;
try { src = fs.readFileSync(htmlPath, 'utf8'); }
catch (e) { console.log('FEHLER: ' + htmlPath + ' nicht lesbar (' + e.code + ')'); process.exit(2); }

// --- Ausnahmen laden (Eigennamen, Marken, Tastenbezeichnungen, Pfade ...) ---
// Eine Zeichenkette pro Zeile; '#' leitet einen Kommentar ein, Leerzeilen egal.
const ignore = new Set();
try {
  for (const line of fs.readFileSync(ignorePath, 'utf8').split('\n')) {
    const t = line.replace(/\s+#.*$/, '').trim();
    if (t && !t.startsWith('#')) ignore.add(t);
  }
} catch (e) { /* ohne Ausnahmeliste laeuft der Check trotzdem, nur strenger */ }

function dec(s) {
  return s.replace(/&auml;/g, 'ä').replace(/&ouml;/g, 'ö').replace(/&uuml;/g, 'ü')
    .replace(/&Auml;/g, 'Ä').replace(/&Ouml;/g, 'Ö').replace(/&Uuml;/g, 'Ü')
    .replace(/&szlig;/g, 'ß').replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"').replace(/&nbsp;/g, ' ').replace(/&#(\d+);/g, (_, n) => String.fromCharCode(+n));
}
// Normalisierung EXAKT wie applyI18n() zur Laufzeit: entity-dekodiert, Whitespace gefaltet.
function norm(s) { return dec(s).replace(/\s+/g, ' ').trim(); }
function unq(lit) {
  return lit.slice(1, -1)
    .replace(/\\"/g, '"').replace(/\\'/g, "'").replace(/\\n/g, '\n').replace(/\\\\/g, '\\');
}

// --- I18N_EN-Block ausschneiden (Klammern zaehlen statt Regex ueber mehrere Zeilen) ---
const dictStart = src.indexOf('const I18N_EN = {');
if (dictStart < 0) { console.log('FEHLER: I18N_EN nicht gefunden'); process.exit(2); }
let depth = 0, end = -1;
for (let i = src.indexOf('{', dictStart); i < src.length; i++) {
  if (src[i] === '{') depth++;
  else if (src[i] === '}') { depth--; if (!depth) { end = i; break; } }
}
if (end < 0) { console.log('FEHLER: I18N_EN-Block nicht abgeschlossen'); process.exit(2); }
const dictSrc = src.slice(dictStart, end + 1);

// Schluessel: pro ZEILE der erste String vor einem Doppelpunkt.
// (Zeilenbasiert, weil zwischen den Eintraegen Kommentarzeilen stehen.)
const keys = new Set();
for (const line of dictSrc.split('\n')) {
  const t = line.trim();
  if (!t || t.startsWith('//')) continue;
  const m = /^("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')\s*:/.exec(t);
  if (m) keys.add(norm(unq(m[1])));
}
if (keys.size < minKeys) { console.log('FEHLER: nur ' + keys.size + ' Schluessel gelesen (minKeys=' + minKeys + ') - Parser passt nicht mehr'); process.exit(2); }

// Braucht dieser Text ueberhaupt eine Uebersetzung?
function needsTranslation(s) {
  if (!s || s.length < 2) return false;
  if (ignore.has(s)) return false;
  if (!/[a-zA-ZäöüÄÖÜß]/.test(s)) return false;                          // reine Zahlen/Symbole
  if (/^[\d\s.,:%°/+×–-]+$/.test(s)) return false;
  if (/^(alt|strg|ctrl|shift|f\d+)([+\s].*)?$/i.test(s)) return false;   // Tastenkuerzel
  if (/^\d+\s*(p|fps|ms|mbit|kbit|mb|gb|hz|%|°c|w)$/i.test(s)) return false;
  if (/^https?:\/\//i.test(s)) return false;                             // URLs
  if (/^[a-z]:\\/i.test(s)) return false;                                // Windows-Pfade
  return true;
}

const missing = [];
const seen = new Set();
function check(text, quelle) {
  const n = norm(text);
  if (!n || !needsTranslation(n) || keys.has(n) || seen.has(n)) return;
  seen.add(n);
  missing.push({ text: n, quelle });
}

// --- 1) tr("...")-Aufrufe ---
const trRe = /\btr\(\s*("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')/g;
let m;
while ((m = trRe.exec(src))) check(unq(m[1]), 'tr()');

// --- 2) sichtbare HTML-Textknoten + uebersetzte Attribute (ohne script/style) ---
const noCode = src.replace(/<script[\s\S]*?<\/script>/gi, '').replace(/<style[\s\S]*?<\/style>/gi, '');
const textRe = />([^<>]+)</g;
while ((m = textRe.exec(noCode))) check(m[1], 'HTML-Text');
const attrRe = /\b(title|placeholder|aria-label)\s*=\s*"([^"]+)"/g;
while ((m = attrRe.exec(noCode))) check(m[2], m[1] + '-Attribut');

console.log('i18n: ' + keys.size + ' Schluessel, ' + ignore.size + ' Ausnahmen, ' + missing.length + ' Luecke(n)');
if (!missing.length) process.exit(0);

console.log('');
console.log('UEBERSETZUNGSLUECKEN - diese Texte fehlen in I18N_EN:');
for (const x of missing) console.log('  [' + x.quelle + '] ' + x.text);
console.log('');
console.log('Entweder in I18N_EN (index.html) eine Uebersetzung ergaenzen,');
console.log('ODER - falls der Text in beiden Sprachen identisch ist (Marke, Kuerzel,');
console.log('Pfad) - eine Zeile in ' + path.basename(ignorePath) + ' aufnehmen.');
process.exit(1);
