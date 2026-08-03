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
const MAX_GESAMT_MB = 500;         // harter Deckel; danach wird nichts mehr angenommen
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

function belegt_mb(string $dir): float {
  $s = 0;
  foreach ((array) @glob($dir . '/*.json') as $f) { $s += (int) @filesize($f); }
  return $s / 1048576;
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

$a  = $_GET['a']  ?? '';
$id = strtolower(trim((string) ($_GET['id'] ?? '')));
$ip = (string) ($_SERVER['REMOTE_ADDR'] ?? '');

// ---------- Hochladen ----------
if ($_SERVER['REQUEST_METHOD'] === 'POST' && $a === '') {
  if (!rate_ok($dir, $ip)) jout(['ok' => false, 'error' => 'zu-viele'], 429);
  aufraeumen($dir);
  if (belegt_mb($dir) >= MAX_GESAMT_MB) jout(['ok' => false, 'error' => 'server-voll'], 507);

  $roh = file_get_contents('php://input', false, null, 0, MAX_BYTES + 1);
  if ($roh === false || $roh === '') jout(['ok' => false, 'error' => 'leer'], 400);
  if (strlen($roh) > MAX_BYTES)      jout(['ok' => false, 'error' => 'zu-gross'], 413);

  $j = json_decode($roh, true);
  if (!bericht_ok($j)) jout(['ok' => false, 'error' => 'kein-lumora-bericht'], 400);

  // Pfade serverseitig entfernen - VERTEIDIGUNG IN DER TIEFE. Die App reinigt bereits
  // vor dem Hochladen, aber hier landet alles, was jemand schickt: ein direkter POST
  // (oder eine aeltere App-Version) koennte den vollen Pfad mitbringen, und darin steckt
  // regelmaessig der Windows-Benutzername. Beim Live-Test genau so passiert: der Pfad
  // stand in der og:description, also in der Vorschaukarte von Discord/WhatsApp.
  $nurDatei = static function ($p) {
    $p = str_replace('\\', '/', (string) $p);
    $i = strrpos($p, '/');
    return $i === false ? $p : substr($p, $i + 1);
  };
  if (isset($j['game'])) $j['game'] = $nurDatei($j['game']);
  if (isset($j['context']) && is_array($j['context'])) {
    unset($j['context']['game']);                 // dort steht der Pfad ein zweites Mal
  }
  unset($j['note']);                              // eigene Notizen sind privat

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
