<?php
define('COUNTER_FILE', __DIR__ . '/data/count.txt');
define('DOWNLOAD_FILE', __DIR__ . '/updates/Lumora Setup 2.2.14.exe');

// ?count – nur Zählerstand zurückgeben (wird per JS beim Laden abgerufen)
if (isset($_GET['count'])) {
    header('Content-Type: application/json');
    $count = file_exists(COUNTER_FILE) ? (int) file_get_contents(COUNTER_FILE) : 0;
    echo json_encode(['count' => $count]);
    exit;
}

// Datei ausliefern + Zähler erhöhen
if (!file_exists(DOWNLOAD_FILE)) {
    http_response_code(404);
    exit('Datei nicht gefunden.');
}

// Atomares Hochzählen mit exklusivem Dateisperr
$fp = fopen(COUNTER_FILE, 'c+');
if ($fp && flock($fp, LOCK_EX)) {
    $count = (int) fread($fp, 32) + 1;
    ftruncate($fp, 0);
    rewind($fp);
    fwrite($fp, $count);
    flock($fp, LOCK_UN);
    fclose($fp);
}

// Datei an den Browser senden
$name = basename(DOWNLOAD_FILE);
header('Content-Type: application/octet-stream');
header('Content-Disposition: attachment; filename="' . $name . '"');
header('Content-Length: ' . filesize(DOWNLOAD_FILE));
header('Cache-Control: no-store');
readfile(DOWNLOAD_FILE);
