<?php
define('COUNTER_FILE', __DIR__ . '/data/count.txt');

// Auszuliefernde Datei NICHT mehr hart verdrahten (das lieferte nach dem 3.0.0-Release
// noch die alte 181-MB-Electron-exe aus). Stattdessen die aktuell veroeffentlichte Version
// aus dem Update-Feed native-update.json ziehen - den aktualisiert der Release-Build immer.
// So folgt der Download automatisch jedem Release; nur wenn der Feed fehlt/kaputt ist, greift
// der Fallback.
function currentDownloadFile() {
    $fallback = __DIR__ . '/updates/Lumora-Native-Setup-3.0.0.exe';
    $feed = __DIR__ . '/updates/native-update.json';
    if (is_file($feed)) {
        $j = json_decode((string) file_get_contents($feed), true);
        if (!empty($j['url'])) {
            // basename schuetzt vor Pfad-Manipulation; Datei muss lokal existieren
            $cand = __DIR__ . '/updates/' . basename((string) $j['url']);
            if (is_file($cand)) {
                return $cand;
            }
        }
    }
    return $fallback;
}
define('DOWNLOAD_FILE', currentDownloadFile());

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
