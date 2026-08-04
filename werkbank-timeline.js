// Zoombare Timeline der Analyse-Werkbank (Design: capture-cpp/ANALYSE-WERKBANK-PLAN.md).
// NUR die Werkbank nutzt dieses Modul - analyze-report.js bleibt die gemeinsame,
// schlanke Quelle fuer Bericht und Website und wird davon nicht aufgeblaeht.
//
// Leitmetapher Profiler: EIN Viewport {t0,t1} ist die einzige Wahrheit, alle Spuren
// zeichnen dieselbe Zeitspanne. Mausrad zoomt auf die Cursorposition, Ziehen
// verschiebt, Doppelklick zeigt alles.
//
// Warum LOD (min/max-Pyramide): ftFull hat bei 30 min/144 fps ~260.000 Punkte. Ein
// naives lineTo je Punkt zeichnet 260k Segmente auf ~1000 Pixelspalten - das ruckelt
// beim Zoomen sichtbar. Die Pyramide reduziert auf 2 Werte je Pixelspalte (min/max),
// die Kurve sieht identisch aus (Spitzen bleiben erhalten, das ist der Kernpunkt bei
// Frametimes) und die Zeichenlast ist unabhaengig von der Sessionlaenge.
(function (root) {
  'use strict';

  var tr = function (t) { return t; };
  function setTranslator(f) { if (typeof f === 'function') tr = f; }

  // ---- Datenaufbereitung ---------------------------------------------------------
  // ftFull -> {t:Float64Array, ft:Float32Array} (Zeitachse kumulativ rekonstruiert,
  // sync-Stuetzpunkte ziehen die Quantisierungsdrift gerade).
  function ftFullEntpacken(ff) {
    if (!ff || !ff.q || !ff.q.length) return null;
    var n = ff.q.length;
    var t = new Float64Array(n), ft = new Float32Array(n);
    var sync = ff.sync || [], si = 0;
    var x = ff.t0 || 0;
    for (var i = 0; i < n; i++) {
      // Stuetzpunkt an diesem Index? Dann Achse exakt setzen (Drift zurueck auf 0).
      while (si < sync.length && sync[si][0] === i) { x = sync[si][1]; si++; }
      t[i] = x;
      ft[i] = ff.q[i] / 100;            // 10-us-Einheiten -> ms
      x += ff.q[i] / 100000;            // -> Sekunden
    }
    return { t: t, ft: ft, n: n };
  }

  // Min/Max-Pyramide: Stufe k fasst 8^k Punkte zusammen.
  function pyramideBauen(serie) {
    var stufen = [], src = serie.ft, srcT = serie.t, n = serie.n;
    var faktor = 8;
    while (n > 512 && stufen.length < 5) {
      var m = Math.ceil(n / faktor);
      var mn = new Float32Array(m), mx = new Float32Array(m), tt = new Float64Array(m);
      for (var i = 0; i < m; i++) {
        var a = i * faktor, b = Math.min(a + faktor, n);
        var lo = Infinity, hi = -Infinity;
        for (var j = a; j < b; j++) { var v = src[j]; if (v < lo) lo = v; if (v > hi) hi = v; }
        mn[i] = lo; mx[i] = hi; tt[i] = srcT[a];
      }
      stufen.push({ min: mn, max: mx, t: tt, n: m, schritt: (stufen.length ? stufen[stufen.length - 1].schritt : 1) * faktor });
      src = mx; srcT = tt; n = m;   // naechste Stufe aus den Maxima (Spitzen bleiben)
    }
    return stufen;
  }

  // ---- Timeline ------------------------------------------------------------------
  // opts: { onSpike(finding), onViewport(t0,t1) }
  function Timeline(canvas, opts) {
    this.cv = canvas;
    this.opts = opts || {};
    this.r = null;            // Report
    this.serie = null;        // entpackte ftFull (oder aus ftSeries gebaut)
    this.pyr = [];
    this.t0 = 0; this.t1 = 1; // Viewport
    this.tMin = 0; this.tMax = 1;
    this.spuren = [];         // sichtbare Spurdefinitionen
    this.hover = null;        // {x, t, idx}
    this.aktiverSpike = null;
    this._binden();
  }

  function serieAusReport(r) {
    var s = ftFullEntpacken(r.ftFull);
    if (!s) {
      // v1-Bericht: ftSeries ([[t,ms],...]) als Ersatzserie - Zoom bleibt moeglich,
      // nur die Tiefe fehlt (2000 Punkte statt aller Frames).
      var ser = r.ftSeries || [];
      s = { t: new Float64Array(ser.length), ft: new Float32Array(ser.length), n: ser.length };
      for (var i = 0; i < ser.length; i++) { s.t[i] = ser[i][0]; s.ft[i] = ser[i][1]; }
    }
    return s;
  }

  Timeline.prototype.laden = function (r) {
    this.r = r;
    var s = serieAusReport(r);
    this.serie = s;
    this.pyr = s.n > 4096 ? pyramideBauen(s) : [];
    this.tMin = s.n ? s.t[0] : 0;
    this.tMax = s.n ? s.t[s.n - 1] : (r.durS || 1);
    if (this.tMax <= this.tMin) this.tMax = this.tMin + 1;
    this.serieB = null; this.pyrB = null; this.rB = null;
    this.aktiverSpike = null;
    this.spurenBauen();
    this.gesamt();
  };

  // Vergleichslauf B als Overlay (nur die Maxima-Linie, halbtransparent violett).
  // Beide Laeufe starten bei ihrer eigenen Session-Zeit ~0 - die Achsen sind damit
  // direkt vergleichbar ("Minute 5 hier vs. Minute 5 dort").
  Timeline.prototype.ladenB = function (r) {
    if (!r) { this.serieB = null; this.pyrB = null; this.rB = null; this.zeichnen(); return; }
    this.rB = r;
    var s = serieAusReport(r);
    this.serieB = s;
    this.pyrB = s.n > 4096 ? pyramideBauen(s) : [];
    if (s.n && s.t[s.n - 1] > this.tMax) { this.tMax = s.t[s.n - 1]; }
    this.gesamt();
  };

  // Welche Spuren hat dieser Bericht? (v1 -> keine; fehlende Spuren werden ausgelassen,
  // statt leere Leisten zu zeigen - eine leere Spur behauptet "gemessen, aber nichts da".)
  Timeline.prototype.spurenBauen = function () {
    this.spuren = [];
    var tk = this.r.tracks;
    if (!tk || !tk.hz) return;
    var dt = 1 / tk.hz;
    var self = this;
    var namen = (this.r.names && this.r.names.procs) || {};
    var drv = (this.r.names && this.r.names.drivers) || {};
    function hatWerte(arr, idx, min) {
      if (!arr || !arr.length) return false;
      for (var i = 0; i < arr.length; i++) if (arr[i] && arr[i][idx] > (min || 0)) return true;
      return false;
    }
    // cpu: [totalPct, maxCorePct, maxCore, topPid, topPct]
    if (hatWerte(tk.cpu, 0)) this.spuren.push({
      key: 'cpu', titel: tr('CPU'), farbe: '#ffc857', max: 100, dt: dt, daten: tk.cpu,
      wert: function (b) { return b[0]; },
      zweit: function (b) { return b[1]; },        // max-Kern als hellere Linie
      info: function (b) {
        return tr('CPU') + ' ' + b[0] + '% · ' + tr('Kern') + ' ' + b[2] + ': ' + b[1] + '%'
             + (b[3] ? ' · ' + (namen[b[3]] || ('PID ' + b[3])) + ' ' + b[4] + '%' : '');
      }
    });
    // gpu: [clockMHz, loadPct, vramMB, throttle]
    if (hatWerte(tk.gpu, 1)) this.spuren.push({
      key: 'gpu', titel: tr('GPU'), farbe: '#35e08a', max: 100, dt: dt, daten: tk.gpu,
      wert: function (b) { return b[1] < 0 ? 0 : b[1]; },
      markiere: function (b) { return b[3] === 1; },   // Throttle rot hinterlegen
      info: function (b) {
        return tr('GPU') + ' ' + (b[1] < 0 ? '–' : b[1] + '%')
             + (b[0] > 0 ? ' · ' + b[0] + ' MHz' : '')
             + (b[2] > 0 ? ' · ' + b[2] + ' MB' : '')
             + (b[3] === 1 ? ' · ' + tr('drosselt') : '');
      }
    });
    // dpc: [maxUs, maxDrv, count]  (logarithmisch: 50us..20ms)
    if (hatWerte(tk.dpc, 0)) this.spuren.push({
      key: 'dpc', titel: tr('DPC/ISR'), farbe: '#9a7bff', max: 100, dt: dt, daten: tk.dpc,
      wert: function (b) { var u = b[0]; return u <= 50 ? 0 : Math.min(100, Math.log(u / 50) / Math.log(400) * 100); },
      info: function (b) {
        return tr('DPC max') + ' ' + (b[0] >= 1000 ? (b[0] / 1000).toFixed(2) + ' ms' : b[0] + ' µs')
             + (b[1] >= 0 && drv[b[1]] ? ' · ' + drv[b[1]] : '') + ' · ' + b[2] + '×';
      }
    });
    // disk: [kb, maxLat10]
    if (hatWerte(tk.disk, 0)) {
      var kbMax = 1;
      for (var i = 0; i < tk.disk.length; i++) if (tk.disk[i][0] > kbMax) kbMax = tk.disk[i][0];
      this.spuren.push({
        key: 'disk', titel: tr('Datenträger'), farbe: '#4ce0ff', max: 100, dt: dt, daten: tk.disk,
        wert: function (b) { return b[0] * 100 / kbMax; },
        info: function (b) {
          return (b[0] >= 1024 ? (b[0] / 1024).toFixed(1) + ' MB' : b[0] + ' KB')
               + ' · ' + tr('Latenz') + ' ' + (b[1] / 10).toFixed(1) + ' ms';
        }
      });
    }
  };

  Timeline.prototype.gesamt = function () {
    this.t0 = this.tMin; this.t1 = this.tMax;
    this.zeichnen();
    if (this.opts.onViewport) this.opts.onViewport(this.t0, this.t1);
  };

  // Viewport auf ein Fenster um t setzen (Spike-Klick aus dem Beweis-Panel)
  Timeline.prototype.zeigeFenster = function (t, spanne) {
    var h = (spanne || 0.4) / 2;
    this.t0 = Math.max(this.tMin, t - h);
    this.t1 = Math.min(this.tMax, t + h);
    if (this.t1 - this.t0 < 0.02) this.t1 = this.t0 + 0.02;
    this.zeichnen();
    if (this.opts.onViewport) this.opts.onViewport(this.t0, this.t1);
  };

  // ---- Interaktion ---------------------------------------------------------------
  Timeline.prototype._binden = function () {
    var self = this, ziehen = null;
    this.cv.addEventListener('wheel', function (e) {
      e.preventDefault();
      if (!self.serie) return;
      var g = self._geo(), rel = (e.clientX - self.cv.getBoundingClientRect().left - g.padL) / g.w;
      rel = Math.max(0, Math.min(1, rel));
      var tCur = self.t0 + (self.t1 - self.t0) * rel;
      var f = e.deltaY > 0 ? 1.25 : 0.8;                       // raus / rein
      var neu = (self.t1 - self.t0) * f;
      var minSpanne = 0.01, maxSpanne = self.tMax - self.tMin;
      neu = Math.max(minSpanne, Math.min(maxSpanne, neu));
      self.t0 = tCur - neu * rel; self.t1 = self.t0 + neu;
      self._klemmen();
      self.zeichnen();
      if (self.opts.onViewport) self.opts.onViewport(self.t0, self.t1);
    }, { passive: false });
    this.cv.addEventListener('mousedown', function (e) {
      ziehen = { x: e.clientX, t0: self.t0, t1: self.t1, bewegt: false };
      self.cv.style.cursor = 'grabbing';
    });
    addEventListener('mousemove', function (e) {
      if (ziehen) {
        var g = self._geo();
        var dt = (e.clientX - ziehen.x) / g.w * (ziehen.t1 - ziehen.t0);
        if (Math.abs(e.clientX - ziehen.x) > 3) ziehen.bewegt = true;
        self.t0 = ziehen.t0 - dt; self.t1 = ziehen.t1 - dt;
        self._klemmen();
        self.zeichnen();
        if (self.opts.onViewport) self.opts.onViewport(self.t0, self.t1);
        return;
      }
      // Hover nur, wenn der Zeiger ueber dem Canvas ist
      var rc = self.cv.getBoundingClientRect();
      if (e.clientX < rc.left || e.clientX > rc.right || e.clientY < rc.top || e.clientY > rc.bottom) {
        if (self.hover) { self.hover = null; self.zeichnen(); }
        return;
      }
      self.hover = { x: e.clientX - rc.left, y: e.clientY - rc.top };
      self.zeichnen();
    });
    addEventListener('mouseup', function (e) {
      if (!ziehen) return;
      var warBewegt = ziehen.bewegt; ziehen = null;
      self.cv.style.cursor = 'crosshair';
      if (!warBewegt) self._klick(e);   // Klick ohne Bewegung = Spike-Auswahl
    });
    this.cv.addEventListener('dblclick', function () { self.gesamt(); });
  };

  Timeline.prototype._klemmen = function () {
    var spanne = this.t1 - this.t0;
    if (this.t0 < this.tMin) { this.t0 = this.tMin; this.t1 = this.t0 + spanne; }
    if (this.t1 > this.tMax) { this.t1 = this.tMax; this.t0 = this.t1 - spanne; }
    if (this.t0 < this.tMin) this.t0 = this.tMin;
  };

  // Klick: naechstgelegenen Spike innerhalb von 10 Pixeln waehlen
  Timeline.prototype._klick = function (e) {
    if (!this.r || !this.r.findings) return;
    var rc = this.cv.getBoundingClientRect(), g = this._geo();
    var x = e.clientX - rc.left;
    var best = null, bestD = 1e9;
    for (var i = 0; i < this.r.findings.length; i++) {
      var f = this.r.findings[i];
      var fx = g.padL + (f.t - this.t0) / (this.t1 - this.t0) * g.w;
      var d = Math.abs(fx - x);
      if (d < bestD) { bestD = d; best = f; }
    }
    if (best && bestD <= 10) {
      this.aktiverSpike = best;
      this.zeichnen();
      if (this.opts.onSpike) this.opts.onSpike(best);
    }
  };

  // ---- Zeichnen ------------------------------------------------------------------
  Timeline.prototype._geo = function () {
    var dpr = root.devicePixelRatio || 1;
    var cw = this.cv.clientWidth || 900, ch = this.cv.clientHeight || 400;
    // padL traegt die Spurnamen ("Datenträger" ist der laengste), padR die
    // fps-Hilfslinien-Beschriftung ("60 fps") - beide waren zu knapp und schnitten ab.
    var padL = 76, padR = 46, padT = 16, padB = 22;
    var spurH = 34, spurLuft = 4;
    var spurGes = this.spuren.length * (spurH + spurLuft);
    var kurveH = Math.max(90, ch - padT - padB - spurGes);
    return { dpr: dpr, cw: cw, ch: ch, padL: padL, padR: padR, padT: padT, padB: padB,
             w: cw - padL - padR, spurH: spurH, spurLuft: spurLuft, kurveH: kurveH };
  };

  Timeline.prototype.zeichnen = function () {
    if (!this.serie) return;
    var g = this._geo(), cv = this.cv, c = cv.getContext('2d');
    cv.width = Math.round(g.cw * g.dpr); cv.height = Math.round(g.ch * g.dpr);
    c.setTransform(g.dpr, 0, 0, g.dpr, 0, 0);
    c.clearRect(0, 0, g.cw, g.ch);

    var t0 = this.t0, t1 = this.t1, spanne = t1 - t0;
    var X = function (t) { return g.padL + (t - t0) / spanne * g.w; };

    // --- Hauptkurve: Skala aus dem SICHTBAREN Bereich - aber nicht aus dem Maximum.
    // Ein einzelner 60-ms-Ruckler drueckte sonst die gesamte Grundlinie (7 ms) auf
    // wenige Pixel zusammen und die Kurve war unlesbar. Stattdessen p99.5 der
    // sichtbaren Werte * 1.35: die Grundlinie fuellt die Hoehe, Ausreisser werden
    // oben GEKAPPT (klar erkennbar: Linie endet am Rand, Spike-Marker zeigt sie an).
    var sicht = this._sichtbar(g.w);
    var skala = 20;
    if (sicht.length) {
      var werte = [];
      for (var i = 0; i < sicht.length; i++) werte.push(sicht[i].hi);
      werte.sort(function (a, b) { return a - b; });
      var p = werte[Math.min(werte.length - 1, Math.floor(werte.length * 0.995))];
      skala = Math.max(20, Math.ceil(p * 1.35 / 5) * 5);
    }
    var kurveOben = g.padT, kurveUnten = g.padT + g.kurveH;
    // Kappung an der Oberkante (s. Skalen-Kommentar oben)
    var Y = function (v) { return Math.max(kurveOben + 2, kurveUnten - (v / skala) * (g.kurveH - 4)); };

    // Gitter: fps-Hilfslinien MIT Einheit (Lehre aus dem Bericht: "30/60" ohne Einheit
    // liest sich als ms-Wert in unsinniger Reihenfolge)
    c.font = '9px Consolas, monospace'; c.textAlign = 'left';
    [16.7, 33.3].forEach(function (ms) {
      var y = Y(ms);
      if (y > kurveOben + 8) {
        c.strokeStyle = 'rgba(255,255,255,.07)'; c.lineWidth = 1;
        c.beginPath(); c.moveTo(g.padL, y + .5); c.lineTo(g.padL + g.w, y + .5); c.stroke();
        c.fillStyle = 'rgba(255,255,255,.32)';
        c.fillText((ms === 16.7 ? '60' : '30') + ' fps', g.padL + g.w + 3, y + 3);
      }
    });
    c.fillStyle = 'rgba(255,255,255,.32)';
    c.fillText(skala + ' ms', 4, kurveOben + 8);

    // Kurve als Min/Max-Band (LOD) + Maxima-Linie: Spitzen bleiben sichtbar
    c.beginPath();
    for (var i2 = 0; i2 < sicht.length; i2++) { var s = sicht[i2]; c.moveTo(s.x + .5, Y(s.lo)); c.lineTo(s.x + .5, Y(s.hi)); }
    c.strokeStyle = 'rgba(96,190,255,.45)'; c.lineWidth = 1; c.stroke();
    c.beginPath();
    for (var i3 = 0; i3 < sicht.length; i3++) { var s3 = sicht[i3]; if (i3) c.lineTo(s3.x, Y(s3.hi)); else c.moveTo(s3.x, Y(s3.hi)); }
    c.strokeStyle = 'rgba(150,215,255,.95)'; c.lineWidth = 1.2; c.stroke();

    // Vergleichslauf B als Overlay-Linie (violett, halbtransparent)
    if (this.serieB) {
      var sichtB = this._sichtbar(g.w, this.serieB, this.pyrB);
      c.beginPath();
      for (var ib = 0; ib < sichtB.length; ib++) { var sb = sichtB[ib]; if (ib) c.lineTo(sb.x, Y(sb.hi)); else c.moveTo(sb.x, Y(sb.hi)); }
      c.strokeStyle = 'rgba(154,123,255,.75)'; c.lineWidth = 1.2; c.stroke();
    }

    // Spike-Marker
    var fs = this.r.findings || [];
    for (var i4 = 0; i4 < fs.length; i4++) {
      var f = fs[i4];
      if (f.t < t0 || f.t > t1) continue;
      var fx = X(f.t), aktiv = this.aktiverSpike && this.aktiverSpike.id === f.id;
      c.strokeStyle = aktiv ? 'rgba(255,200,60,.95)' : 'rgba(255,70,86,.75)';
      c.lineWidth = aktiv ? 2 : 1;
      c.beginPath(); c.moveTo(fx, kurveOben); c.lineTo(fx, kurveUnten); c.stroke();
      c.fillStyle = aktiv ? '#ffc83c' : '#ff4656';
      c.beginPath(); c.arc(fx, kurveOben + 5, aktiv ? 4 : 3, 0, 6.2832); c.fill();
    }

    // --- Spuren
    var y = kurveUnten + 6;
    for (var si = 0; si < this.spuren.length; si++) {
      var sp = this.spuren[si];
      this._spurZeichnen(c, sp, g, y, X);
      y += g.spurH + g.spurLuft;
    }

    // --- Zeitachse
    this._achseZeichnen(c, g, X);

    // --- Hover: Fadenkreuz + Werte
    if (this.hover) this._hoverZeichnen(c, g, X, Y, sicht);
  };

  // Sichtbare Punkte auf Pixelspalten reduzieren (LOD-Stufe nach Zoom waehlen).
  // serie/pyr optional: ohne Angabe die Hauptserie, sonst der Vergleichslauf B.
  Timeline.prototype._sichtbar = function (breite, serieArg, pyrArg) {
    var s = serieArg || this.serie, t0 = this.t0, t1 = this.t1, spanne = t1 - t0;
    var out = [];
    if (!s.n) return out;
    // Punkte je Pixel in der Rohserie schaetzen -> passende Pyramidenstufe
    var gesamtSpanne = this.tMax - this.tMin;
    var rohProPixel = (s.n * (spanne / gesamtSpanne)) / breite;
    var pyr = pyrArg || this.pyr;
    var quelle = { t: s.t, min: s.ft, max: s.ft, n: s.n, schritt: 1 };
    for (var k = 0; k < pyr.length; k++) {
      if (pyr[k].schritt <= rohProPixel * 0.5) quelle = pyr[k]; else break;
    }
    // Binaersuche auf den ersten sichtbaren Index
    var lo = 0, hi = quelle.n - 1;
    while (lo < hi) { var m = (lo + hi) >> 1; if (quelle.t[m] < t0) lo = m + 1; else hi = m; }
    var startI = Math.max(0, lo - 1);
    var proSpalte = {};
    var padL = this._geo().padL;   // EINMAL - _geo() in der Schleife war messbar teuer
    for (var i = startI; i < quelle.n; i++) {
      var t = quelle.t[i];
      if (t > t1) break;
      var x = Math.round((t - t0) / spanne * breite);
      var e = proSpalte[x];
      var vmin = quelle.min[i], vmax = quelle.max[i];
      if (!e) proSpalte[x] = { x: padL + x, lo: vmin, hi: vmax, t: t };
      else { if (vmin < e.lo) e.lo = vmin; if (vmax > e.hi) e.hi = vmax; }
    }
    for (var kx in proSpalte) out.push(proSpalte[kx]);
    out.sort(function (a, b) { return a.x - b.x; });
    return out;
  };

  Timeline.prototype._spurZeichnen = function (c, sp, g, y, X) {
    var h = g.spurH, dt = sp.dt, daten = sp.daten;
    // Rahmen + Titel
    c.fillStyle = 'rgba(255,255,255,.02)';
    c.fillRect(g.padL, y, g.w, h);
    c.strokeStyle = 'rgba(255,255,255,.06)'; c.lineWidth = 1;
    c.strokeRect(g.padL + .5, y + .5, g.w - 1, h - 1);
    c.font = '9px Consolas, monospace'; c.textAlign = 'right';
    c.fillStyle = 'rgba(255,255,255,.4)';
    c.fillText(sp.titel, g.padL - 5, y + 11);

    var i0 = Math.max(0, Math.floor(this.t0 / dt) - 1);
    var i1 = Math.min(daten.length - 1, Math.ceil(this.t1 / dt) + 1);
    // Markierungen (z.B. GPU-Throttle) als Hintergrundband
    if (sp.markiere) {
      c.fillStyle = 'rgba(255,70,86,.16)';
      for (var i = i0; i <= i1; i++) {
        if (!daten[i] || !sp.markiere(daten[i])) continue;
        var xa = X(i * dt), xb = X((i + 1) * dt);
        c.fillRect(Math.max(g.padL, xa), y + 1, Math.max(1, xb - xa), h - 2);
      }
    }
    // Balken/Flaeche
    c.beginPath();
    var erst = true;
    for (var j = i0; j <= i1; j++) {
      if (!daten[j]) continue;
      var v = sp.wert(daten[j]);
      var x = X(j * dt), yy = y + h - 1 - Math.max(0, Math.min(1, v / sp.max)) * (h - 3);
      if (erst) { c.moveTo(x, y + h - 1); c.lineTo(x, yy); erst = false; }
      else c.lineTo(x, yy);
      c.lineTo(X((j + 1) * dt), yy);
    }
    if (!erst) {
      c.lineTo(X((i1 + 1) * dt), y + h - 1);
      c.closePath();
      c.globalAlpha = 0.22; c.fillStyle = sp.farbe; c.fill(); c.globalAlpha = 1;
      c.strokeStyle = sp.farbe; c.lineWidth = 1.1; c.stroke();
    }
    // Zweitwert (z.B. max-Kern) als feine Linie
    if (sp.zweit) {
      c.beginPath(); var e2 = true;
      for (var k = i0; k <= i1; k++) {
        if (!daten[k]) continue;
        var v2 = sp.zweit(daten[k]);
        var x2 = X(k * dt), y2 = y + h - 1 - Math.max(0, Math.min(1, v2 / sp.max)) * (h - 3);
        if (e2) { c.moveTo(x2, y2); e2 = false; } else c.lineTo(x2, y2);
        c.lineTo(X((k + 1) * dt), y2);
      }
      c.strokeStyle = 'rgba(255,255,255,.35)'; c.lineWidth = 1; c.stroke();
    }
    sp._y = y;   // fuer den Hover-Text
  };

  Timeline.prototype._achseZeichnen = function (c, g, X) {
    var t0 = this.t0, t1 = this.t1, spanne = t1 - t0;
    // Schrittweite: 1-2-5-Folge, ~6 Beschriftungen
    var roh = spanne / 6, stufen = [0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600];
    var schritt = stufen[stufen.length - 1];
    for (var i = 0; i < stufen.length; i++) if (stufen[i] >= roh) { schritt = stufen[i]; break; }
    var y = g.padT + g.kurveH + this.spuren.length * (g.spurH + g.spurLuft) + 6;
    c.font = '9px Consolas, monospace'; c.textAlign = 'center';
    c.strokeStyle = 'rgba(255,255,255,.10)'; c.lineWidth = 1;
    c.beginPath(); c.moveTo(g.padL, y + .5); c.lineTo(g.padL + g.w, y + .5); c.stroke();
    var start = Math.ceil(t0 / schritt) * schritt;
    for (var t = start; t <= t1; t += schritt) {
      var x = X(t);
      c.strokeStyle = 'rgba(255,255,255,.16)';
      c.beginPath(); c.moveTo(x, y); c.lineTo(x, y + 4); c.stroke();
      // Beschriftung nur, wenn sie vollstaendig ins Bild passt (lief rechts raus)
      if (x > g.padL + 14 && x < g.padL + g.w - 18) {
        c.fillStyle = 'rgba(255,255,255,.42)';
        c.fillText(zeitLabel(t, schritt), x, y + 14);
      }
    }
  };

  function zeitLabel(t, schritt) {
    var m = Math.floor(t / 60), s = t - m * 60;
    if (schritt < 1) return m + ':' + (s < 10 ? '0' : '') + s.toFixed(schritt < 0.1 ? 2 : 1);
    return m + ':' + (s < 10 ? '0' : '') + Math.round(s);
  }

  Timeline.prototype._hoverZeichnen = function (c, g, X, Y, sicht) {
    var hx = this.hover.x;
    if (hx < g.padL || hx > g.padL + g.w) return;
    var t = this.t0 + (hx - g.padL) / g.w * (this.t1 - this.t0);
    c.strokeStyle = 'rgba(255,255,255,.22)'; c.lineWidth = 1;
    c.beginPath(); c.moveTo(hx + .5, g.padT); c.lineTo(hx + .5, g.padT + g.kurveH); c.stroke();
    // naechster Kurvenwert
    var nah = null, nd = 1e9;
    for (var i = 0; i < sicht.length; i++) { var d = Math.abs(sicht[i].x - hx); if (d < nd) { nd = d; nah = sicht[i]; } }
    var zeilen = [zeitLabel(t, 0.01)];
    if (nah) zeilen.push(nah.hi.toFixed(1) + ' ms' + (nah.hi > nah.lo + 0.05 ? ' (' + tr('max') + ')' : ''));
    var tk = this.r.tracks;
    if (tk && tk.hz) {
      var idx = Math.floor(t * tk.hz);
      for (var si = 0; si < this.spuren.length; si++) {
        var sp = this.spuren[si];
        var b = sp.daten[idx];
        if (b) zeilen.push(sp.info(b));
      }
    }
    // Kasten
    c.font = '10px Consolas, monospace'; c.textAlign = 'left';
    var bw = 0;
    for (var z = 0; z < zeilen.length; z++) bw = Math.max(bw, c.measureText(zeilen[z]).width);
    bw += 14;
    var bh = zeilen.length * 13 + 8;
    var bx = hx + 12 + bw > g.padL + g.w ? hx - bw - 12 : hx + 12;
    var by = Math.min(g.padT + 6, g.ch - bh - 4);
    c.fillStyle = 'rgba(8,12,20,.92)';
    c.strokeStyle = 'rgba(76,224,255,.28)';
    c.beginPath();
    if (c.roundRect) c.roundRect(bx, by, bw, bh, 6); else c.rect(bx, by, bw, bh);
    c.fill(); c.stroke();
    for (var z2 = 0; z2 < zeilen.length; z2++) {
      c.fillStyle = z2 === 0 ? '#eaf6ff' : 'rgba(217,234,245,.85)';
      c.fillText(zeilen[z2], bx + 7, by + 15 + z2 * 13);
    }
  };

  root.WerkbankTimeline = { Timeline: Timeline, setTranslator: setTranslator, ftFullEntpacken: ftFullEntpacken };
})(typeof window !== 'undefined' ? window : this);
