# Release-Build fuer Lumora:
#   1) electron-builder baut Installer + latest.yml
#   2) Release-Notes aus _testlab/release-notes/<version>.txt in latest.yml einfuegen
#   3) latest.yml + Installer + blockmap nach website/updates/ kopieren (alte .exe raus)
# Danach nur die 3 Dateien in website/updates/ auf den Server laden.
# HINWEIS: Diese Datei bewusst ASCII-only halten (PowerShell 5.1 liest .ps1 als ANSI).
$ErrorActionPreference = "Stop"
$src = (Resolve-Path "$PSScriptRoot\..").Path
$ver = (Get-Content "$src\package.json" -Raw | ConvertFrom-Json).version
Write-Host "== Release Lumora $ver =="

# 1) Bauen (Lumora vorher beenden)
Get-Process -Name "Lumora","HDR Launcher" -ErrorAction SilentlyContinue | Stop-Process -Force
Push-Location $src
& npx electron-builder --win
Pop-Location

$yml = "$src\dist\latest.yml"
if (-not (Test-Path $yml)) { throw "latest.yml fehlt - Build fehlgeschlagen?" }

# 2) Release-Notes einfuegen (YAML Block-Scalar); evtl. vorhandenen Block ersetzen.
#    Zweisprachig: gibt es <version>.en.txt, wird sie mit dem Marker ===EN===
#    an die deutschen Notes angehaengt (latest.yml kann nur EIN releaseNotes-Feld;
#    main.js splittet den Marker clientseitig). Der Update-Dialog zeigt je nach
#    App-Sprache DE oder EN.
$notesFile = "$src\_testlab\release-notes\$ver.txt"
$notesFileEn = "$src\_testlab\release-notes\$ver.en.txt"
if (Test-Path $notesFile) {
  $keep = @()
  $skip = $false
  foreach ($l in (Get-Content $yml)) {
    if ($l -match '^releaseNotes:') { $skip = $true; continue }
    if ($skip -and $l -match '^\s') { continue }
    $skip = $false
    $keep += $l
  }
  $notes = Get-Content $notesFile -Encoding UTF8
  $block = @("releaseNotes: |")
  foreach ($n in $notes) { $block += ("  " + $n) }
  if (Test-Path $notesFileEn) {
    $block += "  ===EN==="
    foreach ($n in (Get-Content $notesFileEn -Encoding UTF8)) { $block += ("  " + $n) }
  } else {
    Write-Host "WARN: keine EN-Notes-Datei $notesFileEn - englische Nutzer sehen die deutschen Notes."
  }
  # UTF-8 OHNE BOM schreiben (BOM kann den YAML-Parser des Updaters stoeren).
  $outText = (($keep + $block) -join "`n") + "`n"
  [System.IO.File]::WriteAllText($yml, $outText, (New-Object System.Text.UTF8Encoding($false)))
  Write-Host ("Release-Notes eingefuegt: " + $notes.Count + " Zeilen DE" + $(if (Test-Path $notesFileEn) { " + EN" } else { "" }) + ".")
} else {
  Write-Host "WARN: keine Notes-Datei $notesFile - latest.yml ohne releaseNotes."
}

# 3) Nach website/updates/ kopieren (alte Installer/Blockmaps entfernen)
$upd = "$src\website\updates"
New-Item -ItemType Directory -Force $upd | Out-Null
Get-ChildItem $upd -Filter "*.exe" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem $upd -Filter "*.blockmap" -ErrorAction SilentlyContinue | Remove-Item -Force
Copy-Item "$src\dist\Lumora Setup $ver.exe" $upd -Force
Copy-Item "$src\dist\Lumora Setup $ver.exe.blockmap" $upd -Force
Copy-Item $yml $upd -Force

# 4) download.php auf die neue Setup-Datei zeigen lassen (sonst laeuft der
#    Download-Button ins Leere = 404, weil die alte .exe oben geloescht wurde).
#    Frueher leicht vergessen -> jetzt automatisch. MUSS mit auf den Server!
$php = "$src\website\download.php"
if (Test-Path $php) {
  # WICHTIG: mit .NET lesen (erkennt UTF-8 korrekt). Get-Content -Raw ohne -Encoding
  # liest in PS 5.1 als Windows-1252 -> Umlaute im PHP werden beim Zurueckschreiben
  # zu Mojibake (Za"hlerstand -> ZAxa4hlerstand).
  $phpText = [System.IO.File]::ReadAllText($php)
  $phpText = $phpText -replace 'updates/Lumora Setup [0-9.]+\.exe', ("updates/Lumora Setup " + $ver + ".exe")
  [System.IO.File]::WriteAllText($php, $phpText, (New-Object System.Text.UTF8Encoding($false)))
  Write-Host "download.php aktualisiert -> Lumora Setup $ver.exe"
} else {
  Write-Host "WARN: download.php nicht gefunden - Download-Button NICHT aktualisiert!"
}

# 5) KEIN Datei-Update-Manifest mehr (Nutzer-Entscheidung 2026-07-15: der
#    Datei-Update-Weg hat einmal saemtliche Clients zerstoert). Updates laufen
#    AUSSCHLIESSLICH als voller Installer ueber electron-updater/latest.yml;
#    fuCheck() in main.js ist hart deaktiviert, updates/app/ wird weder
#    erzeugt noch jemals deployt (Server-Manifest = 0.0.0-Tombstone).

Write-Host "`nBereit zum Hochladen (NUR voller Installer):"
Write-Host ("  updates/Lumora Setup " + $ver + ".exe  (ZUERST)")
Write-Host ("  updates/Lumora Setup " + $ver + ".exe.blockmap")
Write-Host "  download.php + geaenderte Website-Seiten"
Write-Host "  updates/latest.yml  (ZULETZT - Feed zeigt erst auf existierenden Setup)"
Get-ChildItem $upd -File | Select-Object Name, @{n='MB';e={[math]::Round($_.Length/1MB,2)}} | Format-Table -AutoSize
