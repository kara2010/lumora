<?php
// Lumora Gruppen-Vermittlung ("Rendezvous"): EINE Datei, kein Datenbank-Zwang.
// Verwaltet Raumcodes und die aktuellen Adressen der Mitglieder und liefert den
// Grid-Player aus. Der VIDEO-Traffic laeuft NIE hierueber - die Browser bauen
// direkte WebRTC-Verbindungen zu den Streamern auf; dieses Skript reicht nur die
// Verbindungs-Handshakes (WHEP: kleine SDP-Textnachrichten) durch, weil eine
// HTTPS-Seite nicht selbst per fetch() mit http://-Adressen reden darf (Mixed
// Content) - PHP darf das serverseitig.
//
// Selbst hosten: diese Datei auf einen beliebigen PHP-Webspace legen (PHP 7+,
// Schreibrechte im eigenen Ordner). In Lumora unter Stream -> Gruppe die URL
// eintragen - fertig. Es entsteht ein Unterordner "gruppen/" fuer die Raumdaten.

error_reporting(0);
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');
if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') { http_response_code(204); exit; }

define('TTL_S', 45);              // Mitglied gilt nach so langer Funkstille als weg
define('MAX_MEMBERS', 16);
define('CODE_CHARS', 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789');   // ohne 0/O/1/I - diktierbar

$dir = __DIR__ . '/gruppen';
if (!is_dir($dir)) {
  @mkdir($dir, 0755, true);
  @file_put_contents($dir . '/.htaccess', "Require all denied\n");   // Rohdaten (IPs) nicht direkt abrufbar
  @file_put_contents($dir . '/index.php', '');
}

function jout($o) { header('Content-Type: application/json; charset=utf-8'); echo json_encode($o); exit; }
function code_ok($c) { return is_string($c) && preg_match('/^[A-Z2-9]{6}$/', $c); }
function room_path($c) { global $dir; return $dir . '/' . $c . '.json'; }

// Raum lesen + abgelaufene Mitglieder ausmisten. flock schuetzt vor parallelen Schreibern.
function room_load($c) {
  $p = room_path($c);
  if (!is_file($p)) return null;
  $j = json_decode(@file_get_contents($p), true);
  if (!is_array($j) || !isset($j['members'])) return null;
  $now = time();
  foreach ($j['members'] as $id => $m) {
    if (!isset($m['lastSeen']) || $now - $m['lastSeen'] > TTL_S) unset($j['members'][$id]);
  }
  return $j;
}
function room_save($c, $j) {
  if (count($j['members']) === 0) { @unlink(room_path($c)); return; }   // leerer Raum -> weg
  @file_put_contents(room_path($c), json_encode($j), LOCK_EX);
}
function members_out($j) {
  $out = array();
  foreach ($j['members'] as $m) { unset($m['vk']); $out[] = $m; }   // Tuersteher-Schluessel NIE nach aussen geben
  usort($out, function ($a, $b) { return ($a['joinedAt'] ?? 0) <=> ($b['joinedAt'] ?? 0); });
  return $out;
}
function read_body() { return file_get_contents('php://input', false, null, 0, 262144); }

// HTTP-Request an ein Mitglied (serverseitig; nur Signalisierung, kurze Timeouts).
function relay($url, $method, $body, $contentType) {
  $ch = curl_init($url);
  curl_setopt_array($ch, array(
    CURLOPT_CUSTOMREQUEST => $method,
    CURLOPT_RETURNTRANSFER => true,
    CURLOPT_HEADER => true,
    // 2 s statt 4 s: ein toter Adress-Kandidat (DS-Lite: IPv4 gesetzt, aber von
    // aussen zu) soll den Verbindungsaufbau nicht laenger als noetig aufhalten.
    CURLOPT_CONNECTTIMEOUT => 2,
    CURLOPT_TIMEOUT => 8,
    CURLOPT_FOLLOWLOCATION => false,
  ));
  if ($body !== null) {
    curl_setopt($ch, CURLOPT_POSTFIELDS, $body);
    curl_setopt($ch, CURLOPT_HTTPHEADER, array('Content-Type: ' . $contentType));
  }
  $resp = curl_exec($ch);
  if ($resp === false) { curl_close($ch); return null; }
  $hlen = curl_getinfo($ch, CURLINFO_HEADER_SIZE);
  $status = curl_getinfo($ch, CURLINFO_RESPONSE_CODE);
  curl_close($ch);
  $headers = substr($resp, 0, $hlen);
  $loc = null;
  if (preg_match('/^location:\s*(\S+)/mi', $headers, $mm)) $loc = trim($mm[1]);
  return array('status' => $status, 'body' => substr($resp, $hlen), 'location' => $loc);
}
// Adress-Kandidaten eines Mitglieds - NUR Adressen aus dem Roster, dieses Skript
// ist bewusst KEIN offener Proxy. Die zuletzt FUNKTIONIERENDE Adresse (goodAddr,
// vom cfg-Parallel-Check gelernt) kommt zuerst: der WHEP-Handshake zahlt dann im
// Normalfall nie den Connect-Timeout eines toten Kandidaten (DS-Lite-Falle).
function member_addrs($m) {
  $a = array();
  if (!empty($m['linkV4'])) $a[] = rtrim($m['linkV4'], '/');
  if (!empty($m['linkV6'])) $a[] = rtrim($m['linkV6'], '/');
  if (!empty($m['goodAddr']) && in_array($m['goodAddr'], $a, true)) {
    array_splice($a, array_search($m['goodAddr'], $a, true), 1);
    array_unshift($a, $m['goodAddr']);
  }
  return $a;
}
// Mehrere Adress-Kandidaten PARALLEL anfragen (curl_multi), der erste Erfolg
// gewinnt - statt sequenziell je Kandidat bis zum Connect-Timeout zu warten.
// NUR fuer nebenwirkungsfreie Anfragen (cfg = GET) - ein paralleler WHEP-POST
// wuerde beim Streamer verwaiste Sessions anlegen. $pairs: array([url, base]).
// Rueckgabe: array(status, body, base) des Gewinners oder null.
function relay_multi($pairs, $okStatus) {
  if (!$pairs) return null;
  $mh = curl_multi_init();
  $map = array();
  foreach ($pairs as $p) {
    $ch = curl_init($p[0]);
    curl_setopt_array($ch, array(
      CURLOPT_RETURNTRANSFER => true,
      CURLOPT_CONNECTTIMEOUT => 2,
      CURLOPT_TIMEOUT => 6,
      CURLOPT_FOLLOWLOCATION => false,
    ));
    curl_multi_add_handle($mh, $ch);
    $map[(int)$ch] = array($ch, $p[1]);
  }
  $best = null;
  do {
    $mrc = curl_multi_exec($mh, $running);
    if ($running) curl_multi_select($mh, 0.2);
    while ($info = curl_multi_info_read($mh)) {
      $ch = $info['handle'];
      $entry = isset($map[(int)$ch]) ? $map[(int)$ch] : null;
      $body = curl_multi_getcontent($ch);
      $code = curl_getinfo($ch, CURLINFO_RESPONSE_CODE);
      curl_multi_remove_handle($mh, $ch);
      curl_close($ch);
      unset($map[(int)$ch]);
      if ($entry && $body !== false && $code === $okStatus) {
        $best = array('status' => $code, 'body' => $body, 'base' => $entry[1]);
        $running = 0;
        break;
      }
    }
  } while ($running && $mrc === CURLM_OK);
  foreach ($map as $entry) { @curl_multi_remove_handle($mh, $entry[0]); @curl_close($entry[0]); }
  curl_multi_close($mh);
  return $best;
}

$a = isset($_GET['a']) ? $_GET['a'] : '';
$code = isset($_GET['code']) ? strtoupper(trim($_GET['code'])) : '';

// ---- API ---------------------------------------------------------------------
if ($a === 'create') {
  // Wunsch-Code: der Client (5-Min-Karenz beim kurzen Stream-Neustart) will seinen
  // zuletzt genutzten Code wiederbekommen. NUR wenn er gerade frei ist -> KEIN
  // dauerhafter Anspruch, keine Reservierung. Ist er belegt/abgelaufen -> Zufallscode.
  $want = isset($_GET['want']) ? strtoupper(trim($_GET['want'])) : '';
  if (code_ok($want) && !is_file(room_path($want))) {
    room_save($want, array('code' => $want, 'created' => time(), 'members' => array('_' => array('id' => '_', 'lastSeen' => time(), 'joinedAt' => time()))));
    jout(array('ok' => true, 'code' => $want));
  }
  for ($i = 0; $i < 50; $i++) {
    $c = '';
    for ($k = 0; $k < 6; $k++) $c .= CODE_CHARS[random_int(0, strlen(CODE_CHARS) - 1)];
    if (!is_file(room_path($c))) {
      room_save($c, array('code' => $c, 'created' => time(), 'members' => array('_' => array('id' => '_', 'lastSeen' => time(), 'joinedAt' => time()))));
      // Platzhalter-Mitglied haelt den Raum bis zum ersten echten update am Leben.
      jout(array('ok' => true, 'code' => $c));
    }
  }
  jout(array('ok' => false, 'error' => 'no-code'));
}
if ($a === 'update') {
  if (!code_ok($code)) jout(array('ok' => false, 'error' => 'bad-code'));
  $m = json_decode(read_body(), true);
  if (!is_array($m) || empty($m['id'])) jout(array('ok' => false, 'error' => 'bad-member'));
  $j = room_load($code);
  if ($j === null) jout(array('ok' => false, 'error' => 'no-room'));
  unset($j['members']['_']);
  $id = substr(preg_replace('/[^a-zA-Z0-9]/', '', $m['id']), 0, 32);
  if (!isset($j['members'][$id]) && count($j['members']) >= MAX_MEMBERS) jout(array('ok' => false, 'error' => 'room-full'));
  $prev = isset($j['members'][$id]) ? $j['members'][$id] : null;
  $j['members'][$id] = array(
    'id' => $id,
    'name' => mb_substr((string)($m['name'] ?? 'Spieler'), 0, 24),
    'linkV4' => isset($m['linkV4']) && $m['linkV4'] ? (string)$m['linkV4'] : null,
    'linkV6' => isset($m['linkV6']) && $m['linkV6'] ? (string)$m['linkV6'] : null,
    'streaming' => !isset($m['streaming']) || $m['streaming'] !== false,
    'joinedAt' => $prev ? $prev['joinedAt'] : time(),
    'lastSeen' => time(),
    // gelernten "funktionierenden Weg" ueber Heartbeats hinweg behalten
    // (member_addrs validiert ihn ohnehin gegen die aktuellen Links).
    'goodAddr' => ($prev && !empty($prev['goodAddr'])) ? $prev['goodAddr'] : null,
    // Tuersteher-Schluessel des Streamers: server-seitig behalten, aber NIE oeffentlich
    // ausgeben (members_out strippt ihn). Der Client sendet ihn bei jedem Heartbeat mit.
    'vk' => (isset($m['vk']) && $m['vk']) ? substr(preg_replace('/[^a-zA-Z0-9]/', '', (string)$m['vk']), 0, 64) : ($prev['vk'] ?? null),
  );
  room_save($code, $j);
  jout(array('ok' => true, 'members' => members_out($j)));
}
if ($a === 'list') {
  if (!code_ok($code)) jout(array('ok' => false, 'error' => 'bad-code'));
  $j = room_load($code);
  if ($j === null) jout(array('ok' => false, 'error' => 'no-room'));
  $vis = $j; unset($vis['members']['_']);
  jout(array('ok' => true, 'members' => members_out($vis)));
}
if ($a === 'leave') {
  if (!code_ok($code)) jout(array('ok' => false, 'error' => 'bad-code'));
  $b = json_decode(read_body(), true);
  $j = room_load($code);
  if ($j !== null && is_array($b) && !empty($b['id'])) {
    unset($j['members'][substr(preg_replace('/[^a-zA-Z0-9]/', '', $b['id']), 0, 32)]);
    room_save($code, $j);
  }
  jout(array('ok' => true));
}
// WHEP-Handshake zum Streamer durchreichen (SDP-Offer hin, SDP-Answer zurueck).
if ($a === 'whep') {
  if (!code_ok($code)) jout(array('ok' => false, 'error' => 'bad-code'));
  $mid = isset($_GET['id']) ? $_GET['id'] : '';
  $j = room_load($code);
  if ($j === null || !isset($j['members'][$mid])) jout(array('ok' => false, 'error' => 'no-member'));
  $sdp = read_body();
  if (!$sdp) jout(array('ok' => false, 'error' => 'no-sdp'));
  // Tuersteher-Schluessel des Streamers unsichtbar mit durchreichen (nur der Server
  // kennt ihn aus dem Roster; der direkte IP-Zugriff hat ihn nicht -> Lumora weist ab).
  $vk = (isset($j['members'][$mid]['vk']) && $j['members'][$mid]['vk']) ? '?vk=' . rawurlencode($j['members'][$mid]['vk']) : '';
  foreach (member_addrs($j['members'][$mid]) as $base) {
    $r = relay($base . '/whep' . $vk, 'POST', $sdp, 'application/sdp');
    if ($r && ($r['status'] === 201 || $r['status'] === 200)) {
      $session = null;
      if ($r['location']) $session = preg_match('#^https?://#', $r['location']) ? $r['location'] : $base . $r['location'];
      jout(array('ok' => true, 'sdp' => $r['body'], 'session' => $session));
    }
  }
  jout(array('ok' => false, 'error' => 'unreachable'));
}
// WHEP-Session sauber abmelden (DELETE) - nur auf Adressen aus dem Roster.
if ($a === 'unwhep') {
  if (!code_ok($code)) jout(array('ok' => false, 'error' => 'bad-code'));
  $b = json_decode(read_body(), true);
  $sess = is_array($b) && isset($b['session']) ? (string)$b['session'] : '';
  $j = room_load($code);
  $allowed = false;
  if ($j !== null && $sess) {
    foreach ($j['members'] as $m) {
      foreach (member_addrs($m) as $base) if (strpos($sess, $base) === 0) { $allowed = true; break 2; }
    }
  }
  if ($allowed) relay($sess, 'DELETE', null, null);
  jout(array('ok' => true));
}
// Empfangsqualitaet einer Grid-Kachel an den jeweiligen Streamer durchreichen
// (adaptive Bitrate). Wie whep nur an Roster-Adressen - kein offenes Relay.
// Die HTTPS-Seite darf nicht direkt an http://<streamer> posten (Mixed Content).
if ($a === 'qos') {
  if (!code_ok($code)) jout(array('ok' => false, 'error' => 'bad-code'));
  $mid = isset($_GET['id']) ? $_GET['id'] : '';
  $j = room_load($code);
  if ($j === null || !isset($j['members'][$mid])) jout(array('ok' => false, 'error' => 'no-member'));
  $body = read_body();
  if ($body) {
    foreach (member_addrs($j['members'][$mid]) as $base) {
      $r = relay($base . '/qos', 'POST', $body, 'application/json');
      if ($r && ($r['status'] === 204 || $r['status'] === 200)) break;
    }
  }
  jout(array('ok' => true));
}
// ICE-Konfiguration (STUN/TURN) des jeweiligen Streamers durchreichen.
// PARALLEL an alle Adress-Kandidaten (nebenwirkungsfrei, GET) - der Gewinner wird
// als goodAddr im Raum gemerkt, damit der direkt folgende WHEP-Handshake sofort
// den funktionierenden Weg nimmt statt am toten Kandidaten zu haengen.
if ($a === 'cfg') {
  if (!code_ok($code)) jout(array('ok' => false, 'error' => 'bad-code'));
  $mid = isset($_GET['id']) ? $_GET['id'] : '';
  $j = room_load($code);
  if ($j !== null && isset($j['members'][$mid])) {
    $pairs = array();
    foreach (member_addrs($j['members'][$mid]) as $base) $pairs[] = array($base . '/cfg', $base);
    $r = relay_multi($pairs, 200);
    if ($r) {
      if (empty($j['members'][$mid]['goodAddr']) || $j['members'][$mid]['goodAddr'] !== $r['base']) {
        $j['members'][$mid]['goodAddr'] = $r['base'];
        room_save($code, $j);
      }
      header('Content-Type: application/json'); echo $r['body']; exit;
    }
  }
  jout(array('ok' => false));
}

// ---- Einzelstream-Zuschauerlink (?s=<CODE>) ------------------------------------
// Lumora registriert jeden Einzelstream als Ein-Personen-"Schattenraum" und teilt
// DIESE URL statt der nackten IP (Privatsphaere + iPhone-tauglich + ueberlebt
// IP-Wechsel). Die Seite entscheidet CLIENTSEITIG (robuster als Server-UA, iPads
// melden sich als Macintosh): iOS -> Grid ueber HTTPS (Relay-Handshake, laeuft
// nachweislich auf dem iPhone); alle anderen -> Weiterleitung auf den direkten
// IP-Player des Streamers (voller Funktionsumfang, Video bleibt P2P).
$s = isset($_GET['s']) ? strtoupper(trim($_GET['s'])) : '';
if ($s !== '' && code_ok($s)) {
  $j = room_load($s);
  $t4 = null; $t6 = null; $found = false;
  if ($j !== null) {
    foreach ($j['members'] as $mid => $m) {
      if ($mid === '_') continue;
      if ((!empty($m['linkV4']) || !empty($m['linkV6'])) && (!isset($m['streaming']) || $m['streaming'] !== false)) {
        $found = true;
        $t4 = !empty($m['linkV4']) ? (string)$m['linkV4'] : null;
        $t6 = !empty($m['linkV6']) ? (string)$m['linkV6'] : null;
        break;
      }
    }
  }
  header('Content-Type: text/html; charset=utf-8');
  header('Cache-Control: no-store');
  if (!$found) {
    // Stream (noch) nicht da: freundliche Offline-Seite, die alle 6 s selbst
    // nachschaut - wer den Link kurz vor dem Streamstart oeffnet, landet automatisch.
    echo '<!DOCTYPE html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><meta http-equiv="refresh" content="6"><title>Lumora Stream</title></head>'
       . '<body style="background:#0b0d12;color:#e8e8ea;font-family:system-ui;display:flex;align-items:center;justify-content:center;height:100vh;margin:0">'
       . '<div style="text-align:center"><div style="font-size:20px;font-weight:600" id="t1">Der Stream ist gerade offline</div>'
       . '<div style="color:#888;margin-top:8px" id="t2">Diese Seite prüft alle paar Sekunden automatisch neu.</div></div>'
       . '<script>if((navigator.language||"").toLowerCase().indexOf("de")!==0){document.documentElement.lang="en";document.getElementById("t1").textContent="The stream is currently offline";document.getElementById("t2").textContent="This page automatically re-checks every few seconds."}</script>'
       . '</body></html>';
    exit;
  }
  $T4 = json_encode($t4); $T6 = json_encode($t6); $SC = json_encode($s);
  // Desktop-Ziel: der Domain-Player (?p=..., IP bleibt aus der Adresszeile) -
  // nur wenn player.html neben diesem Skript deployt ist; sonst wie frueher
  // die direkte IP-Weiterleitung (Sicherheitsnetz, kein Redirect-Kreis moeglich).
  $desktopTarget = is_file(__DIR__ . '/player.html') ? '("?p=" + encodeURIComponent(code))' : '(v4 || v6)';
  echo '<!DOCTYPE html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Lumora Stream</title></head>'
     . '<body style="background:#0b0d12;color:#e8e8ea;font-family:system-ui;display:flex;align-items:center;justify-content:center;height:100vh;margin:0">'
     . '<div style="text-align:center"><div style="font-size:16px;color:#888" id="msg">Stream wird geöffnet…</div>'
     . '<div style="margin-top:14px"><a id="fallback" style="color:#7ab3ff" href="#"></a></div></div>'
     . "<script>\n"
     . "var v4 = $T4, v6 = $T6, code = $SC;\n"
     . 'var de = (navigator.language||"").toLowerCase().indexOf("de")===0;'
     . 'if(!de){document.documentElement.lang="en";document.getElementById("msg").textContent="Opening stream…"}'
     . 'var ua = navigator.userAgent;'
     . 'var ios = /iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && "ontouchend" in document);'
     . 'var target = ios ? ("?code=" + encodeURIComponent(code)) : ' . $desktopTarget . ';'
     . 'var fb = document.getElementById("fallback");'
     . 'fb.href = target; fb.textContent = de ? "Falls nichts passiert: hier tippen" : "If nothing happens: tap here";'
     . 'location.replace(target);'
     . "</script></body></html>";
  exit;
}

// ---- Einzelstream-Player von der Domain (?p=<CODE>) ---------------------------
// Liefert den vollwertigen Einzelplayer (player.html - DIESELBE Datei, die auch
// die App an lokale Zuschauer ausliefert) ueber HTTPS aus und injiziert
// window.RELAY: Die Signalisierung (cfg/whep/qos/unwhep) laeuft dann ueber diesen
// Vermittlungsserver, das Video bleibt direkt P2P. Ergebnis: In der Adresszeile
// des Zuschauers steht nie die IP des Streamers. Die Member-ID ist nur ein
// Startwert - der Player loest sie bei jedem Verbindungsaufbau frisch per a=list
// auf (App-Neustart vergibt eine neue ID).
$p = isset($_GET['p']) ? strtoupper(trim($_GET['p'])) : '';
if ($p !== '' && code_ok($p)) {
  $j = room_load($p);
  $mid = null; $midAny = null;
  if ($j !== null) {
    foreach ($j['members'] as $id => $m) {
      if ($id === '_') continue;
      if ($midAny === null) $midAny = $id;
      if (!isset($m['streaming']) || $m['streaming'] !== false) { $mid = $id; break; }
    }
  }
  if ($mid === null) $mid = $midAny;
  $html = ($mid !== null) ? @file_get_contents(__DIR__ . '/player.html') : false;
  // Raum weg oder player.html fehlt: zurueck zur ?s=-Seite (zeigt die Offline-
  // Ansicht mit Auto-Retry bzw. faellt ohne player.html auf die IP-Weiterleitung
  // zurueck - kein Kreis, weil ?s= nur bei vorhandener Datei auf ?p= zeigt).
  if ($html === false) { header('Location: ?s=' . $p); exit; }
  $cfg = '<script>window.RELAY = { api: ' . json_encode('?code=' . $p) . ', id: ' . json_encode($mid) . ', watch: ' . json_encode('?s=' . $p) . ' }</script>';
  $pos = strpos($html, '<head>');
  if ($pos !== false) $html = substr_replace($html, '<head>' . $cfg, $pos, strlen('<head>'));
  header('Content-Type: text/html; charset=utf-8');
  header('Cache-Control: no-store');
  echo $html; exit;
}

// ---- Grid-Player (Browser-Zuschauer) ------------------------------------------
if (!code_ok($code)) {
  header('Content-Type: text/html; charset=utf-8');
  echo '<!DOCTYPE html><html lang="de"><head><meta charset="utf-8"><title>Lumora Gruppen-Vermittlung</title></head>'
     . '<body style="background:#0b0d12;color:#e8e8ea;font-family:system-ui;display:flex;align-items:center;justify-content:center;height:100vh;margin:0">'
     . '<div style="text-align:center"><div style="font-size:20px;font-weight:600">Lumora Gruppen-Vermittlung ist aktiv ✅</div>'
     . '<div style="color:#888;margin-top:8px">Öffne einen Gruppen-Link mit ?code=… oder starte eine Gruppe in Lumora.</div></div></body></html>';
  exit;
}
header('Content-Type: text/html; charset=utf-8');
header('Cache-Control: no-store');
$CODE_JS = json_encode($code);
echo <<<'HTMLHEAD'
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Lumora Gruppen-Grid</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  html, body { height: 100%; background: #0b0d12; color: #e8e8ea; font-family: system-ui, -apple-system, Segoe UI, sans-serif; overflow: hidden; }
  body { height: 100dvh; }
  #grid { display: grid; gap: 8px; padding: 8px; height: 100%; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); grid-auto-rows: 1fr; }
  /* ── Lumora-Designsprache (wie App + Website): Akzentblau, Glas-Optik mit
     Licht-Kante, weiche Radien, Hover-Leben. Ein wiederverwendbarer Glas-Look
     fuer ALLE Overlays (Pills, Icons, Slider, Leisten-Knoepfe). ── */
  .tile { position: relative; background: #000; border-radius: 12px; overflow: hidden; cursor: pointer; border: 2px solid transparent; transition: border-color .18s, box-shadow .18s; touch-action: manipulation; }
  .tile:hover { border-color: rgba(255,255,255,.09); }
  .tile:fullscreen { border-radius: 0; border: none; background: #000; }
  .tile:-webkit-full-screen { border-radius: 0; border: none; background: #000; }
  .tile.active-audio { border-color: rgba(74,222,128,.75); box-shadow: 0 0 0 1px rgba(74,222,128,.25), 0 0 22px rgba(74,222,128,.18); }
  .tile video { width: 100%; height: 100%; object-fit: contain; background: #000; display: block; }
  .glass, .tile .label, .tile .stats, .tile .muteIco, .tile .statsIco, .tile .pipIco, .tile .volCtl {
    background: linear-gradient(180deg, rgba(30,34,48,.72), rgba(13,15,23,.78));
    -webkit-backdrop-filter: blur(14px) saturate(1.3); backdrop-filter: blur(14px) saturate(1.3);
    border: 1px solid rgba(255,255,255,.13);
    box-shadow: inset 0 1px 0 rgba(255,255,255,.09), 0 4px 16px rgba(0,0,0,.35);
  }
  .tile .label { position: absolute; left: 10px; bottom: 10px; max-width: calc(100% - 60px); padding: 6px 13px; border-radius: 999px; font-size: 13px; font-weight: 600; letter-spacing: .01em; display: flex; align-items: center; gap: 8px; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
  .tile .dot { width: 8px; height: 8px; border-radius: 50%; background: #f5b942; animation: pulse 1.2s infinite; flex-shrink: 0; }
  .tile .dot.live { background: #4ade80; box-shadow: 0 0 7px rgba(74,222,128,.9); animation: none; }
  .tile .dot.err { background: #f05a5a; box-shadow: 0 0 7px rgba(240,90,90,.8); animation: none; }
  .tile .dot.off { background: #777; animation: none; }
  @keyframes pulse { 0%,100% { opacity: 1 } 50% { opacity: .35 } }
  /* Kachel-Icons: einheitliche runde Glas-Knoepfe mit Hover-Pop */
  .tile .muteIco, .tile .statsIco, .tile .pipIco { position: absolute; top: 10px; border-radius: 50%; width: 32px; height: 32px; display: flex; align-items: center; justify-content: center; font-size: 14px; opacity: .6; transition: opacity .16s, transform .16s, border-color .16s; }
  .tile .muteIco { right: 10px; font-size: 15px; opacity: .85; }
  .tile .statsIco { right: 50px; }
  .tile .pipIco { right: 90px; display: none; }
  .tile .muteIco:hover, .tile .statsIco:hover, .tile .pipIco:hover { opacity: 1; transform: scale(1.12); border-color: rgba(122,179,255,.55); }
  .tile.stats-on .statsIco { opacity: 1; border-color: rgba(122,179,255,.55); color: #7ab3ff; }
  .tile.active-audio .muteIco { border-color: rgba(74,222,128,.55); }
  .tile .stats { position: absolute; top: 10px; left: 10px; display: none; padding: 6px 12px; border-radius: 10px; font-size: 11.5px; font-variant-numeric: tabular-nums; color: #ddd; }
  .tile.stats-on .stats { display: block; }
  .tile .stats b { color: #74e857; font-weight: 600; }
  /* Verbindet-Zustand: Glas-Pill mit Spinner statt nacktem Grau-Text */
  .tile .wait { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; font-size: 13px; text-align: center; padding: 16px; pointer-events: none; }
  .tile .wait > span, .tile .wait { color: #b6b9c6; }
  .tile .wait::before { content: ""; width: 0; height: 0; margin-right: 0; transition: none; }
  .tile .wait.connecting::before { content: ""; width: 15px; height: 15px; margin-right: 10px; border: 2px solid rgba(122,179,255,.25); border-top-color: #7ab3ff; border-radius: 50%; animation: spin 0.9s linear infinite; flex-shrink: 0; }
  @keyframes spin { to { transform: rotate(360deg) } }
  /* Bild-in-Bild-Knopf (nur sichtbar, wenn der Browser es kann - JS blendet ein) */
  /* Lautstaerke-Slider: nur an der aktiven Ton-Kachel; unten ZENTRIERT, damit die
     fixe Fenster-Knopfleiste (#bar) ihn nie ueberlagert. Eigene Thumb/Track-Optik. */
  .tile .volCtl { position: absolute; bottom: 10px; left: 50%; transform: translateX(-50%); display: none; align-items: center; border-radius: 999px; padding: 9px 14px; }
  .tile.active-audio .volCtl { display: flex; }
  .tile .volCtl input { -webkit-appearance: none; appearance: none; width: 96px; height: 4px; border-radius: 4px; background: rgba(255,255,255,.22); cursor: pointer; outline: none; }
  .tile .volCtl input::-webkit-slider-thumb { -webkit-appearance: none; width: 15px; height: 15px; border-radius: 50%; background: #fff; border: none; box-shadow: 0 1px 5px rgba(0,0,0,.55), 0 0 0 1px rgba(0,0,0,.12); transition: transform .14s; }
  .tile .volCtl input::-webkit-slider-thumb:hover { transform: scale(1.2); }
  .tile .volCtl input::-moz-range-thumb { width: 15px; height: 15px; border-radius: 50%; background: #fff; border: none; box-shadow: 0 1px 5px rgba(0,0,0,.55); }
  .tile .volCtl input::-moz-range-track { height: 4px; border-radius: 4px; background: rgba(255,255,255,.22); }
  @media (max-width: 640px), (pointer: coarse) { .tile .volCtl input { width: 116px; height: 6px; } }
  /* Spotlight-Ansicht: EINE grosse Hauptkachel, die uebrigen als Leiste darunter
     (Klick auf ein Thumbnail macht es zur Hauptkachel). Rein per Klassen - das
     Raster bleibt der Standard, der Zustand wird im Browser gemerkt. */
  #grid.spotlight { grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); grid-template-rows: minmax(0, 1fr) 118px; }
  #grid.spotlight .tile.spot { grid-row: 1; grid-column: 1 / -1; }
  #grid.spotlight .tile:not(.spot) { grid-row: 2; }
  #grid.spotlight .tile:not(.spot) .statsIco, #grid.spotlight .tile:not(.spot) .pipIco,
  #grid.spotlight .tile:not(.spot) .volCtl, #grid.spotlight .tile:not(.spot) .stats { display: none; }
  #grid.spotlight .tile:not(.spot) .label { font-size: 11px; padding: 3px 8px; max-width: calc(100% - 16px); }
  #empty { display: none; align-items: center; justify-content: center; height: 100%; flex-direction: column; gap: 8px; color: #777; text-align: center; padding: 24px; }
  #empty .big { font-size: 16px; color: #ccc; }
  #soundHint { position: fixed; left: 50%; bottom: 10%; transform: translateX(-50%); display: none; align-items: center; gap: 12px; cursor: pointer; z-index: 20; white-space: nowrap; background: linear-gradient(180deg, rgba(32,38,56,.92), rgba(15,18,30,.94)); color: #fff; border: 1px solid rgba(122,179,255,.45); border-radius: 999px; padding: 15px 26px; font-size: 17px; font-weight: 600; -webkit-backdrop-filter: blur(12px); backdrop-filter: blur(12px); box-shadow: inset 0 1px 0 rgba(255,255,255,.10), 0 10px 34px rgba(0,0,0,.55), 0 0 26px rgba(79,142,247,.22); animation: soundPulse 1.9s ease-in-out infinite; -webkit-tap-highlight-color: transparent; }
  #soundHint.show { display: flex; }
  #soundHint .ico { font-size: 22px; }
  @keyframes soundPulse { 0%,100% { transform: translateX(-50%) scale(1); } 50% { transform: translateX(-50%) scale(1.045); } }
  @media (max-width: 640px), (pointer: coarse) { #soundHint { font-size: 15px; padding: 13px 20px; white-space: normal; max-width: 88vw; text-align: left; bottom: 14%; } }
  /* Fenster-Knopfleiste: Glas-Pillen mit Licht-Kante, Hover hebt an + Akzent-Rand.
     Der Lumora-Beitritts-Knopf ist der Primaer-CTA im Marken-Gradient. */
  #bar { position: fixed; bottom: 0; right: 0; display: flex; justify-content: flex-end; align-items: center; gap: 10px; padding: 14px max(16px, env(safe-area-inset-right)) max(14px, env(safe-area-inset-bottom)) 16px; z-index: 15; }
  button.ctl, a.ctl {
    display: flex; align-items: center; gap: 8px;
    background: linear-gradient(180deg, rgba(30,34,48,.82), rgba(13,15,23,.88));
    color: #e8e8ea; border: 1px solid rgba(255,255,255,.13); border-radius: 12px;
    padding: 10px 16px; font-size: 13.5px; font-weight: 600; letter-spacing: .01em; cursor: pointer;
    -webkit-backdrop-filter: blur(14px) saturate(1.3); backdrop-filter: blur(14px) saturate(1.3);
    box-shadow: inset 0 1px 0 rgba(255,255,255,.09), 0 4px 16px rgba(0,0,0,.35);
    transition: transform .15s, border-color .15s, box-shadow .15s, background .15s;
    -webkit-tap-highlight-color: transparent; touch-action: manipulation; user-select: none;
  }
  button.ctl:hover, a.ctl:hover { border-color: rgba(122,179,255,.55); transform: translateY(-1px); box-shadow: inset 0 1px 0 rgba(255,255,255,.12), 0 8px 22px rgba(0,0,0,.45), 0 0 16px rgba(79,142,247,.15); }
  button.ctl:active, a.ctl:active { transform: translateY(0) scale(.97); }
  button.ctl svg { width: 17px; height: 17px; fill: currentColor; opacity: .9 }
  #lumoraJoinBtn { background: linear-gradient(135deg, #4f8ef7, #7ab3ff); color: #06080f; font-weight: 700; border-color: rgba(255,255,255,.22); box-shadow: inset 0 1px 0 rgba(255,255,255,.35), 0 6px 20px rgba(79,142,247,.35); }
  #lumoraJoinBtn:hover { border-color: rgba(255,255,255,.45); box-shadow: inset 0 1px 0 rgba(255,255,255,.4), 0 8px 26px rgba(79,142,247,.5); filter: brightness(1.06); }
  button.ctl:focus-visible, a.ctl:focus-visible { outline: 2px solid #7ab3ff; outline-offset: 2px; }
  @media (max-width: 560px) { #fsLabel { display: none; } }
  @media (max-width: 640px), (pointer: coarse) { button.ctl, a.ctl { padding: 12px 18px; font-size: 15px; min-height: 46px; border-radius: 13px; } }
  /* Steuerelemente bei Inaktivitaet ausblenden (wie im Einzel-Player): nach ein
     paar Sekunden ohne Maus/Tipp verschwinden Leiste, Kachel-Icons und Slider -
     nur Namens-Label und (angepinnte) Stats bleiben. Bewegung holt alles zurueck. */
  #bar { transition: opacity .3s; }
  .tile .volCtl { transition: opacity .3s; }
  body.idle #bar,
  body.idle .tile .muteIco, body.idle .tile .statsIco, body.idle .tile .pipIco,
  body.idle .tile .volCtl { opacity: 0; pointer-events: none; }
  body.idle { cursor: none; }
</style>
</head>
<body>
<div id="grid"></div>
<div id="empty">
  <div class="big">Noch niemand im Grid…</div>
  <div>Wartet auf Mitglieder der Gruppe.</div>
</div>
<div id="pipNote" style="display:none; align-items:center; justify-content:center; height:100%; flex-direction:column; gap:8px; color:#777; text-align:center; padding:24px; cursor:pointer">
  <div style="font-size:16px; color:#ccc">⧉ Das Grid läuft als Mini-Fenster</div>
  <div>Hier klicken, um es zurückzuholen.</div>
</div>
<div id="soundHint"><span class="ico">🔊</span><span>Auf eine Kachel tippen, um deren Ton zu hören</span></div>
<div id="bar">
  <!-- "Mit Lumora mitstreamen": oeffnet die installierte App per lumora://join/<CODE>
       (Protokoll registriert die App selbst). Nur auf Windows-Desktops eingeblendet
       (Lumora gibt es nur fuer Windows); ohne installierte App passiert beim Klick
       nichts - der Tooltip nennt die Download-Adresse. -->
  <a class="ctl" id="lumoraJoinBtn" style="display:none; text-decoration:none"
     title="Öffnet deine installierte Lumora-App und tritt der Gruppe bei – dein Stream startet automatisch mit. Noch kein Lumora? Kostenlos auf lumora-streaming.de" href="#">
    🎮 <span>Mit Lumora mitstreamen</span>
  </a>
  <button class="ctl" id="gridPipBtn" style="display:none" title="Das GANZE Grid als schwebendes Mini-Fenster über allen Programmen – ideal, um nebenbei selbst zu zocken. Ton und Klick-Bedienung funktionieren auch im Mini-Fenster.">
    ⧉ <span>Abdocken</span>
  </button>
  <button class="ctl" id="layoutBtn" style="display:none" title="Ansicht umschalten: Raster (alle gleich groß) / Spotlight (einer groß, Rest als Leiste)">
    <svg viewBox="0 0 24 24" id="layoutIcon"><path d="M3 3h18v12H3V3zm2 2v8h14V5H5zM3 17h5v4H3v-4zm6.5 0h5v4h-5v-4zM16 17h5v4h-5v-4z"/></svg>
    <span id="layoutLabel">Spotlight</span>
  </button>
  <button class="ctl" id="fsBtn" title="Vollbild (F) – Doppelklick auf eine Kachel für Einzelbild">
    <svg viewBox="0 0 24 24"><path d="M4 4h6v2H6v4H4V4zm10 0h6v6h-2V6h-4V4zM4 14h2v4h4v2H4v-6zm14 0h2v6h-6v-2h4v-4z"/></svg>
    <span id="fsLabel">Vollbild</span>
  </button>
</div>
<script>
HTMLHEAD;
echo "var ROOM = $CODE_JS;\n";
echo "var API = location.pathname + '?code=' + encodeURIComponent(ROOM);\n";
echo <<<'HTMLJS'
(function () {
  'use strict'
  // Grid-Player im Gruppen-Modus mit Vermittlungs-Server: Roster + Verbindungs-
  // Handshake laufen ueber DIESES Skript (HTTPS); die Videodaten selbst als
  // direkte WebRTC-Verbindung Browser <-> Streamer (kein Byte ueber den Server).
  var activeAudioId = null
  var tiles = new Map()
  var everInteracted = false
  var soundHintTimer = null, soundHintDone = false
  // EINE feste Element-Referenz aufs Grid: bleibt auch gueltig, wenn das Grid per
  // Document-PiP in ein anderes Fenster/Dokument verschoben wird (getElementById
  // im Haupt-document faende es dann nicht mehr).
  var gridEl = document.getElementById('grid')
  // --- Sprache: folgt der BROWSER-Sprache des Zuschauers (nicht der App-Einstellung
  // des Streamers) - Zuschauer koennen irgendwo sitzen. de* -> Deutsch, sonst Englisch.
  var IS_DE = String(navigator.language || '').toLowerCase().indexOf('de') === 0
  var T = IS_DE ? {
    connecting: 'Verbindet…', lost: 'Verbindung unterbrochen…', paused: '⏸ Streamt gerade nicht',
    player: 'Spieler', fs: 'Vollbild', fsExit: 'Beenden', spot: 'Spotlight', grid: 'Raster',
    dock: 'Abdocken', undock: 'Zurückholen',
    emptyBig: 'Noch niemand im Grid…', emptySub: 'Wartet auf Mitglieder der Gruppe.',
    pipBig: '⧉ Das Grid läuft als Mini-Fenster', pipSub: 'Hier klicken, um es zurückzuholen.',
    soundHint: 'Auf eine Kachel tippen, um deren Ton zu hören',
    join: 'Mit Lumora mitstreamen',
    joinTitle: 'Öffnet deine installierte Lumora-App und tritt der Gruppe bei – dein Stream startet automatisch mit. Noch kein Lumora? Kostenlos auf lumora-streaming.de',
    dockTitle: 'Das GANZE Grid als schwebendes Mini-Fenster über allen Programmen – ideal, um nebenbei selbst zu zocken. Ton und Klick-Bedienung funktionieren auch im Mini-Fenster.',
    layoutTitle: 'Ansicht umschalten: Raster (alle gleich groß) / Spotlight (einer groß, Rest als Leiste)',
    fsTitle: 'Vollbild (F) – Doppelklick auf eine Kachel für Einzelbild',
    statsTitle: 'Infoanzeige ein/aus', volTitle: 'Lautstärke',
    pipTitle: 'Bild-in-Bild: schwebendes Mini-Fenster über allen Programmen – ideal, um nebenbei selbst zu zocken'
  } : {
    connecting: 'Connecting…', lost: 'Connection lost…', paused: '⏸ Not streaming right now',
    player: 'Player', fs: 'Fullscreen', fsExit: 'Exit', spot: 'Spotlight', grid: 'Grid',
    dock: 'Pop out', undock: 'Bring back',
    emptyBig: 'Nobody in the grid yet…', emptySub: 'Waiting for group members.',
    pipBig: '⧉ The grid is running as a mini window', pipSub: 'Click here to bring it back.',
    soundHint: 'Tap a tile to hear its sound',
    join: 'Join with Lumora',
    joinTitle: 'Opens your installed Lumora app and joins the group – your own stream starts along automatically. No Lumora yet? Free at lumora-streaming.de',
    dockTitle: 'Pops the WHOLE grid out as a floating always-on-top mini window – perfect for gaming on the side. Sound and click controls keep working in the mini window.',
    layoutTitle: 'Switch the view: grid (all equal) / spotlight (one large, the rest as a strip)',
    fsTitle: 'Fullscreen (F) – double-click a tile for single view',
    statsTitle: 'Toggle stats overlay', volTitle: 'Volume',
    pipTitle: 'Picture-in-picture: floating mini window above all programs – perfect for gaming on the side'
  }
  // Statische Texte des Grundgeruests einmalig auf die Zuschauer-Sprache setzen.
  ;(function applyLang() {
    try { document.documentElement.lang = IS_DE ? 'de' : 'en' } catch (e) {}
    document.querySelector('#empty .big').textContent = T.emptyBig
    document.querySelector('#empty div:last-child').textContent = T.emptySub
    var pn = document.getElementById('pipNote')
    pn.children[0].textContent = T.pipBig
    pn.children[1].textContent = T.pipSub
    document.querySelector('#soundHint span:last-child').textContent = T.soundHint
    var jb = document.getElementById('lumoraJoinBtn')
    jb.querySelector('span').textContent = T.join
    jb.title = T.joinTitle
    var gp = document.getElementById('gridPipBtn')
    gp.querySelector('span').textContent = T.dock
    gp.title = T.dockTitle
    document.getElementById('layoutBtn').title = T.layoutTitle
    document.getElementById('fsBtn').title = T.fsTitle
    document.getElementById('fsLabel').textContent = T.fs
  })()
  // Gemerkte Nutzer-Einstellungen (localStorage): Ton-Wahl je RAUM (kommt nach
  // Reload/Reconnect wieder), Lautstaerke + Ansicht geraeteweit.
  var AUDIO_KEY = 'lumoraGridAudio-' + ROOM
  var pendingAudioId = null
  try { pendingAudioId = localStorage.getItem(AUDIO_KEY) || null } catch (e) {}
  var savedVol = 1
  try { savedVol = Math.min(1, Math.max(0, parseFloat(localStorage.getItem('lumoraGridVol') || '1'))) } catch (e) {}
  if (isNaN(savedVol)) savedVol = 1
  var spotlightOn = false
  try { spotlightOn = localStorage.getItem('lumoraGridSpot') === '1' } catch (e) {}
  var spotId = null
  // Ton-Restore POLICY-FEST: Browser blockieren unmuted-Autoplay ohne Geste -
  // darum wird die gemerkte Ton-Wahl erst bei der ERSTEN Beruehrung der Seite
  // scharf geschaltet (irgendein Klick/Tipp genuegt), nicht schon beim Laden.
  document.addEventListener('pointerdown', function () {
    if (!activeAudioId && pendingAudioId && tiles.has(pendingAudioId)) {
      activeAudioId = pendingAudioId
      tiles.forEach(function (t) { applyMute(t) })
    }
    pendingAudioId = null
  }, { once: true, capture: true })
  // Spotlight-Ansicht anwenden: grosse Hauptkachel + Thumbnail-Leiste. Faellt
  // automatisch aufs Raster zurueck, solange weniger als 2 Kacheln da sind.
  function applySpotlight() {
    var grid = gridEl
    if (spotId && !tiles.has(spotId)) spotId = null
    if (!spotId || (tiles.get(spotId) && tiles.get(spotId).streaming === false)) {
      spotId = null
      tiles.forEach(function (t, id) { if (!spotId && t.streaming !== false) spotId = id })
      if (!spotId) tiles.forEach(function (t, id) { if (!spotId) spotId = id })
    }
    var on = spotlightOn && tiles.size >= 2
    grid.classList.toggle('spotlight', on)
    tiles.forEach(function (t, id) { t.el.classList.toggle('spot', on && id === spotId) })
    var btn = document.getElementById('layoutBtn')
    btn.style.display = tiles.size >= 2 ? 'flex' : 'none'
    document.getElementById('layoutLabel').textContent = spotlightOn ? T.grid : T.spot
  }
  function setSpot(id) { spotId = id; applySpotlight() }
  document.getElementById('layoutBtn').onclick = function () {
    spotlightOn = !spotlightOn
    try { localStorage.setItem('lumoraGridSpot', spotlightOn ? '1' : '0') } catch (e) {}
    applySpotlight()
  }
  // --- Das GANZE Grid als schwebendes Mini-Fenster (Document-PiP) --------------
  // Anders als das Kachel-PiP (nur EIN Video) dockt dies das komplette Grid ab:
  // alle Streams, Ton und Klick-Bedienung in einem Always-on-top-Fenster - man
  // sieht die ganze Runde, waehrend man selbst zockt. Chromium-API (Chrome/Edge);
  // der Knopf erscheint nur, wenn der Browser sie kann.
  var pipWin = null
  function setGridDocked(docked) {
    document.getElementById('gridPipBtn').querySelector('span').textContent = docked ? T.dock : T.undock
    document.getElementById('pipNote').style.display = docked ? 'none' : 'flex'
  }
  document.getElementById('pipNote').onclick = function () { toggleGridPip() }
  async function toggleGridPip() {
    everInteracted = true
    if (pipWin) { try { pipWin.close() } catch (e) {} ; return }   // pagehide holt das Grid zurueck
    try {
      pipWin = await documentPictureInPicture.requestWindow({ width: 520, height: 320 })
    } catch (e) { pipWin = null; return }
    // Styles des Hauptfensters ins PiP-Fenster kopieren (sonst nacktes Layout).
    try {
      var css = ''
      for (var i = 0; i < document.styleSheets.length; i++) {
        try { var rules = document.styleSheets[i].cssRules; for (var k = 0; k < rules.length; k++) css += rules[k].cssText + '\n' } catch (e2) {}
      }
      var st = pipWin.document.createElement('style')
      st.textContent = css
      pipWin.document.head.appendChild(st)
      pipWin.document.body.style.margin = '0'
      pipWin.document.body.style.background = '#0b0d12'
      pipWin.document.body.style.height = '100vh'
    } catch (e3) {}
    pipWin.document.body.appendChild(gridEl)
    // Videos pausieren beim Verschieben zwischen Dokumenten -> alle neu anwerfen
    // (zusaetzlich heilt der Watchdog haengende von selbst).
    tiles.forEach(function (t) { try { t.video.play().catch(function () {}) } catch (e4) {} })
    setGridDocked(false)
    // Fenster zu (X oder erneuter Klick): Grid an seinen Platz zurueckholen.
    pipWin.addEventListener('pagehide', function () {
      try { document.body.insertBefore(gridEl, document.getElementById('empty')) } catch (e5) {}
      tiles.forEach(function (t) { try { t.video.play().catch(function () {}) } catch (e6) {} })
      pipWin = null
      setGridDocked(true)
    })
  }
  if (typeof window.documentPictureInPicture !== 'undefined') {
    var gpb = document.getElementById('gridPipBtn')
    gpb.style.display = 'flex'
    gpb.onclick = toggleGridPip
  }
  // Bildschirm wachhalten (Handy/Tablet): ohne Wake Lock geht das Display nach
  // ~30 s aus, waehrend man zuschaut. Re-Acquire bei Rueckkehr aus dem Hintergrund.
  var wakeLock = null
  function acquireWakeLock() {
    try {
      if ('wakeLock' in navigator && document.visibilityState === 'visible') {
        navigator.wakeLock.request('screen').then(function (l) { wakeLock = l }).catch(function () {})
      }
    } catch (e) {}
  }
  acquireWakeLock()
  document.addEventListener('visibilitychange', function () { if (document.visibilityState === 'visible') acquireWakeLock() })
  // --- Steuerelemente bei Inaktivitaet ausblenden ------------------------------
  // Nur ECHTE Mausbewegung (pointerType 'mouse') blendet ein - mobile Browser
  // feuern nach jedem Tipp simulierte Maus-Events (gleiche Falle wie im Einzel-
  // Player). Touch: jeder Tipp zeigt die Elemente und startet den Timer neu.
  var idleTimer = null
  function activity() {
    document.body.classList.remove('idle')
    clearTimeout(idleTimer)
    idleTimer = setTimeout(function () { document.body.classList.add('idle') }, 2600)
  }
  if (window.PointerEvent) {
    document.addEventListener('pointermove', function (e) { if (e.pointerType === 'mouse') activity() })
  } else {
    document.addEventListener('mousemove', activity)
  }
  document.addEventListener('touchstart', activity, { passive: true })
  activity()

  async function pollList() {
    try {
      var r = await fetch(API + '&a=list', { cache: 'no-store' })
      var j = await r.json()
      if (j && j.ok) applyRoster(j)
    } catch (e) {}
  }

  function applyRoster(j) {
    var members = j.members || []
    var seen = new Set()
    members.forEach(function (m) {
      seen.add(m.id)
      if (!m.linkV4 && !m.linkV6) return
      var t = tiles.get(m.id)
      var isNew = !t
      if (!t) { t = createTile(m.id); tiles.set(m.id, t) }
      t.nm.textContent = m.name || T.player
      var wasStreaming = t.streaming
      t.streaming = m.streaming !== false
      if (!t.streaming) {
        killPc(t)
        t.dot.className = 'dot off'
        t.wait.textContent = T.paused; t.wait.classList.remove('connecting')
        t.wait.style.display = 'flex'
        return
      }
      // ERSTAUFBAU SOFORT anstossen (neue Kachel bzw. pausiert -> streamt wieder):
      // vorher uebernahm das ausschliesslich der 4s-Watchdog - jede Kachel wartete
      // dadurch bis zu 4 s VOR dem ersten Verbindungsversuch (Hauptposten der
      // "dauert lange"-Wahrnehmung). Der Watchdog bleibt der einzige HEIL-Pfad
      // fuer alles Weitere; der connecting-Guard verhindert Doppelaufbauten.
      if (isNew || wasStreaming === false) connectTile(t)
    })
    tiles.forEach(function (t, id) { if (!seen.has(id)) { closeTile(t); tiles.delete(id) } })
    applySpotlight()   // Spotlight-Zuordnung nach jeder Roster-Aenderung nachfuehren
    var has = tiles.size > 0
    document.getElementById('empty').style.display = has ? 'none' : 'flex'
    var show = has && !everInteracted && !soundHintDone
    document.getElementById('soundHint').classList.toggle('show', show)
    if (show && !soundHintTimer) soundHintTimer = setTimeout(function () {
      soundHintDone = true
      document.getElementById('soundHint').classList.remove('show')
    }, 2000)
  }

  function createTile(id) {
    var el = document.createElement('div')
    el.className = 'tile'
    el.innerHTML =
      '<video muted autoplay playsinline></video>' +
      '<span class="wait">' + T.connecting + '</span>' +
      '<span class="label"><span class="dot"></span><span class="nm"></span></span>' +
      '<span class="stats"></span>' +
      '<span class="pipIco" title="' + T.pipTitle + '">⧉</span>' +
      '<span class="statsIco" title="' + T.statsTitle + '">📊</span>' +
      '<span class="muteIco">🔇</span>' +
      '<span class="volCtl"><input type="range" min="0" max="100" title="' + T.volTitle + '"></span>'
    // Klick: im Spotlight-Modus macht ein Klick aufs THUMBNAIL es zur Hauptkachel;
    // sonst (Raster bzw. Klick auf die grosse Kachel) schaltet er wie gehabt den Ton.
    el.onclick = function () {
      var isThumb = gridEl.classList.contains('spotlight') && !el.classList.contains('spot')
      if (isThumb) { everInteracted = true; setSpot(id) } else setActiveAudio(id)
    }
    el.ondblclick = function () { focusTile(id) }
    // iOS-Safari feuert bei Touch KEIN dblclick-Event - Doppel-Tipp selbst
    // erkennen (2 Taps < 350 ms auf derselben Kachel = Fokus-Vollbild).
    var lastTap = 0
    el.addEventListener('touchend', function () {
      var now = Date.now()
      if (now - lastTap < 350) { lastTap = 0; focusTile(id) } else lastTap = now
    })
    var t = {
      id: id, el: el, video: el.querySelector('video'), dot: el.querySelector('.dot'),
      nm: el.querySelector('.nm'), wait: el.querySelector('.wait'), muteIco: el.querySelector('.muteIco'),
      stats: el.querySelector('.stats'), statsOn: false, lastBytes: 0, lastTs: 0,
      streaming: true, pc: null, session: null,
      gen: 0,            // Verbindungs-Generation: entwertet Events/Fortsetzungen alter Verbindungen
      connecting: false, // genau EIN Aufbau zur Zeit
      startedAt: 0,      // Beginn des aktuellen Aufbaus (Watchdog: haengt der Aufbau?)
      lastFrames: -1, stallCount: 0,   // Watchdog: kommen wirklich neue Bilder an?
    }
    var ico = el.querySelector('.statsIco')
    ico.onclick = function (e) { e.stopPropagation(); t.statsOn = !t.statsOn; el.classList.toggle('stats-on', t.statsOn) }
    ico.ondblclick = function (e) { e.stopPropagation() }
    // Bild-in-Bild: nur anzeigen, wenn der Browser es kann (Chromium/Safari; auf
    // iPhone via webkitSetPresentationMode). Klick toggelt rein/raus.
    var pip = el.querySelector('.pipIco')
    var pipStd = 'pictureInPictureEnabled' in document && document.pictureInPictureEnabled
    var pipWebkit = t.video.webkitSupportsPresentationMode && typeof t.video.webkitSetPresentationMode === 'function'
    if (pipStd || pipWebkit) pip.style.display = 'flex'
    pip.onclick = function (e) {
      e.stopPropagation()
      everInteracted = true
      try {
        if (pipStd) {
          if (document.pictureInPictureElement === t.video) document.exitPictureInPicture().catch(function () {})
          else t.video.requestPictureInPicture().catch(function () {})
        } else if (pipWebkit) {
          t.video.webkitSetPresentationMode(t.video.webkitPresentationMode === 'picture-in-picture' ? 'inline' : 'picture-in-picture')
        }
      } catch (e2) {}
    }
    pip.ondblclick = function (e) { e.stopPropagation() }
    pip.addEventListener('touchend', function (e) { e.stopPropagation() })
    // Lautstaerke-Slider (nur an der aktiven Ton-Kachel sichtbar): Wert gilt
    // geraeteweit fuer alle Kacheln und wird gemerkt. Alle Events stoppen, damit
    // das Ziehen weder den Ton umschaltet noch als Doppel-Tipp (Vollbild) zaehlt.
    var vol = el.querySelector('.volCtl'), volInput = vol.querySelector('input')
    volInput.value = Math.round(savedVol * 100)
    volInput.oninput = function () {
      savedVol = Math.min(1, Math.max(0, volInput.value / 100))
      try { localStorage.setItem('lumoraGridVol', String(savedVol)) } catch (e2) {}
      tiles.forEach(function (o) { try { o.video.volume = savedVol; if (o !== t) o.el.querySelector('.volCtl input').value = volInput.value } catch (e3) {} })
    }
    ;['click', 'dblclick', 'pointerdown', 'touchend', 'touchstart'].forEach(function (ev) {
      vol.addEventListener(ev, function (e) { e.stopPropagation() })
    })
    try { t.video.volume = savedVol } catch (e2) {}
    gridEl.appendChild(el)   // gridEl statt getElementById: funktioniert auch im abgedockten PiP-Fenster
    return t
  }
  setInterval(function () {
    tiles.forEach(function (t) {
      if (!t.statsOn || !t.pc) return
      t.pc.getStats().then(function (s) {
        var inb = null
        s.forEach(function (r) { if (r.type === 'inbound-rtp' && r.kind === 'video') inb = r })
        if (!inb) return
        var fps = inb.framesPerSecond != null ? Math.round(inb.framesPerSecond) : '–'
        var res = (inb.frameWidth && inb.frameHeight) ? inb.frameWidth + '×' + inb.frameHeight : '–'
        var now = performance.now(), mbit = '–'
        if (t.lastTs && inb.bytesReceived != null) {
          var v = (inb.bytesReceived - t.lastBytes) * 8 / ((now - t.lastTs) / 1000) / 1e6
          if (v >= 0) mbit = v.toFixed(1)
        }
        t.lastBytes = inb.bytesReceived || 0; t.lastTs = now
        t.stats.innerHTML = '<b>' + fps + '</b> fps · ' + res + ' · ' + mbit + ' Mbit/s'
      }).catch(function () {})
    })
  }, 1000)

  // QoS-Meldung an den jeweiligen Streamer (adaptive Bitrate): alle 5 s ein
  // kleiner Delta-Bericht pro Kachel, via PHP-Relay (a=qos). Der Streamer senkt
  // bei anhaltend schlechtem Empfang automatisch seine Bitrate.
  var qosId = Math.random().toString(36).slice(2, 8)
  setInterval(function () {
    // Hintergrund-Tab: gedrosseltes Rendering laesst drop/freeze steigen, ohne
    // dass das Netz ein Problem hat - nicht melden (sonst Fehl-Drosselung).
    if (document.visibilityState !== 'visible') { tiles.forEach(function (t) { t.qosPrev = null }); return }
    tiles.forEach(function (t) {
      if (!t.pc || t.pc.connectionState !== 'connected') { t.qosPrev = null; return }
      t.pc.getStats().then(function (s) {
        var inb = null
        s.forEach(function (r) { if (r.type === 'inbound-rtp' && r.kind === 'video') inb = r })
        if (!inb) return
        var cur = { recv: inb.packetsReceived || 0, lost: inb.packetsLost || 0, drop: inb.framesDropped || 0, frz: inb.freezeCount || 0 }
        if (t.qosPrev) {
          fetch(API + '&a=qos&id=' + encodeURIComponent(t.id), { method: 'POST', body: JSON.stringify({
            id: qosId,
            recv: cur.recv - t.qosPrev.recv, lost: cur.lost - t.qosPrev.lost,
            drop: cur.drop - t.qosPrev.drop, frz: cur.frz - t.qosPrev.frz,
            jit: inb.jitter != null ? Math.round(inb.jitter * 1000) : 0,
            fps: inb.framesPerSecond != null ? inb.framesPerSecond : null,
          }) }).catch(function () {})
        }
        t.qosPrev = cur
      }).catch(function () {})
    })
  }, 5000)

  // Verbindung einer Kachel hart beenden + WHEP-Session abmelden. Erhoeht die
  // Generation, damit alle Events/awaits der ALTEN Verbindung wirkungslos werden -
  // eine sterbende alte Verbindung konnte sonst eine frisch aufgebaute kapern
  // (ihr failed-Event riss die neue wieder ab: Endlos-"Verbindet…").
  function killPc(t) {
    t.gen++
    var pc = t.pc
    t.pc = null
    if (pc) { try { pc.close() } catch (e) {} }
    if (t.session) {
      try { fetch(API + '&a=unwhep', { method: 'POST', keepalive: true, body: JSON.stringify({ session: t.session }) }) } catch (e) {}
      t.session = null
    }
  }
  function closeTile(t) {
    killPc(t)
    t.el.remove()
  }
  async function connectTile(t) {
    if (t.streaming === false || t.connecting) return
    t.connecting = true
    killPc(t)
    var gen = t.gen
    t.startedAt = Date.now()
    t.lastFrames = -1; t.stallCount = 0
    t.wait.textContent = T.connecting; t.wait.classList.add('connecting'); t.wait.style.display = 'flex'; t.dot.className = 'dot'
    try {
      var ice = [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun.cloudflare.com:3478' }], relay = false
      var bufMs = 300
      try {
        // cfg je Kachel 60 s cachen: Reconnects (Watchdog-Heilung, Qualitaets-
        // wechsel des Streamers) sparen die komplette Relay-Runde Browser->PHP->
        // Streamer. Nebeneffekt des frischen cfg-Aufrufs beim ERSTaufbau: der
        // PHP-Relay lernt dabei den funktionierenden Adress-Weg (goodAddr) fuer
        // den direkt folgenden WHEP-Handshake.
        var cfg = (t.cfgCache && (Date.now() - t.cfgCacheAt < 60000)) ? t.cfgCache
          : await fetch(API + '&a=cfg&id=' + encodeURIComponent(t.id), { cache: 'no-store' }).then(function (r) { return r.json() })
        if (cfg && cfg.ok !== false) { t.cfgCache = cfg; t.cfgCacheAt = Date.now() }
        if (cfg && Array.isArray(cfg.iceServers) && cfg.iceServers.length) ice = cfg.iceServers
        relay = !!(cfg && cfg.forceRelay)
        if (cfg && typeof cfg.buffer === 'number') bufMs = cfg.buffer
      } catch (e) {}
      if (gen !== t.gen) return   // inzwischen ersetzt/geschlossen
      var pc = new RTCPeerConnection({ iceServers: ice, iceTransportPolicy: relay ? 'relay' : 'all', bundlePolicy: 'max-bundle' })
      t.pc = pc
      var stream = new MediaStream()
      pc.addTransceiver('video', { direction: 'recvonly' })
      pc.addTransceiver('audio', { direction: 'recvonly' })
      pc.ontrack = function (e) {
        if (t.pc !== pc) return
        stream.addTrack(e.track)
        t.video.srcObject = stream
        // Empfangspuffer (Jitter-Buffer) setzen - fehlte im Grid komplett! Ohne ihn
        // lief jede Kachel mit Minimal-Puffer und JEDE kleine Sende-/Netzschwankung
        // schlug als Ruckler/"Verschlucken" durch. Wert kommt vom Streamer (/cfg),
        // wie beim Einzel-Player.
        try { e.receiver.playoutDelayHint = bufMs / 1000 } catch (err) {}
        try { e.receiver.jitterBufferTarget = bufMs } catch (err) {}
      }
      // Nur ANZEIGE - das Heilen uebernimmt zentral der Watchdog. 'disconnected'
      // ist bei WebRTC oft nur ein voruebergehender Schluckauf, der sich von
      // selbst erholt - nicht sofort abreissen.
      pc.addEventListener('connectionstatechange', function () {
        if (t.pc !== pc) return
        if (pc.connectionState === 'connected') {
          t.dot.className = 'dot live'; t.wait.style.display = 'none'; applyMute(t)
        } else if (pc.connectionState === 'failed') {
          t.dot.className = 'dot err'; t.wait.textContent = T.lost; t.wait.classList.remove('connecting'); t.wait.style.display = 'flex'
        }
      })
      var offer = await pc.createOffer()
      await pc.setLocalDescription(offer)
      await new Promise(function (res) {
        if (pc.iceGatheringState === 'complete') return res()
        // 1,5 s statt 2,5 s Sicherheits-Deckel: STUN antwortet normal in <300 ms
        // und 'complete' feuert vorher - der Deckel greift nur, wenn ein Adapter
        // (VPN o.ae.) das Sammeln aufhaelt. Dann reichen die bis dahin gesammelten
        // Kandidaten fast immer; im Zweifel heilt der Watchdog.
        var to = setTimeout(res, 1500)
        pc.addEventListener('icegatheringstatechange', function () { if (pc.iceGatheringState === 'complete') { clearTimeout(to); res() } })
      })
      if (gen !== t.gen) { try { pc.close() } catch (e) {}; return }
      var resp = await fetch(API + '&a=whep&id=' + encodeURIComponent(t.id), { method: 'POST', headers: { 'Content-Type': 'application/sdp' }, body: pc.localDescription.sdp })
      var j = await resp.json()
      if (gen !== t.gen) { try { pc.close() } catch (e) {}; return }
      if (!j || !j.ok || !j.sdp) {
        // Streamer (noch) nicht erreichbar: Verbindung SAUBER entsorgen, damit der
        // Watchdog es weiter versucht - eine liegengebliebene pc-Leiche blockierte
        // frueher jeden weiteren Aufbau ("kommt nie wieder auf die Beine").
        killPc(t)
        t.dot.className = 'dot err'; t.wait.textContent = T.lost; t.wait.classList.remove('connecting'); t.wait.style.display = 'flex'
        return
      }
      t.session = j.session || null
      await pc.setRemoteDescription({ type: 'answer', sdp: j.sdp })
    } catch (e) {
      if (gen === t.gen) killPc(t)
    } finally {
      if (gen === t.gen) t.connecting = false
      else t.connecting = false
    }
  }
  function framesDecoded(t) {
    return new Promise(function (res) {
      if (!t.pc) return res(null)
      t.pc.getStats().then(function (s) {
        var n = null; s.forEach(function (r) { if (r.type === 'inbound-rtp' && r.kind === 'video') n = r.framesDecoded })
        res(n)
      }).catch(function () { res(null) })
    })
  }
  // --- Watchdog: DER eine Heil-Pfad fuer alle Lebenslagen --------------------
  // Alle 4 s wird jede streamende Kachel geprueft und bei Bedarf neu verbunden:
  //  - keine Verbindung vorhanden (Erstaufbau, gescheiterter Versuch, Rueckkehr)
  //  - Verbindung failed/closed (Sender-Absturz, Netzwechsel)
  //  - Aufbau haengt seit >15 s (WHEP/ICE steckengeblieben)
  //  - Status sagt "connected", aber >12 s kein neues Bild (stiller Tod - mobile
  //    Browser und harte Sender-Abstuerze lassen den Status gern "connected" luegen)
  // Egal welcher Einzelpfad versagt: spaetestens hier heilt die Kachel von selbst.
  async function watchTile(t) {
    if (t.streaming === false || t.connecting) return
    if (document.visibilityState !== 'visible') return   // im Hintergrund kein Akku-Verbrennen; Rueckkehr triggert sofort
    if (!t.pc) { connectTile(t); return }
    var st = t.pc.connectionState
    if (st === 'failed' || st === 'closed') { connectTile(t); return }
    if (st === 'new' || st === 'connecting') {
      if (Date.now() - t.startedAt > 15000) connectTile(t)
      return
    }
    if (t.video.paused) t.video.play().catch(function () {})
    var n = await framesDecoded(t)
    if (n == null || t.connecting || !t.pc) return
    if (n === t.lastFrames) {
      t.stallCount++
      if (t.stallCount >= 3) connectTile(t)
    } else { t.stallCount = 0; t.lastFrames = n }
  }
  setInterval(function () { tiles.forEach(function (t) { watchTile(t) }) }, 4000)

  function setActiveAudio(id) {
    everInteracted = true
    document.getElementById('soundHint').classList.remove('show')
    activeAudioId = (activeAudioId === id) ? null : id
    // Wahl je Raum merken: nach Reload/Reconnect kommt der Ton (nach der ersten
    // Beruehrung, s. Policy-Restore oben) automatisch von derselben Person zurueck.
    try {
      if (activeAudioId) localStorage.setItem(AUDIO_KEY, activeAudioId)
      else localStorage.removeItem(AUDIO_KEY)
    } catch (e) {}
    tiles.forEach(function (t) { applyMute(t) })
  }
  function applyMute(t) {
    var on = t.id === activeAudioId
    t.video.muted = !on
    try { t.video.volume = savedVol } catch (e) {}
    t.el.classList.toggle('active-audio', on)
    t.muteIco.textContent = on ? '🔊' : '🔇'
  }

  // Vollbild mit Apple-Fallbacks: iPad-Safari kennt das Fullscreen-API nur mit
  // webkit-Prefix, iPhone-Safari GAR nicht fuer beliebige Elemente - dort bleibt
  // nur das NATIVE Video-Vollbild (video.webkitEnterFullscreen). Ein nackter
  // requestFullscreen()-Aufruf wirft auf dem iPhone einen synchronen TypeError,
  // den .catch() nicht faengt - der Knopf waere kommentarlos tot.
  function fsRequest(el) {
    try {
      if (el.requestFullscreen) { el.requestFullscreen().catch(function () {}); return true }
      if (el.webkitRequestFullscreen) { el.webkitRequestFullscreen(); return true }
    } catch (e) {}
    return false
  }
  function fsActive() { return document.fullscreenElement || document.webkitFullscreenElement || null }
  function fsExit() {
    try {
      if (document.exitFullscreen) document.exitFullscreen().catch(function () {})
      else if (document.webkitExitFullscreen) document.webkitExitFullscreen()
    } catch (e) {}
  }
  function toggleFullscreen() {
    if (fsActive()) { fsExit(); return }
    if (fsRequest(document.documentElement)) return
    // iPhone: kein Element-Vollbild moeglich. Wenn genau eine Kachel Ton hat bzw.
    // nur eine existiert, deren Video nativ in den Vollbildmodus schicken.
    var t = tiles.get(activeAudioId) || (tiles.size === 1 ? tiles.values().next().value : null)
    if (t && t.video && t.video.webkitEnterFullscreen) { try { t.video.webkitEnterFullscreen() } catch (e) {} }
  }
  function focusTile(id) {
    everInteracted = true
    document.getElementById('soundHint').classList.remove('show')
    activeAudioId = id
    tiles.forEach(function (t) { applyMute(t) })
    var t = tiles.get(id)
    if (!t) return
    // Doppelklick/Doppel-Tipp auf die BEREITS bildschirmfuellende Kachel fuehrt
    // wieder HERAUS (vorher lief erneutes requestFullscreen ins Leere - man kam
    // per Doppelklick rein, aber nicht mehr raus).
    if (fsActive() === t.el) { fsExit(); return }
    if (fsRequest(t.el)) return
    if (t.video && t.video.webkitEnterFullscreen) { try { t.video.webkitEnterFullscreen() } catch (e) {} }   // iPhone: natives Video-Vollbild
  }
  document.getElementById('fsBtn').onclick = toggleFullscreen
  // "Mit Lumora mitstreamen" nur auf Windows-Desktops zeigen (Lumora ist Windows-
  // only; auf iPhone/Android waere der Protokoll-Link eine Sackgasse).
  if (/Windows NT/.test(navigator.userAgent) && !/Mobile/.test(navigator.userAgent)) {
    var jb = document.getElementById('lumoraJoinBtn')
    if (jb) { jb.href = 'lumora://join/' + encodeURIComponent(ROOM); jb.style.display = 'flex' }
  }
  document.getElementById('soundHint').onclick = function () { everInteracted = true; document.getElementById('soundHint').classList.remove('show') }
  function fsLabelSync() { document.getElementById('fsLabel').textContent = fsActive() ? T.fsExit : T.fs }
  document.addEventListener('fullscreenchange', fsLabelSync)
  document.addEventListener('webkitfullscreenchange', fsLabelSync)
  document.addEventListener('keydown', function (e) { if (e.key === 'f' || e.key === 'F') toggleFullscreen() })

  // Rueckkehr aus dem Hintergrund: der Watchdog heilt ohnehin alles - hier nur
  // SOFORT einen Durchlauf anstossen (statt bis zu 4 s zu warten) und die vom
  // mobilen Browser pausierten Videos wieder anwerfen.
  function resumeAll() { tiles.forEach(function (t) { watchTile(t) }) }
  document.addEventListener('visibilitychange', function () { if (document.visibilityState === 'visible') resumeAll() })
  window.addEventListener('pageshow', function (e) { if (e.persisted) resumeAll() })
  function leaveAll() { tiles.forEach(function (t) { if (t.session) { try { fetch(API + '&a=unwhep', { method: 'POST', keepalive: true, body: JSON.stringify({ session: t.session }) }) } catch (e) {} } }) }
  window.addEventListener('pagehide', function (e) { if (!e.persisted) leaveAll() })
  window.addEventListener('beforeunload', leaveAll)

  pollList()
  setInterval(pollList, 4000)
})()
</script>
</body>
</html>
HTMLJS;
