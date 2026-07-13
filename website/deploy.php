<?php
// ===========================================================================
// deploy.php  -  abgesicherter Deployment-Endpunkt fuer Lumora
// ---------------------------------------------------------------------------
// Nimmt per HTTPS token-authentifizierte, gechunkte Datei-Uploads entgegen und
// schreibt AUSSCHLIESSLICH innerhalb des eigenen Verzeichnisbaums (__DIR__).
//
// Sicherheits-Prinzipien (defense in depth):
//   - Nur HTTPS (nicht-faelschbar), nur POST.
//   - Token-Auth per hash_equals (konstante Zeit). Geheimnis liegt separat in
//     deploy.secret.php (gitignored, NIE im Repo).
//   - Ziel-Pfad: strikte Whitelist + ".."-Verbot je Segment + realpath-Schranke
//     gegen __DIR__  ->  kein Ausbruch, kein Path-Traversal, kein Symlink-Escape.
//   - Optionale Pfad-Allowlist ('allow') begrenzt den Blast-Radius eines Token-
//     Leaks: z.B. den Auto-Update-Ordner updates/ bewusst NICHT freigeben.
//   - Selbstschutz: deploy.php / Geheimnis / .htaccess sind nie ueberschreibbar.
//   - Kein include/eval/exec von Upload-Daten. Reines Datei-Schreiben.
//   - Chunked (.part -> atomares rename) mit flock, .part-Cleanup, Read-Timeout,
//     Groessen-Limit, sha256-Rueckgabe.
//
// WICHTIG (Betrieb, siehe Security-Audit): Der Token ist ein RCE-Credential
// (legitime Deploys enthalten .php). Streng geheim halten, per .htaccess auf
// vertrauenswuerdige IP(s) beschraenken, bei Verdacht sofort rotieren. Den
// Auto-Update-Ordner updates/ NICHT ueber diesen Token freigeben, solange die
// Releases unsigniert sind (sonst wird ein Token-Leak zur Supply-Chain-Gefahr).
// ===========================================================================
declare(strict_types=1);
header('X-Content-Type-Options: nosniff');
header('Content-Type: application/json');

function bail(int $code, string $msg): void {
  http_response_code($code);
  echo json_encode(['ok' => false, 'error' => $msg]);
  exit;
}

// --- 0) Nur HTTPS + POST ---------------------------------------------------
// HTTPS-Nachweis NUR aus nicht-faelschbaren Quellen. X-Forwarded-Proto ist ein
// Client-Header und wird bewusst NICHT vertraut. Terminiert dein Hoster TLS
// extern und meldet intern nur Port 80, hier die vom Hoster dokumentierte,
// vertrauenswuerdige Variable ergaenzen - niemals blanko einen Client-Header.
$https = (!empty($_SERVER['HTTPS']) && strtolower((string)$_SERVER['HTTPS']) !== 'off')
      || ((string)($_SERVER['SERVER_PORT'] ?? '') === '443');
if (!$https) bail(403, 'https required');
if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') bail(405, 'post only');

// --- 1) Geheimnis + optionale Allowlist laden (separat, gitignored) ---------
$secretFile = __DIR__ . '/deploy.secret.php';
if (!is_file($secretFile)) bail(503, 'disabled');
$cfg = require $secretFile;
// deploy.secret.php gibt entweder das Token (String) ODER ein Array zurueck:
//   ['token' => '<hex>', 'allow' => ['index.html','en/','...']]
// 'allow' (optional): nur Pfade, die mit einem dieser Praefixe beginnen, sind
// beschreibbar. Leer/fehlt = gesamter eigener Baum erlaubt.
$SECRET = is_array($cfg) ? (string)($cfg['token'] ?? '') : (string)$cfg;
$ALLOW  = (is_array($cfg) && isset($cfg['allow']) && is_array($cfg['allow'])) ? $cfg['allow'] : [];
$DENY   = (is_array($cfg) && isset($cfg['deny'])  && is_array($cfg['deny']))  ? $cfg['deny']  : [];
if (strlen($SECRET) < 32) bail(503, 'misconfigured');

// --- 2) Token pruefen (konstante Zeit) -------------------------------------
$token = (string)($_SERVER['HTTP_X_DEPLOY_AUTH'] ?? '');
if ($token === '' || !hash_equals($SECRET, $token)) bail(401, 'unauthorized');

// --- 3) Ziel-Pfad streng validieren ----------------------------------------
$rel = (string)($_SERVER['HTTP_X_DEPLOY_PATH'] ?? '');
if ($rel === '' || strlen($rel) > 255) bail(400, 'bad path');
if (strpos($rel, "\0") !== false) bail(400, 'bad path');
if (strpos($rel, '\\') !== false) bail(400, 'bad path');
if ($rel[0] === '/') bail(400, 'bad path');
if (!preg_match('#^[A-Za-z0-9._/-]+$#', $rel)) bail(400, 'bad path');
foreach (explode('/', $rel) as $seg) {
  if ($seg === '' || $seg === '.' || $seg === '..') bail(400, 'bad path');
}
// Selbstschutz: Endpunkt, Geheimnis und Server-Config nie ueberschreibbar
$protected = ['deploy.php', 'deploy.secret.php', '.htaccess', '.htpasswd'];
if (in_array(strtolower(basename($rel)), $protected, true)) bail(403, 'protected');

// Pfad-Policy (Blast-Radius-Begrenzung eines Token-Leaks). 'deny' hat Vorrang:
// z.B. deny ['updates/'] haelt den unsignierten Auto-Update-Feed ausserhalb der
// Reichweite dieses Tokens (schliesst den Supply-Chain-Vektor).
foreach ($DENY as $p) {
  if (is_string($p) && $p !== '' && strncmp($rel, $p, strlen($p)) === 0) bail(403, 'path not allowed');
}
if ($ALLOW) {
  $okPrefix = false;
  foreach ($ALLOW as $p) {
    if (is_string($p) && $p !== '' && strncmp($rel, $p, strlen($p)) === 0) { $okPrefix = true; break; }
  }
  if (!$okPrefix) bail(403, 'path not allowed');
}

$base   = __DIR__;
$target = $base . '/' . $rel;
$dir    = dirname($target);

// --- 4) Zielverzeichnis anlegen (nur innerhalb base) -----------------------
if (!is_dir($dir)) {
  if (!@mkdir($dir, 0755, true) && !is_dir($dir)) bail(500, 'mkdir failed');
}
// realpath-Schranke: aufgeloester Ziel-Ordner MUSS unter dem realpath von base
// liegen (faengt Symlinks und jede Form von Ausbruch ab).
$realBase = realpath($base);
$realDir  = realpath($dir);
if ($realBase === false || $realDir === false) bail(400, 'bad path');
$prefix = rtrim($realBase, '/') . '/';
if ($realDir !== rtrim($realBase, '/') && strncmp($realDir . '/', $prefix, strlen($prefix)) !== 0) {
  bail(400, 'bad path');
}

// --- 5) Chunk-Parameter ----------------------------------------------------
$MAX    = 600 * 1024 * 1024;   // harte Obergrenze pro Datei: 600 MB
$offset = (int)($_SERVER['HTTP_X_DEPLOY_OFFSET'] ?? '0');
$final  = ((string)($_SERVER['HTTP_X_DEPLOY_FINAL'] ?? '')) === '1';
if ($offset < 0 || $offset > $MAX) bail(400, 'bad offset');

$part = $target . '.part';

// Verwaiste .part-Reste im Zielordner aufraeumen (abgebrochene Uploads > 2 h),
// damit sie nicht Disk/Inodes fuellen. Die aktuelle .part ist juenger -> bleibt.
foreach ((glob($dir . '/*.part') ?: []) as $stale) {
  if (is_file($stale) && (time() - (int)filemtime($stale)) > 7200) @unlink($stale);
}

// --- 6) Datei oeffnen + exklusiv sperren, DANN Groesse pruefen + schreiben --
// flock serialisiert parallele Chunks auf denselben Pfad (kein Verzahnen).
$out = fopen($part, $offset === 0 ? 'wb' : 'cb');
if (!$out) bail(500, 'open failed');
if (!flock($out, LOCK_EX)) { fclose($out); bail(500, 'lock failed'); }
clearstatcache(true, $part);
$st = fstat($out);
$curSize = $st ? (int)$st['size'] : 0;
if ($curSize !== $offset) { flock($out, LOCK_UN); fclose($out); bail(409, 'offset mismatch'); }
if ($offset > 0) fseek($out, 0, SEEK_END);

$in = fopen('php://input', 'rb');
if (!$in) { flock($out, LOCK_UN); fclose($out); bail(500, 'open failed'); }
stream_set_timeout($in, 30);   // Slow-POST-Schutz
$written = 0;
while (!feof($in)) {
  $buf = fread($in, 262144);   // 256 KB
  $meta = stream_get_meta_data($in);
  if (!empty($meta['timed_out'])) { fclose($in); flock($out, LOCK_UN); fclose($out); bail(408, 'read timeout'); }
  if ($buf === false) break;
  if ($buf === '') continue;
  if (fwrite($out, $buf) === false) { fclose($in); flock($out, LOCK_UN); fclose($out); @unlink($part); bail(500, 'write failed'); }
  $written += strlen($buf);
  if ($offset + $written > $MAX) { fclose($in); flock($out, LOCK_UN); fclose($out); @unlink($part); bail(413, 'too large'); }
}
fclose($in);

// --- 7) Abschluss: .part -> Ziel (atomar) + sha256 zurueck -----------------
if ($final) {
  flock($out, LOCK_UN);
  fclose($out);
  if (!@rename($part, $target)) { @unlink($part); bail(500, 'finalize failed'); }
  @chmod($target, 0644);
  echo json_encode(['ok' => true, 'path' => $rel, 'size' => filesize($target), 'sha256' => hash_file('sha256', $target)]);
} else {
  flock($out, LOCK_UN);
  fclose($out);
  echo json_encode(['ok' => true, 'received' => $written, 'next' => $offset + $written]);
}
