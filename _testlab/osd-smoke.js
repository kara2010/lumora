// Rauchtest fuer osd.html: fuehrt das OSD-Skript mit einem minimalen DOM WIRKLICH aus
// und baut jedes der sechs Designs mit ALLEN Wertegruppen (inkl. NET) auf.
//
// Warum das noetig ist: ein Syntax-Check ist bestanden, auch wenn eine aufgerufene
// Funktion gar nicht existiert. Genau das ist in 3.2.6 passiert - beim Umbau der
// Renderer ging netSeg() verloren, jeder Renderer rief sie weiter auf, und das OSD
// blieb beim Anwender komplett leer (ReferenceError im Panel-Aufbau). Dieser Test
// haette das sofort gemeldet.
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const htmlPfad = path.join(__dirname, '..', 'osd.html');
const html = fs.readFileSync(htmlPfad, 'utf8');
let js = '';
for (const m of html.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/g)) js += m[1] + '\n';
if (!js.trim()) { console.error('FEHLER: kein Inline-Skript in osd.html gefunden'); process.exit(1); }

// --- minimaler DOM ---------------------------------------------------------------
function macheElement(id) {
  const kinder = [];
  const el = {
    id, _html: '', textContent: '', value: '',
    style: { setProperty() {}, removeProperty() {} },
    classList: {
      _s: new Set(),
      add(...c) { c.forEach(x => this._s.add(x)); },
      remove(...c) { c.forEach(x => this._s.delete(x)); },
      toggle(c, an) { if (an === undefined) an = !this._s.has(c); an ? this._s.add(c) : this._s.delete(c); return an; },
      contains(c) { return this._s.has(c); },
    },
    addEventListener() {}, removeEventListener() {},
    getBoundingClientRect: () => ({ width: 300, height: 200, left: 0, top: 0 }),
    appendChild(k) { kinder.push(k); return k; },
    insertBefore(k) { kinder.push(k); return k; },
    querySelector() { return null; },
    querySelectorAll() { return []; },
    closest() { return null; },
    getContext: () => ({
      clearRect() {}, beginPath() {}, moveTo() {}, lineTo() {}, stroke() {}, fill() {},
      arc() {}, fillText() {}, setLineDash() {}, fillRect() {}, save() {}, restore() {},
    }),
    get innerHTML() { return this._html; },
    set innerHTML(v) { this._html = String(v); },
    width: 320, height: 46,
  };
  return el;
}
const elemente = new Map();
function holeElement(id) {
  if (!elemente.has(id)) elemente.set(id, macheElement(id));
  return elemente.get(id);
}
const document = {
  getElementById: (id) => holeElement(id),
  querySelector: () => null,
  querySelectorAll: () => [],
  createElement: (t) => macheElement('neu-' + t),
  addEventListener() {},
  body: macheElement('body'),
  documentElement: macheElement('html'),
};

let bounds = null;
const sandbox = {
  document, console,
  window: { devicePixelRatio: 1, performance: { now: () => 0 }, addEventListener() {} },
  performance: { now: () => 0 },
  requestAnimationFrame: (f) => { f(); return 1; },
  cancelAnimationFrame() {},
  setTimeout: (f) => { return 1; },       // nichts asynchron nachlaufen lassen
  clearTimeout() {}, setInterval: () => 1, clearInterval() {},
  ResizeObserver: class { observe() {} disconnect() {} },
  ipcRenderer: { invoke: (kanal, arg) => { if (kanal === 'osd-bounds') bounds = arg; return Promise.resolve(); }, on() {}, send() {} },
  require: () => ({ ipcRenderer: sandboxIpc }),
  URLSearchParams, URL, location: { href: 'https://app.lumora/osd.html', search: '' },
  navigator: { userAgent: 'test' },
};
const sandboxIpc = sandbox.ipcRenderer;
sandbox.globalThis = sandbox;

const ctx = vm.createContext(sandbox);
try {
  vm.runInContext(js, ctx, { filename: 'osd.html', timeout: 10000 });
} catch (e) {
  console.error('FEHLER beim Laden des OSD-Skripts: ' + e.message);
  process.exit(1);
}

// --- jedes Design mit allen Gruppen rendern ---------------------------------------
const DESIGNS = ['compact', 'min', 'bar', 'neon', 'strip', 'tiles'];
const ALLE_FELDER = {
  gpu: ['load', 'temp', 'power', 'clock', 'vram'],
  cpu: ['load', 'temp', 'clock', 'power', 'ram'],
  fps: ['fps', 'frametime', 'graph'],
  net: ['rate', 'total'],
};
let fehler = 0;
for (const design of DESIGNS) {
  for (const reihenfolge of [['gpu','cpu','fps','net'], ['net','fps','cpu','gpu']]) {
    try {
      vm.runInContext(
        'applyConfig(' + JSON.stringify({
          theme: design, fields: ALLE_FELDER, order: reihenfolge,
          corner: 'tl', opacity: 0.55, accent: '#74e857',
        }) + ')', ctx, { timeout: 10000 });
      // Werte einspeisen (Netz an) - deckt auch paintValues ab
      vm.runInContext(
        'updateOsd(' + JSON.stringify({
          gpu: { name: 'GPU', brand: 'nv', load: 50, temp: 60, power: 120, clock: 2000, vram: 8000 },
          cpu: { name: 'CPU', load: 30, temp: 55, clock: 4500, power: 80, ram: 16000 },
          fps: 120, frametime: 8.3,
          net: { rate: '↓1.0 ↑0.2 MB/s', total: '512 MB' },
        }) + ')', ctx, { timeout: 10000 });
      const inhalt = ctx.document.getElementById('panel').innerHTML;
      if (!inhalt || inhalt.length < 20) {
        console.log('FEHL ' + design + ' [' + reihenfolge.join(',') + ']: Panel ist leer');
        fehler++;
      } else if (!/NET/.test(inhalt)) {
        console.log('FEHL ' + design + ' [' + reihenfolge.join(',') + ']: NET-Gruppe fehlt im Panel');
        fehler++;
      } else {
        console.log('OK   ' + design + ' [' + reihenfolge.join(',') + ']  (' + inhalt.length + ' Zeichen)');
      }
    } catch (e) {
      console.log('FEHL ' + design + ' [' + reihenfolge.join(',') + ']: ' + e.message);
      fehler++;
    }
  }
}
console.log('');
if (fehler) { console.log('osd-Rauchtest: FEHLER (' + fehler + ')'); process.exit(1); }
console.log('osd-Rauchtest: ok - alle Designs bauen ihr Panel auf, NET-Gruppe erscheint.');
