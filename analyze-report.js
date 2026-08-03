// Gemeinsame Darstellung eines Ruckler-Berichts - EINE Quelle fuer App UND Webseite.
//
// Diese Datei wird von zwei Seiten geladen:
//   - index.html  (Analyse-Reiter der App)
//   - website/bericht.php (geteilter Bericht im Browser)
//
// BEWUSST herausgeloest statt kopiert: Kopien driften auseinander, und genau das hat
// uns mehrfach Fehler gekostet (Lightbox-Kopie auf der Streaming-Seite, Klick-durch nur
// im Gaming-OSD gefixt, recentFt nur an einer von zwei Lesestellen). Wer hier etwas
// aendert, aendert es automatisch an beiden Orten.
//
// Eingabe ist ausschliesslich das Berichts-JSON (Schema v1, geschrieben von
// runAnalyzeBroker in osd_broker.h). Keine IPC, kein DOM ausserhalb der uebergebenen
// Elemente, keine Abhaengigkeiten. Uebersetzt wird ueber eine Funktion, die der
// Aufrufer stellt (die App reicht window.tr durch, die Webseite ihre eigene).
(function (root) {
  'use strict';

  // --- Uebersetzung: der Aufrufer setzt LumoraReport.setTranslator(fn) ---
  var tr = function (s) { return s; };

  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"]/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
    });
  }

  var KIND = {
    driver: 'Treiber', process: 'Programm', 'gpu-throttle': 'GPU-Drosselung',
    vram: 'Grafikspeicher', disk: 'Datenträger', 'proc-start': 'Programmstart',
    'game-internal': 'Spielintern'
  };
  var LIMIT = {
    gpu: 'GPU-Limit', cpu: 'CPU-Limit', 'cpu-core': 'Einzelkern-Limit',
    framecap: 'FPS-Limit (VSync/Limiter)', throttle: 'GPU drosselt',
    vram: 'Grafikspeicher voll', unknown: 'Nicht eindeutig'
  };
  var LIMIT_COL = {
    gpu: '#35e08a', cpu: '#ffb03a', 'cpu-core': '#ffb03a', framecap: '#4ce0ff',
    throttle: '#ff2a3c', vram: '#ff2a3c', unknown: '#6d86a0'
  };

  function verdictColor(k) {
    return k === 'clean' ? '#3fbf6a' : (k === 'game-internal' ? '#d8a13a' : '#ff6b6b');
  }

  // Urteil in Klartext. verdictKey + Zahlen -> Satz; die UI uebersetzt.
  function verdict(r) {
    var n = esc(r.verdictName || ''), h = r.verdictHits | 0, t = r.spikes | 0;
    switch (r.verdictKey) {
      case 'clean': return tr('Sauberer Lauf – keine Ruckler über der Schwelle. 👍');
      case 'driver': return tr('Hauptverdächtiger: der Treiber ') + tr('„') + n + tr('“') + tr(' – auffällig bei ') + h + '/' + t + ' ' + tr('Rucklern');
      case 'process': return tr('Hauptverdächtiger: das Programm ') + tr('„') + n + tr('“') + tr(' – auffällig bei ') + h + '/' + t + ' ' + tr('Rucklern');
      case 'gpu-throttle': return tr('Hauptursache: die Grafikkarte drosselt') + ' (' + n + ') – ' + h + '/' + t;
      case 'vram': return tr('Hauptverdacht: Grafikspeicher lief voll – bei ') + h + '/' + t + ' ' + tr('Rucklern');
      case 'disk': return tr('Hauptverdacht: Datenträger-Last – bei ') + h + '/' + t + ' ' + tr('Rucklern');
      case 'proc-start': return tr('Hauptverdacht: Programmstarts im Hintergrund') + (n ? ' (' + tr('„') + n + tr('“') + ')' : '') + ' – ' + h + '/' + t;
      default: return tr('Kein Systemstörer gefunden – die Ruckler kommen vermutlich aus dem Spiel selbst (z. B. Shader/Nachladen).');
    }
  }

  // Frametime-Kurve: DPR-scharf, Verlaufs-Fuellung, 60/30-fps-Gitter, Spike-Marker.
  // scaleFactor > 1 = groessere Darstellung (Webseite), Beschriftungen wachsen mit.
  function drawChart(cv, series, findings, opt) {
    opt = opt || {};
    var f = opt.scaleFactor || 1;
    var dpr = root.devicePixelRatio || 1;
    var w = cv.clientWidth || 640, h = cv.clientHeight || 150;
    cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr);
    var c = cv.getContext('2d'); c.setTransform(dpr, 0, 0, dpr, 0, 0);
    c.clearRect(0, 0, w, h);
    if (!series || !series.length) return;
    var t0 = series[0][0], t1 = series[series.length - 1][0] || 1;
    var mx = 0; for (var i = 0; i < series.length; i++) if (series[i][1] > mx) mx = series[i][1];
    var scale = Math.max(50, Math.ceil(mx * 1.15 / 10) * 10);
    // padR traegt die Achsenbeschriftung rechts. Sie enthaelt zwei VERSCHIEDENE
    // Einheiten: oben die ms-Obergrenze, darunter die fps-Hilfslinien. Ohne die
    // Einheit an den fps-Linien las sich die Spalte als "70ms / 30 / 60" - drei
    // ms-Werte in unsinniger Reihenfolge. Einheit dran, dafuer etwas breiter.
    var padR = 46 * f, padB = 16 * f;
    var X = function (t) { return ((t - t0) / Math.max(0.001, t1 - t0)) * (w - padR) + 2; };
    var Y = function (v) { return h - padB - Math.min(1, v / scale) * (h - padB - 8 * f); };
    c.strokeStyle = 'rgba(255,255,255,.08)'; c.lineWidth = 1;
    c.fillStyle = 'rgba(255,255,255,.35)'; c.font = (9 * f) + 'px Consolas,monospace'; c.textAlign = 'left';
    [16.7, 33.3].forEach(function (ms) {
      var y = Y(ms);
      if (y > 8 * f) {
        c.beginPath(); c.moveTo(2, y + .5); c.lineTo(w - padR + 2, y + .5); c.stroke();
        c.fillText((ms === 16.7 ? '60' : '30') + ' fps', w - padR + 6, y + 3 * f);
      }
    });
    c.fillText(scale + 'ms', w - padR + 2, 10 * f);
    var g = c.createLinearGradient(0, 0, 0, h);
    g.addColorStop(0, 'rgba(96,190,255,.35)'); g.addColorStop(1, 'rgba(96,190,255,.02)');
    c.beginPath(); c.moveTo(X(series[0][0]), h - padB);
    for (var j = 0; j < series.length; j++) c.lineTo(X(series[j][0]), Y(series[j][1]));
    c.lineTo(X(series[series.length - 1][0]), h - padB); c.closePath(); c.fillStyle = g; c.fill();
    c.beginPath();
    series.forEach(function (p, i2) { i2 ? c.lineTo(X(p[0]), Y(p[1])) : c.moveTo(X(p[0]), Y(p[1])); });
    c.strokeStyle = 'rgba(140,210,255,.95)'; c.lineWidth = 1.4 * f; c.stroke();
    (findings || []).forEach(function (fd) {
      var x = X(fd.t);
      c.strokeStyle = 'rgba(255,90,90,.8)'; c.lineWidth = 1;
      c.beginPath(); c.moveTo(x + .5, 4 * f); c.lineTo(x + .5, h - padB); c.stroke();
      c.fillStyle = 'rgba(255,90,90,.9)';
      c.beginPath(); c.arc(x + .5, Y(Math.min(fd.ftMs, scale)), 2.6 * f, 0, 7); c.fill();
    });
  }

  // Nur der Dateiname, NIE der Pfad. Die App reinigt bereits vor dem Hochladen -
  // aber die Anzeige darf sich darauf nicht verlassen: in Pfaden steckt regelmaessig
  // der Windows-Benutzername, und ein geteilter Bericht ist oeffentlich.
  function dateiname(p) {
    if (!p) return '';
    var x = String(p).split('\\').join('/');
    return x.slice(x.lastIndexOf('/') + 1);
  }

  function statTiles(r) {
    var tile = function (k, v, sub) {
      return '<div class="an-tile"><div class="k">' + k + '</div><div class="v">' + v + '</div>'
        + (sub ? '<div class="s">' + sub + '</div>' : '') + '</div>';
    };
    var mins = Math.round((r.durS || 0) / 60 * 10) / 10;
    return '<div class="an-tiles">'
      + tile(tr('Dauer'), mins + ' min', esc(dateiname(r.game || '')))
      + tile(tr('Ø FPS'), (r.avgFps || 0).toFixed(0), tr('Median ') + (r.medianFtMs || 0).toFixed(1) + ' ms')
      + tile(tr('1%-Low'), (r.p1LowFps || 0).toFixed(0) + ' fps', 'p99 ' + (r.p99FtMs || 0).toFixed(1) + ' ms')
      + tile(tr('Ruckler'), r.spikes | 0, (r.spikesPerMin || 0).toFixed(1) + '/min') + '</div>';
  }

  // Flaschenhals: dominante Ursache + Verteilung. Bewusst nur bei genug Sekundenproben -
  // sonst behaupten wir nichts.
  function limitSection(r) {
    var L = r.limit;
    if (!L || !L.samples || L.samples < 10) return '';
    var order = ['gpu', 'cpu', 'cpu-core', 'framecap', 'throttle', 'vram', 'unknown'];
    var rows = order.filter(function (k) { return (L[k] | 0) > 0; }).map(function (k) {
      return '<div class="an-aggrow"><span class="n">' + tr(LIMIT[k]) + '</span>'
        + '<span class="bar"><i style="width:' + (L[k] | 0) + '%;background:' + LIMIT_COL[k] + '"></i></span>'
        + '<span class="h">' + (L[k] | 0) + '%</span></div>';
    }).join('');
    var topTxt = L.top && L.top !== 'unknown'
      ? tr('Überwiegend') + ': <b>' + esc(tr(LIMIT[L.top] || L.top)) + '</b> (' + (L.topPct | 0) + '% ' + tr('der Messzeit') + ')'
      : tr('Kein klares Limit erkennbar.');
    return '<h4 class="an-sub">' + tr('Flaschenhals') + '</h4>'
      + '<p class="settings-hint" style="margin:2px 0 8px">' + topTxt + '</p>'
      + '<div class="an-agg">' + rows + '</div>'
      + '<p class="settings-hint" style="margin-top:-4px">'
      + tr('Ermittelt aus GPU-Auslastung, Kernlast und Frametime-Gleichmäßigkeit – eine Einschätzung, keine frame-genaue Messung.')
      + '</p>';
  }

  // Verdaechtigen-Balken (aggregate) - in der App und auf der Webseite gleich.
  function suspectSection(r) {
    if (!r.aggregate || !r.aggregate.length) return '';
    // Jeder Wert aus dem Bericht wird escaped bzw. auf eine Zahl gezwungen, BEVOR er in
    // innerHTML landet. Der Server whitelistet zwar schon (kind nur aus erlaubter Menge),
    // aber dieselbe Funktion rendert auch in der App - und die darf sich nicht auf den
    // Server verlassen. a.kind unescaped waere ein gespeichertes XSS gewesen.
    var mxH = Math.max.apply(null, r.aggregate.map(function (a) { return a.hits | 0; })) || 1;
    return '<div class="an-agg">' + r.aggregate.map(function (a) {
      var hits = a.hits | 0;
      return '<div class="an-aggrow"><span class="n">' + esc(tr(KIND[a.kind] || a.kind))
        + (a.name ? ': ' + esc(a.name) : '') + '</span>'
        + '<span class="bar"><i style="width:' + Math.round(hits / mxH * 100) + '%"></i></span>'
        + '<span class="h">' + hits + '×</span></div>';
    }).join('') + '</div>';
  }

  // Kontextzeile (Aufloesung, HDR, GPU, Treiber) - fehlende Felder fallen weg.
  function metaLine(r) {
    var m = r.context || {};
    return [esc(m.resolution || ''), m.hdr ? 'HDR' : '', m.streaming ? tr('Streaming aktiv') : '',
            esc(r.gpu || ''), r.gpuDriver ? tr('Treiber ') + esc(r.gpuDriver) : '']
      .filter(Boolean).join(' · ');
  }

  root.LumoraReport = {
    setTranslator: function (fn) { if (typeof fn === 'function') tr = fn; },
    esc: esc, verdict: verdict, verdictColor: verdictColor,
    drawChart: drawChart, statTiles: statTiles, limitSection: limitSection,
    suspectSection: suspectSection, metaLine: metaLine,
    KIND: KIND, LIMIT: LIMIT, LIMIT_COL: LIMIT_COL
  };
})(typeof window !== 'undefined' ? window : this);
