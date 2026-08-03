// Baut den geteilten Ruckler-Bericht aus dem eingebetteten JSON auf.
// Die Darstellung selbst kommt aus analyze-report.js - DERSELBEN Datei, die auch die
// App benutzt. Hier steht nur das Drumherum: Sprache, Seitengeruest, Fussbereich.
(function () {
  'use strict';

  var el = document.getElementById('berichtsdaten');
  var app = document.getElementById('app');
  if (!el || !app) return;

  var r;
  try { r = JSON.parse(el.textContent); } catch (e) { r = null; }
  if (!r) { app.innerHTML = '<div class="lade">Dieser Bericht lässt sich nicht lesen.</div>'; return; }

  // --- Sprache: Browsereinstellung, per ?lang= überschreibbar ---
  var q = new URLSearchParams(location.search);
  var lang = q.get('lang') || ((navigator.language || 'en').toLowerCase().indexOf('de') === 0 ? 'de' : 'en');

  // Nur die Zeichenketten, die analyze-report.js und diese Seite ausgeben.
  var EN = {
    '„': '"', '“': '"',
    'Sauberer Lauf – keine Ruckler über der Schwelle. 👍': 'Clean run – no stutters above the threshold. 👍',
    'Hauptverdächtiger: der Treiber ': 'Prime suspect: the driver ',
    'Hauptverdächtiger: das Programm ': 'Prime suspect: the program ',
    ' – auffällig bei ': ' – involved in ',
    'Rucklern': 'stutters',
    'Hauptursache: die Grafikkarte drosselt': 'Main cause: the graphics card is throttling',
    'Hauptverdacht: Grafikspeicher lief voll – bei ': 'Main suspicion: video memory ran full – in ',
    'Hauptverdacht: Datenträger-Last – bei ': 'Main suspicion: disk load – in ',
    'Hauptverdacht: Programmstarts im Hintergrund': 'Main suspicion: programs starting in the background',
    'Kein Systemstörer gefunden – die Ruckler kommen vermutlich aus dem Spiel selbst (z. B. Shader/Nachladen).':
      'No system culprit found – the stutters most likely come from the game itself (e.g. shader compilation or streaming assets).',
    'Dauer': 'Duration', 'Ø FPS': 'Avg FPS', 'Median ': 'Median ', '1%-Low': '1% low',
    'Ruckler': 'Stutters', 'Flaschenhals': 'Bottleneck', 'Überwiegend': 'Predominantly',
    'der Messzeit': 'of the measurement', 'Kein klares Limit erkennbar.': 'No clear limit detected.',
    'Ermittelt aus GPU-Auslastung, Kernlast und Frametime-Gleichmäßigkeit – eine Einschätzung, keine frame-genaue Messung.':
      'Derived from GPU load, per-core load and frame time consistency – an assessment, not a frame-exact measurement.',
    'GPU-Limit': 'GPU limit', 'CPU-Limit': 'CPU limit', 'Einzelkern-Limit': 'Single-core limit',
    'FPS-Limit (VSync/Limiter)': 'FPS cap (VSync/limiter)', 'GPU drosselt': 'GPU throttling',
    'Grafikspeicher voll': 'Video memory full', 'Nicht eindeutig': 'Inconclusive',
    'Treiber': 'Driver', 'Programm': 'Program', 'GPU-Drosselung': 'GPU throttling',
    'Grafikspeicher': 'Video memory', 'Datenträger': 'Disk', 'Programmstart': 'Program start',
    'Spielintern': 'In-game', 'Streaming aktiv': 'streaming active', 'Treiber ': 'driver ',
    // Seitentexte
    'Ruckler-Analyse': 'Stutter analysis', 'Verdächtige': 'Suspects',
    'Frametime-Verlauf': 'Frame time', 'Messung': 'Measurement',
    'Frametime': 'Frame time', 'Erkannter Ruckler': 'Detected stutter',
    'Gemessen mit Lumora': 'Measured with Lumora',
    'Lumora findet heraus, warum dein Spiel ruckelt – kostenlos und quelloffen.':
      'Lumora finds out why your game stutters – free and open source.',
    'Lumora herunterladen': 'Download Lumora',
    'Dieser Bericht wurde freiwillig geteilt. Lumora sammelt keine Daten von sich aus.':
      'This report was shared voluntarily. Lumora does not collect any data on its own.',
    'Spiele-Launcher mit Ruckler-Analyse': 'Game launcher with stutter analysis'
  };
  function tr(s) { return lang === 'en' ? (EN[s] !== undefined ? EN[s] : s) : s; }
  LumoraReport.setTranslator(tr);

  var esc = LumoraReport.esc;
  var farbe = LumoraReport.verdictColor(r.verdictKey);

  // Zeitpunkt lesbar machen ("2026-08-03T21:14:02" -> ortsübliche Schreibweise)
  var zeit = '';
  if (r.wall) {
    var d = new Date(String(r.wall).replace(' ', 'T'));
    zeit = isNaN(d.getTime()) ? String(r.wall)
      : d.toLocaleString(lang === 'en' ? 'en-GB' : 'de-DE',
          { day: '2-digit', month: 'short', year: 'numeric', hour: '2-digit', minute: '2-digit' });
  }

  var meta = LumoraReport.metaLine(r);
  var metaTeile = meta ? meta.split(' · ').map(function (t) { return '<span>' + t + '</span>'; }).join('') : '';

  var suspects = LumoraReport.suspectSection(r);
  var limit = LumoraReport.limitSection(r);

  app.innerHTML =
    '<div class="kopf">'
    + '<a class="marke" href="https://lumora-streaming.de/' + (lang === 'en' ? 'en/' : '') + '">'
    + '<img src="/icon-64.png" alt="" width="30" height="30">'
    + '<span><b>Lumora</b><span>' + tr('Spiele-Launcher mit Ruckler-Analyse') + '</span></span></a>'
    + (zeit ? '<div class="kopf-zeit">' + esc(zeit) + '</div>' : '')
    + '</div>'

    + '<section class="urteil" style="--tint:' + farbe + '">'
    + '<div class="urteil-kopf"><span class="urteil-punkt"></span>' + tr('Ruckler-Analyse') + '</div>'
    + '<p class="urteil-text">' + LumoraReport.verdict(r) + '</p>'
    + (metaTeile ? '<div class="urteil-meta">' + metaTeile + '</div>' : '')
    + '</section>'

    + LumoraReport.statTiles(r)

    + '<section class="block">'
    + '<h2>' + tr('Frametime-Verlauf') + '</h2>'
    + '<canvas class="an-chart" id="kurve"></canvas>'
    + '<div class="chart-legende">'
    + '<span><i style="background:rgba(140,210,255,.95)"></i>' + tr('Frametime') + '</span>'
    + '<span><i style="background:rgba(255,90,90,.9)"></i>' + tr('Erkannter Ruckler') + '</span>'
    + '</div></section>'

    + (suspects ? '<section class="block"><h2>' + tr('Verdächtige') + '</h2>' + suspects + '</section>' : '')
    + (limit ? '<section class="block">' + limit + '</section>' : '')

    + '<div class="fuss"><div class="fuss-txt">'
    + '<b>' + tr('Gemessen mit Lumora') + '</b>'
    + '<span>' + tr('Lumora findet heraus, warum dein Spiel ruckelt – kostenlos und quelloffen.') + '</span>'
    + '</div><a class="cta" href="https://lumora-streaming.de/' + (lang === 'en' ? 'en/' : '') + '">'
    + tr('Lumora herunterladen') + '</a></div>'
    + '<p class="klein">' + tr('Dieser Bericht wurde freiwillig geteilt. Lumora sammelt keine Daten von sich aus.') + '</p>';

  // Kurve zeichnen - grösser als in der App, deshalb scaleFactor.
  function zeichnen() {
    var cv = document.getElementById('kurve');
    if (cv) LumoraReport.drawChart(cv, r.ftSeries, r.findings, { scaleFactor: 1.6 });
  }
  requestAnimationFrame(zeichnen);

  // Bei Grössenänderung neu zeichnen (Canvas ist pixelbasiert, nicht skalierbar).
  var t = null;
  addEventListener('resize', function () { clearTimeout(t); t = setTimeout(zeichnen, 120); });

  document.documentElement.lang = lang;
})();
