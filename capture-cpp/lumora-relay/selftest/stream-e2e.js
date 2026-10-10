// Ende-zu-Ende-Selbsttest des Streaming-Wegs: Capture -> Relay -> WebRTC -> Browser (libwebrtc).
//
// Warum es diesen Test gibt (2026-10): Mit einem neuen NVIDIA-Treiber blieb das Stream-Bild
// ueberall schwarz - und KEIN bestehendes Pruefmittel sah es. Der Bitstrom war gueltig, das
// Relay meldete "Keyframe durchgelassen, 0 Sendefehler", die Verbindung stand. Der Fehler
// existierte nur auf dem letzten Meter im Browser (Bilder empfangen, 0 dekodiert, PLI-Sturm).
// Genau dort misst dieser Test: ein echter WHEP-Client in Edge (Chromium = dieselbe
// libwebrtc wie WebView2-Vorschau und Chrome) meldet framesDecoded, Codec und Helligkeit.
//
// Laeufe:  1) AV1 (Modus auto)   -> Bild da, Codec AV1, keine PLIs
//          2) H.264 (Modus h264) -> Bild da, Codec H264
//          3) --negative: Relay beschaedigt AV1 absichtlich -> der AV1-Waechter muss auf
//             H.264 umschalten und der Browser danach Bild bekommen
// Voraussetzung: PC mit Encoder-GPU, Lumora beendet (Ports 8558/8889/9997/8189 frei), Edge.
// Aufruf: node capture-cpp/lumora-relay/selftest/stream-e2e.js [--relay <exe>] [--capture <exe>]
//         [--nur av1|h264|negative] [--sichtbar]      Exit 0 = alle Laeufe gruen.
const { spawn, execSync } = require('child_process');
const fs = require('fs'), path = require('path'), http = require('http'), net = require('net'), os = require('os');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const args = process.argv.slice(2);
const arg = (k, d) => { const i = args.indexOf(k); return i >= 0 && args[i + 1] ? args[i + 1] : d; };
const RELAY = path.resolve(arg('--relay', path.join(ROOT, 'bin', 'lumora-media-relay.exe')));
const CAPTURE = path.resolve(arg('--capture', path.join(ROOT, 'bin', 'lumora-capture-native.exe')));
const NUR = arg('--nur', null);
const SICHTBAR = args.includes('--sichtbar');
const EDGE = ['C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
              process.env['LOCALAPPDATA'] + '/Google/Chrome/Application/chrome.exe', 'C:/Program Files/Google/Chrome/Application/chrome.exe'].find(p => fs.existsSync(p));

const log = (s) => console.log('[e2e] ' + s);
const portFrei = (port) => new Promise(res => { const s = net.connect(port, '127.0.0.1'); s.once('connect', () => { s.destroy(); res(false); }); s.once('error', () => res(true)); });
const warteAuf = (proc, muster, ms) => new Promise((res, rej) => {
  let buf = ''; const t = setTimeout(() => rej(new Error('Timeout: "' + muster + '" nicht gesehen. Ausgabe:\n' + buf.slice(-1500))), ms);
  const h = (d) => { buf += d.toString(); if (buf.includes(muster)) { clearTimeout(t); res(buf); } };
  proc.stdout.on('data', h); proc.stderr.on('data', h);
  proc.once('exit', (c) => { clearTimeout(t); rej(new Error('Prozess beendet (Code ' + c + ') bevor "' + muster + '" kam. Ausgabe:\n' + buf.slice(-1500))); });
});
const kill = (p) => { if (p && p.pid && p.exitCode === null) try { execSync('taskkill /PID ' + p.pid + ' /T /F', { stdio: 'ignore' }); } catch {} };

async function lauf(name, relayArgs, dauer, bewerte) {
  log('--- Lauf ' + name + ' ---');
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'lumora-e2e-'));
  const relayLog = []; let relay, capture, browser, server;
  try {
    relay = spawn(RELAY, relayArgs, { cwd: path.dirname(RELAY) });
    relay.stdout.on('data', d => relayLog.push(d.toString()));
    await warteAuf(relay, 'relay bereit', 10000);
    // TEMP umleiten: %TEMP%\lumora-codec.txt der Shell wuerde sonst den Codec ueberstimmen
    capture = spawn(CAPTURE, ['--encoder', 'auto', '--fps', '60', '--bitrate', '12', '--mtx-host', '127.0.0.1', '--mtx-port', '8558',
                              '--codec', 'auto', '--monitor', '0', '--frames', String(60 * (dauer + 30))],
                    { cwd: path.dirname(CAPTURE), env: { ...process.env, TEMP: tmp, TMP: tmp } });
    const capOut = await warteAuf(capture, 'Frames live', 20000);
    const enc = (capOut.match(/Encoder [^\n]*/) || [''])[0];
    log('Capture laeuft: ' + enc.trim());
    // Sonde servieren + Ergebnis entgegennehmen
    const ergebnis = new Promise((res) => {
      server = http.createServer((req, resp) => {
        if (req.method === 'POST' && req.url === '/result') { let b = ''; req.on('data', d => b += d); req.on('end', () => { resp.end('ok'); res(JSON.parse(b)); }); return; }
        resp.setHeader('Content-Type', 'text/html; charset=utf-8'); resp.end(fs.readFileSync(path.join(__dirname, 'whep-probe.html')));
      });
    });
    await new Promise(r => server.listen(0, '127.0.0.1', r));
    const url = 'http://127.0.0.1:' + server.address().port + '/?dauer=' + dauer + '&whep=' + encodeURIComponent('http://127.0.0.1:8889/whep');
    const bargs = [SICHTBAR ? '--new-window' : '--headless=new', '--no-first-run', '--no-default-browser-check', '--autoplay-policy=no-user-gesture-required',
                   '--user-data-dir=' + path.join(tmp, 'edge'), '--window-size=1280,800', url];
    browser = spawn(EDGE, bargs, { stdio: 'ignore' });
    const erg = await Promise.race([ergebnis, new Promise((_, rej) => setTimeout(() => rej(new Error('Sonde meldete nach ' + (dauer + 30) + 's nichts')), (dauer + 30) * 1000))]);
    const letzte = erg.proben[erg.proben.length - 1] || {};
    log('Sonde: ' + JSON.stringify(letzte));
    if (erg.fehler) throw new Error('Sonde: ' + erg.fehler);
    const befund = bewerte(erg, relayLog.join(''));
    if (befund.length) { log('FEHLGESCHLAGEN: ' + name); befund.forEach(b => log('   - ' + b)); return false; }
    log('OK: ' + name); return true;
  } finally {
    kill(browser); kill(capture); kill(relay);
    if (server) server.close();
    await new Promise(r => setTimeout(r, 1500));
    try { fs.rmSync(tmp, { recursive: true, force: true }); } catch {}
  }
}
const bildDa = (p) => p && p.framesDekodiert > 30 && p.lumaMax > 40 && p.w > 0;

(async () => {
  if (!fs.existsSync(RELAY)) { log('Relay fehlt: ' + RELAY); process.exit(2); }
  if (!fs.existsSync(CAPTURE)) { log('Capture fehlt: ' + CAPTURE); process.exit(2); }
  if (!EDGE) { log('Kein Edge/Chrome gefunden'); process.exit(2); }
  for (const p of [8558, 8889, 9997, 8189]) if (!(await portFrei(p))) { log('Port ' + p + ' belegt - laeuft Lumora/ein Stream? Erst beenden.'); process.exit(2); }
  log('Relay: ' + RELAY); log('Capture: ' + CAPTURE); log('Browser: ' + EDGE);
  const ergebnisse = [];
  if (!NUR || NUR === 'av1') ergebnisse.push(await lauf('AV1', ['--codec', 'auto'], 10, (erg) => {
    const p = erg.proben[erg.proben.length - 1]; const f = [];
    if (!bildDa(p)) f.push('kein Bild: framesDekodiert=' + p.framesDekodiert + ' lumaMax=' + p.lumaMax);
    if (!/AV1/i.test(p.codec || '')) f.push('Codec ist ' + p.codec + ', erwartet AV1');
    if (p.pli > 2) f.push('Browser forderte ' + p.pli + ' Keyframes nach - dekodiert er wirklich?');
    return f;
  }));
  if (!NUR || NUR === 'h264') ergebnisse.push(await lauf('H.264', ['--codec', 'h264'], 10, (erg) => {
    const p = erg.proben[erg.proben.length - 1]; const f = [];
    if (!bildDa(p)) f.push('kein Bild: framesDekodiert=' + p.framesDekodiert + ' lumaMax=' + p.lumaMax);
    if (!/H264/i.test(p.codec || '')) f.push('Codec ist ' + p.codec + ', erwartet H264');
    return f;
  }));
  if (!NUR || NUR === 'negative') ergebnisse.push(await lauf('AV1-Waechter (Sabotage -> H.264-Fallback)', ['--codec', 'auto', '--selftest-corrupt-av1'], 20, (erg, rlog) => {
    const p = erg.proben[erg.proben.length - 1]; const f = [];
    if (!/AV1-WAECHTER/.test(rlog)) f.push('Relay hat den Fallback nicht ausgeloest (kein "AV1-WAECHTER" im Log)');
    if (!/H264/i.test(p.codec || '')) f.push('Codec am Ende ' + p.codec + ', erwartet H264 nach Fallback');
    const mitte = erg.proben[Math.floor(erg.proben.length / 2)] || {};
    if (!(p.framesDekodiert > (mitte.framesDekodiert || 0) + 30) || !(p.lumaMax > 40)) f.push('nach dem Fallback kein laufendes Bild: ' + JSON.stringify({ mitte: mitte.framesDekodiert, ende: p.framesDekodiert, lumaMax: p.lumaMax }));
    return f;
  }));
  const ok = ergebnisse.every(Boolean);
  log(ok ? 'ALLE LAEUFE OK' : (ergebnisse.filter(x => !x).length + ' Lauf/Laeufe FEHLGESCHLAGEN'));
  process.exit(ok ? 0 : 1);
})().catch(e => { log('ABBRUCH: ' + e.message); process.exit(1); });
