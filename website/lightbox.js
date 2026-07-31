/* Vollbild-Ansicht fuer Screenshots auf den Unterseiten.
   Sammelt alle <img class="shot"> der Seite zu einer Galerie – gleiche Bedienung
   wie die Galerie der Startseite: Klick vergroessert, Pfeile/Wischen blaettern,
   Esc schliesst. Bildunterschrift kommt aus dem folgenden <p class="shot-cap">,
   ersatzweise aus dem alt-Text. Ohne Bilder tut das Skript nichts. */
(function () {
  var shots = Array.prototype.slice.call(document.querySelectorAll('img.shot'));
  if (!shots.length) return;

  var en = (document.documentElement.lang || '').toLowerCase().indexOf('en') === 0;
  var T = en
    ? { close: 'Close', prev: 'Previous image', next: 'Next image', zoom: 'Enlarge image' }
    : { close: 'Schließen', prev: 'Vorheriges Bild', next: 'Nächstes Bild', zoom: 'Bild vergrößern' };

  // Bildunterschrift: bevorzugt die sichtbare Beschriftung, sonst der alt-Text
  var items = shots.map(function (img) {
    var cap = img.nextElementSibling;
    var text = (cap && cap.classList.contains('shot-cap')) ? cap.textContent.trim() : '';
    return { img: img, cap: text || img.getAttribute('alt') || '' };
  });
  var many = items.length > 1;

  var lb = document.createElement('div');
  lb.className = 'lightbox';
  lb.innerHTML =
    '<span class="lightbox-close" aria-label="' + T.close + '">✕</span>' +
    (many ? '<button class="lightbox-nav lightbox-prev" aria-label="' + T.prev + '">‹</button>' : '') +
    '<img alt="">' +
    (many ? '<button class="lightbox-nav lightbox-next" aria-label="' + T.next + '">›</button>' : '') +
    '<div class="lightbox-caption"></div>';
  document.body.appendChild(lb);

  var lbImg = lb.querySelector('img');
  var lbCap = lb.querySelector('.lightbox-caption');
  var index = 0;

  function set(i) {
    index = (i + items.length) % items.length;
    var it = items[index];
    lbImg.src = it.img.currentSrc || it.img.src;
    lbImg.alt = it.img.getAttribute('alt') || '';
    lbCap.textContent = it.cap;
    lbCap.style.display = it.cap ? '' : 'none';
  }
  function go(dir, ev) { if (ev) ev.stopPropagation(); set(index + dir); }
  function open(i) { set(i); lb.classList.add('open'); }
  function close(ev) { if (ev) ev.stopPropagation(); lb.classList.remove('open'); }

  items.forEach(function (it, i) {
    it.img.style.cursor = 'zoom-in';
    it.img.setAttribute('role', 'button');
    it.img.setAttribute('tabindex', '0');
    it.img.setAttribute('title', T.zoom);
    it.img.addEventListener('click', function () { open(i); });
    it.img.addEventListener('keydown', function (e) {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); open(i); }
    });
  });

  lb.addEventListener('click', close);
  lbImg.addEventListener('click', function (e) { e.stopPropagation(); });
  lb.querySelector('.lightbox-close').addEventListener('click', close);
  if (many) {
    lb.querySelector('.lightbox-prev').addEventListener('click', function (e) { go(-1, e); });
    lb.querySelector('.lightbox-next').addEventListener('click', function (e) { go(1, e); });
  }

  document.addEventListener('keydown', function (e) {
    if (!lb.classList.contains('open')) return;
    if (e.key === 'Escape') close();
    else if (many && e.key === 'ArrowLeft') go(-1);
    else if (many && e.key === 'ArrowRight') go(1);
  });

  // Wisch-Geste fuers Smartphone
  if (many) {
    var x0 = null;
    lb.addEventListener('touchstart', function (e) { x0 = e.changedTouches[0].clientX; }, { passive: true });
    lb.addEventListener('touchend', function (e) {
      if (x0 === null) return;
      var dx = e.changedTouches[0].clientX - x0;
      if (Math.abs(dx) > 45) go(dx < 0 ? 1 : -1);
      x0 = null;
    }, { passive: true });
  }
})();
