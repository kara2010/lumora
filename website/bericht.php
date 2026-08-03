<?php
// Geteilter Ruckler-Bericht: Annahme (POST), Anzeige (GET) und Zuruecknahme (DELETE).
//
// Bewusst NICHT hinter dem Deploy-Token (sync-endpoint.php): das ist ein oeffentlicher
// Schreib-Endpunkt und braucht eigene Grenzen - Groesse, Schema, Rate-Limit, Gesamtdeckel.
//
// Ablage: data/berichte/<id>.json (+ <id>.meta mit Anlagezeit und Loeschtoken-Hash).
// Die Rohdaten enthalten Prozess-/Treibernamen; deshalb liegt data/ hinter einer
// eigenen .htaccess und wird NIE direkt ausgeliefert, sondern nur ueber diese Datei.

declare(strict_types=1);

const MAX_BYTES     = 262144;      // 256 KB je Bericht (echte Berichte: 5-30 KB)
const MAX_PRO_IP    = 10;          // Uploads je Stunde und IP
const MAX_GESAMT_MB = 500;         // harter Deckel (Bytes); danach wird nichts mehr angenommen
const MAX_ANZAHL    = 50000;       // harter Deckel (Dateien) gegen Inode-Erschoepfung
const WARN_GESAMT_MB= 400;         // ab hier Warnung in der Uebersicht
const TTL_TAGE      = 365;         // ohne Abruf nach 12 Monaten weg
const ID_ZEICHEN    = '23456789abcdefghjkmnpqrstuvwxyz';   // ohne 0/1/i/l/o - vorlesbar

$dir = __DIR__ . '/data/berichte';
if (!is_dir($dir)) {
  @mkdir($dir, 0755, true);
  @file_put_contents(dirname($dir) . '/.htaccess', "Require all denied\n");
  @file_put_contents($dir . '/index.php', '');
}

function jout(array $o, int $code = 200): void {
  http_response_code($code);
  header('Content-Type: application/json; charset=utf-8');
  echo json_encode($o, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
  exit;
}
function id_ok(string $id): bool { return (bool) preg_match('/^[' . ID_ZEICHEN . ']{6}$/', $id); }
function pfad(string $dir, string $id): string { return $dir . '/' . $id . '.json'; }
function meta_pfad(string $dir, string $id): string { return $dir . '/' . $id . '.meta'; }

// Belegung in EINEM Durchlauf: Megabyte UND Anzahl. Der MB-Deckel allein reicht nicht -
// ein winziger gueltiger Bericht ist ~200 Bytes, unter 500 MB passen Millionen davon und
// erschoepfen die Inodes, lange bevor der MB-Deckel greift.
function belegung(string $dir): array {
  $s = 0; $n = 0;
  foreach ((array) @glob($dir . '/*.json') as $f) { $s += (int) @filesize($f); $n++; }
  return ['mb' => $s / 1048576, 'anzahl' => $n];
}

// Abgelaufene Berichte entfernen. Laeuft nur beim Hochladen (selten genug), kein Cronjob.
function aufraeumen(string $dir): void {
  $grenze = time() - TTL_TAGE * 86400;
  foreach ((array) @glob($dir . '/*.json') as $f) {
    $at = @fileatime($f); $mt = @filemtime($f);          // atime = letzter Abruf
    $letzte = max((int) $at, (int) $mt);
    if ($letzte && $letzte < $grenze) {
      @unlink($f);
      @unlink(substr($f, 0, -5) . '.meta');
    }
  }
}

// Rate-Limit je IP ueber eine kleine Zaehlerdatei (Muster wie download.php).
function rate_ok(string $dir, string $ip): bool {
  $f = $dir . '/.rate';
  $jetzt = time();
  $daten = [];
  $h = @fopen($f, 'c+');
  if (!$h) return true;                                   // Zaehler nicht schreibbar -> nicht blockieren
  @flock($h, LOCK_EX);
  $roh = stream_get_contents($h);
  if ($roh) { $d = json_decode($roh, true); if (is_array($d)) $daten = $d; }
  foreach ($daten as $k => $eintraege) {                  // aelter als 1 h faellt raus
    $daten[$k] = array_values(array_filter((array) $eintraege, fn($t) => $jetzt - (int) $t < 3600));
    if (!$daten[$k]) unset($daten[$k]);
  }
  $key = hash('sha256', $ip);                             // IP nicht im Klartext ablegen
  $eigen = $daten[$key] ?? [];
  $erlaubt = count($eigen) < MAX_PRO_IP;
  if ($erlaubt) { $eigen[] = $jetzt; $daten[$key] = $eigen; }
  ftruncate($h, 0); rewind($h); fwrite($h, json_encode($daten));
  @flock($h, LOCK_UN); fclose($h);
  return $erlaubt;
}

// Bericht plausibel? Nur Schema v1 mit den Feldern, die die Anzeige wirklich braucht.
function bericht_ok($j): bool {
  if (!is_array($j)) return false;
  if (($j['version'] ?? 0) !== 1) return false;
  foreach (['durS', 'avgFps', 'medianFtMs', 'spikes', 'verdictKey'] as $p) {
    if (!array_key_exists($p, $j)) return false;
  }
  if (!is_string($j['verdictKey']) || strlen($j['verdictKey']) > 32) return false;
  if (isset($j['ftSeries']) && (!is_array($j['ftSeries']) || count($j['ftSeries']) > 4000)) return false;
  if (isset($j['findings']) && (!is_array($j['findings']) || count($j['findings']) > 2000)) return false;
  return true;
}

// ---- Der zentrale Schutz: den Bericht aus einer WHITELIST neu aufbauen. ----------
// Dies ist ein oeffentlicher, anmeldungsfreier Schreib-Endpunkt: jeder kann per curl
// beliebiges JSON schicken, das anschliessend jedem Betrachter ausgeliefert wird.
// bericht_ok() prueft nur, ob die Pflichtfelder DA sind - nicht, was sonst noch drin
// steht. Wuerde das Rohobjekt gespeichert, landete jedes Zusatzfeld (context.resolution,
// aggregate[].kind, limit.top, ...) ungefiltert in innerHTML -> gespeichertes XSS auf
// der eigenen Domain. Darum wird hier NUR das uebernommen, was die Anzeige liest, mit
// festem Typ, gedeckelter Laenge und - bei Schluesseln, die als Label dienen - nur aus
// einer erlaubten Menge. Was hier herausfaellt, kann nachgelagert nichts mehr anrichten,
// egal welcher Renderer (Browser, Discord-Crawler, kuenftige Vorlage) es liest.
function s_txt($v, int $max): string {
  $s = is_scalar($v) ? (string) $v : '';
  // < > und " ganz entfernen: damit ist der gespeicherte String in JEDEM HTML-Kontext
  // inert - als Textknoten (kein Tag moeglich) UND in einem Attribut (kein Ausbruch
  // aus "..."). Kein legitimer Prozess-/GPU-/Aufloesungsname enthaelt diese Zeichen.
  // Unabhaengig davon escaped die Anzeige zusaetzlich; dies ist die Schicht, die die
  // DATEI selbst sauber haelt, egal welcher Renderer sie liest.
  $s = str_replace(["\0", '<', '>', '"'], '', $s);
  $s = preg_replace('/[\x00-\x1f\x7f]/u', '', $s) ?? '';  // uebrige Steuerzeichen
  return mb_substr(trim($s), 0, $max);
}
function s_flt($v): float { return is_numeric($v) ? (float) $v : 0.0; }
function s_int($v): int   { return is_numeric($v) ? (int) $v : 0; }
function s_bool($v): bool { return (bool) $v; }
function s_clamp(int $v, int $lo, int $hi): int { return max($lo, min($hi, $v)); }
function s_datei(string $p): string {                    // nur Dateiname, nie Pfad
  $p = str_replace('\\', '/', $p);
  $i = strrpos($p, '/');
  return $i === false ? $p : substr($p, $i + 1);
}

function bereinige(array $j): array {
  $VERDICTS = ['clean','driver','process','gpu-throttle','vram','disk','proc-start','game-internal'];
  $KINDS    = ['driver','process','gpu-throttle','vram','disk','proc-start','game-internal'];
  $LIMITS   = ['gpu','cpu','cpu-core','framecap','throttle','vram','unknown'];

  $vk = is_string($j['verdictKey'] ?? null) ? $j['verdictKey'] : '';
  $out = [
    'version'     => 1,
    'wall'        => s_txt($j['wall'] ?? '', 40),
    'durS'        => s_flt($j['durS'] ?? 0),
    'avgFps'      => s_flt($j['avgFps'] ?? 0),
    'medianFtMs'  => s_flt($j['medianFtMs'] ?? 0),
    'p1LowFps'    => s_flt($j['p1LowFps'] ?? 0),
    'p99FtMs'     => s_flt($j['p99FtMs'] ?? 0),
    'spikes'      => s_int($j['spikes'] ?? 0),
    'spikesPerMin'=> s_flt($j['spikesPerMin'] ?? 0),
    'verdictKey'  => in_array($vk, $VERDICTS, true) ? $vk : 'game-internal',
    'verdictName' => s_datei(s_txt($j['verdictName'] ?? '', 64)),   // kann ein Prozessname sein
    'verdictHits' => s_int($j['verdictHits'] ?? 0),
    'game'        => s_datei(s_txt($j['game'] ?? '', 128)),
    'gpu'         => s_txt($j['gpu'] ?? '', 64),
    'gpuDriver'   => s_txt($j['gpuDriver'] ?? '', 32),
  ];

  // Kontextzeile: Aufloesung nur im erwarteten Muster, sonst weg. game NIE (Pfad).
  $ctx = is_array($j['context'] ?? null) ? $j['context'] : [];
  $res = s_txt($ctx['resolution'] ?? '', 16);
  $out['context'] = [
    'resolution' => preg_match('/^\d{1,5}x\d{1,5}(@\d{1,3})?$/', $res) ? $res : '',
    'hdr'        => s_bool($ctx['hdr'] ?? false),
    'streaming'  => s_bool($ctx['streaming'] ?? false),
  ];

  // Kurve + Ruckler: rein numerisch, nur was der Canvas zeichnet. Laengen gedeckelt.
  $out['ftSeries'] = [];
  foreach (array_slice((array) ($j['ftSeries'] ?? []), 0, 4000) as $p) {
    if (is_array($p) && count($p) >= 2) $out['ftSeries'][] = [s_flt($p[0]), s_flt($p[1])];
  }
  $out['findings'] = [];
  foreach (array_slice((array) ($j['findings'] ?? []), 0, 2000) as $fd) {
    if (is_array($fd)) $out['findings'][] = ['t' => s_flt($fd['t'] ?? 0), 'ftMs' => s_flt($fd['ftMs'] ?? 0)];
  }

  // Verdaechtigen-Balken: kind nur aus der erlaubten Menge (sonst Zeile raus),
  // name gedeckelt, hits als Zahl. name kann ein Prozessname sein -> Dateiname-Schutz.
  $out['aggregate'] = [];
  foreach (array_slice((array) ($j['aggregate'] ?? []), 0, 24) as $a) {
    if (!is_array($a)) continue;
    $kind = is_string($a['kind'] ?? null) ? $a['kind'] : '';
    if (!in_array($kind, $KINDS, true)) continue;
    $out['aggregate'][] = [
      'kind' => $kind,
      'name' => s_datei(s_txt($a['name'] ?? '', 64)),
      'hits' => s_clamp(s_int($a['hits'] ?? 0), 0, 100000),
    ];
  }

  // Flaschenhals: alle Werte numerisch, top nur aus der erlaubten Menge.
  if (is_array($j['limit'] ?? null)) {
    $L = $j['limit'];
    $lim = ['samples' => s_int($L['samples'] ?? 0), 'topPct' => s_clamp(s_int($L['topPct'] ?? 0), 0, 100)];
    foreach ($LIMITS as $k) $lim[$k] = s_clamp(s_int($L[$k] ?? 0), 0, 100);
    $top = is_string($L['top'] ?? null) ? $L['top'] : '';
    $lim['top'] = in_array($top, $LIMITS, true) ? $top : 'unknown';
    $out['limit'] = $lim;
  }

  return $out;
}

$a  = $_GET['a']  ?? '';
$id = strtolower(trim((string) ($_GET['id'] ?? '')));
$ip = (string) ($_SERVER['REMOTE_ADDR'] ?? '');

// ---------- Hochladen ----------
if ($_SERVER['REQUEST_METHOD'] === 'POST' && $a === '') {
  if (!rate_ok($dir, $ip)) jout(['ok' => false, 'error' => 'zu-viele'], 429);
  aufraeumen($dir);
  $bel = belegung($dir);
  if ($bel['mb'] >= MAX_GESAMT_MB || $bel['anzahl'] >= MAX_ANZAHL) {
    jout(['ok' => false, 'error' => 'server-voll'], 507);
  }

  $roh = file_get_contents('php://input', false, null, 0, MAX_BYTES + 1);
  if ($roh === false || $roh === '') jout(['ok' => false, 'error' => 'leer'], 400);
  if (strlen($roh) > MAX_BYTES)      jout(['ok' => false, 'error' => 'zu-gross'], 413);

  $j = json_decode($roh, true);
  if (!bericht_ok($j)) jout(['ok' => false, 'error' => 'kein-lumora-bericht'], 400);

  // Aus einer Whitelist neu aufbauen: nur bekannte Felder, feste Typen, gedeckelte
  // Laengen, Label-Schluessel nur aus erlaubten Mengen. Pfade und die private Notiz
  // fallen dabei von selbst weg, weil sie schlicht nicht uebernommen werden.
  $j = bereinige($j);

  // Freie ID suchen
  $id = '';
  for ($v = 0; $v < 40 && $id === ''; $v++) {
    $k = '';
    for ($i = 0; $i < 6; $i++) $k .= ID_ZEICHEN[random_int(0, strlen(ID_ZEICHEN) - 1)];
    if (!is_file(pfad($dir, $k))) $id = $k;
  }
  if ($id === '') jout(['ok' => false, 'error' => 'keine-id'], 503);

  $token = bin2hex(random_bytes(16));
  // Wieder einkodieren statt Rohtext ablegen: was hier landet, ist garantiert das,
  // was der Prueflauf oben gesehen hat (kein Schmuggeln ausserhalb des Schemas).
  if (@file_put_contents(pfad($dir, $id), json_encode($j, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE), LOCK_EX) === false) {
    jout(['ok' => false, 'error' => 'schreibfehler'], 500);
  }
  @file_put_contents(meta_pfad($dir, $id), json_encode([
    'erstellt' => time(), 'token' => hash('sha256', $token),
  ]), LOCK_EX);

  // Kurzer Link /b/<id> braucht eine Rewrite-Regel in .htaccess. Die kann das
  // Deploy-Werkzeug nicht hochladen (es verweigert .htaccess absichtlich, um den
  // Schaden eines geleakten Tokens zu begrenzen) - sie muss von Hand dazu. Also
  // hier nachsehen, ob sie schon da ist, statt sie anzunehmen: sonst gaebe der
  // Server eine Adresse heraus, die 404 liefert. Sobald die Regel hochgeladen
  // ist, schalten die Links von selbst auf die kurze Form um.
  $ht = __DIR__ . '/.htaccess';
  $kurz = is_readable($ht) && strpos((string) @file_get_contents($ht), '^b/(') !== false;
  $url = 'https://lumora-streaming.de/' . ($kurz ? 'b/' . $id : 'bericht.php?id=' . $id);
  jout(['ok' => true, 'id' => $id, 'url' => $url, 'token' => $token]);
}

// ---------- Zuruecknehmen ----------
if (($_SERVER['REQUEST_METHOD'] === 'DELETE' || $a === 'del') && id_ok($id)) {
  $token = (string) ($_GET['token'] ?? '');
  $meta  = json_decode((string) @file_get_contents(meta_pfad($dir, $id)), true);
  if (!is_array($meta) || !hash_equals((string) ($meta['token'] ?? ''), hash('sha256', $token))) {
    jout(['ok' => false, 'error' => 'token-falsch'], 403);
  }
  @unlink(pfad($dir, $id));
  @unlink(meta_pfad($dir, $id));
  jout(['ok' => true]);
}

// ---------- Anzeigen ----------
if (!id_ok($id) || !is_file(pfad($dir, $id))) {
  http_response_code(404);
  $r = null;
} else {
  $roh = (string) @file_get_contents(pfad($dir, $id));
  $r = json_decode($roh, true);
  @touch(pfad($dir, $id));                                 // Abruf = am Leben halten
}

// Sprache: ?lang=en erzwingt Englisch, sonst entscheidet der Browser. Das MUSS hier
// serverseitig passieren - Titel und og:* werden von Discord/WhatsApp/Suchmaschinen
// gelesen, die kein JavaScript ausfuehren. Die Seite selbst uebersetzt bericht.js
// spaeter im Browser; wuerde der Kopf immer deutsch bleiben, saehe ein geteilter Link
// in einem englischen Forum als deutsche Vorschaukarte aus.
$lang = 'de';
if (isset($_GET['lang'])) {
  $lang = $_GET['lang'] === 'en' ? 'en' : 'de';
} elseif (!preg_match('/\bde\b/i', $_SERVER['HTTP_ACCEPT_LANGUAGE'] ?? '')
       &&  preg_match('/\ben\b/i', $_SERVER['HTTP_ACCEPT_LANGUAGE'] ?? '')) {
  $lang = 'en';
}
$en = $lang === 'en';

// Vorschautext fuer Chat-Programme aus den Kennzahlen bauen (og:description).
// Ohne Bericht (zurueckgezogen oder Tippfehler in der ID) muss der Titel das auch
// sagen - sonst steht im Reiter und in jeder Chat-Vorschau "Ruckler-Analyse", als
// gaebe es die Seite noch.
if ($r === null) {
  $ogTitel = $en ? 'Report not found | Lumora' : 'Bericht nicht gefunden | Lumora';
  $ogText  = $en ? 'This report is no longer available.'
                 : 'Dieser Bericht ist nicht mehr verfügbar.';
} else {
  $ogTitel = $en ? 'Stutter analysis | Lumora' : 'Ruckler-Analyse | Lumora';
  $ogText  = $en ? 'A stutter report measured with Lumora.'
                 : 'Ein mit Lumora gemessener Ruckler-Bericht.';
}
if ($r) {
  $spikes = (int) ($r['spikes'] ?? 0);
  $fps    = (int) round((float) ($r['avgFps'] ?? 0));
  $min    = round(((float) ($r['durS'] ?? 0)) / 60, 1);
  $name   = (string) ($r['verdictName'] ?? '');
  $key    = (string) ($r['verdictKey'] ?? '');
  $urteil = $en ? match ($key) {
    'clean'        => 'Clean run – no stutters above the threshold',
    'driver'       => 'Prime suspect: driver ' . $name,
    'process'      => 'Prime suspect: process ' . $name,
    'gpu-throttle' => 'The graphics card is throttling',
    'vram'         => 'Video memory ran full',
    'disk'         => 'Disk load',
    'proc-start'   => 'Background program starts',
    default        => 'No system cause found – likely game-internal',
  } : match ($key) {
    'clean'        => 'Sauberer Lauf – keine Ruckler über der Schwelle',
    'driver'       => 'Hauptverdächtiger: Treiber ' . $name,
    'process'      => 'Hauptverdächtiger: Programm ' . $name,
    'gpu-throttle' => 'Die Grafikkarte drosselt',
    'vram'         => 'Grafikspeicher lief voll',
    'disk'         => 'Datenträger-Last',
    'proc-start'   => 'Programmstarts im Hintergrund',
    default        => 'Keine Systemursache gefunden – vermutlich spielintern',
  };
  $ogTitel = $urteil . ' | Lumora';
  $spiel   = ($r['game'] ?? '') ? ' · ' . $r['game'] : '';
  $ogText  = $en
    ? $spikes . ' stutters in ' . $min . ' min · avg ' . $fps . ' fps' . $spiel . ' – measured with Lumora.'
    : $spikes . ' Ruckler in ' . $min . ' min · ø ' . $fps . ' fps' . $spiel . ' – gemessen mit Lumora.';
}
$h = fn($s) => htmlspecialchars((string) $s, ENT_QUOTES, 'UTF-8');

// Letzte Schutzschicht: selbst WENN doch einmal Fremdtext in die Seite geraet, darf er
// kein Skript ausfuehren. script-src 'self' verbietet sowohl <script>-Einschleusung als
// auch Inline-Handler (onerror=...). Die Seite braucht nur zwei eigene Skripte und ein
// eigenes Stylesheet; Inline-STYLES (die Balkenbreiten) bleiben erlaubt - CSS kann kein
// Skript starten, und ihre Werte sind ohnehin schon serverseitig auf Zahlen reduziert.
// frame-ancestors 'none' unterbindet Einbettung (Clickjacking).
header("Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
     . "img-src 'self' data:; font-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
header('X-Content-Type-Options: nosniff');
header('X-Frame-Options: DENY');   // aeltere Browser ohne frame-ancestors
// Referrer-Policy kommt bereits aus der globalen .htaccess (strict-origin-when-cross-origin);
// der geteilte Link traegt ohnehin nur die oeffentliche ID, kein Geheimnis.
?><!DOCTYPE html>
<html lang="<?= $lang ?>">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title><?= $h($ogTitel) ?></title>
<!-- Einzelne Berichte gehoeren nicht in den Suchindex: fremde Messdaten verwaessern
     die Domain. Die Vorschaukarte fuer Chat-Programme bleibt davon unberuehrt. -->
<meta name="robots" content="noindex, follow">
<meta property="og:type" content="article">
<meta property="og:site_name" content="Lumora">
<meta property="og:locale" content="<?= $en ? 'en_US' : 'de_DE' ?>">
<meta property="og:title" content="<?= $h($ogTitel) ?>">
<meta property="og:description" content="<?= $h($ogText) ?>">
<meta property="og:image" content="https://lumora-streaming.de/bericht-og.png">
<meta property="og:image:width" content="1200">
<meta property="og:image:height" content="630">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="<?= $h($ogTitel) ?>">
<meta name="twitter:description" content="<?= $h($ogText) ?>">
<meta name="twitter:image" content="https://lumora-streaming.de/bericht-og.png">
<link rel="icon" href="/favicon-v2.png">
<link rel="stylesheet" href="/bericht.css">
</head>
<body>
<?php if (!$r): ?>
  <main class="leer">
    <div class="leer-icon">🔍</div>
    <h1>Dieser Bericht ist nicht (mehr) da</h1>
    <p>Der Link ist ungültig, oder der Bericht wurde von der Person zurückgezogen, die ihn geteilt hat.</p>
    <a class="cta" href="https://lumora-streaming.de/ruckler-analyse.html">Was ist die Lumora Ruckler-Analyse?</a>
  </main>
<?php else: ?>
  <main id="app" data-id="<?= $h($id) ?>">
    <div class="lade">Bericht wird aufgebaut …</div>
  </main>
  <script id="berichtsdaten" type="application/json"><?= json_encode($r, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_HEX_TAG | JSON_HEX_AMP) ?></script>
  <script src="/analyze-report.js"></script>
  <script src="/bericht.js"></script>
<?php endif; ?>
</body>
</html>
