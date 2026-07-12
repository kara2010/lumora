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
$notesFile = "$src\_testlab\release-notes\$ver.txt"
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
  # UTF-8 OHNE BOM schreiben (BOM kann den YAML-Parser des Updaters stoeren).
  $outText = (($keep + $block) -join "`n") + "`n"
  [System.IO.File]::WriteAllText($yml, $outText, (New-Object System.Text.UTF8Encoding($false)))
  Write-Host ("Release-Notes eingefuegt: " + $notes.Count + " Zeilen.")
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

# 5) Datei-Update-Manifest (Basis-Installer-Strategie, seit 2.2.12): app.asar,
#    entpackte Module und Binaries mit sha512 nach website/updates/app/.
#    Installierte Clients (ab 2.2.12) aktualisieren sich darueber selbst -
#    hochgeladen werden muessen nur manifest.json + die als GEAENDERT
#    gemeldeten Dateien. latest.yml bleibt fuer seltene Basis-Wechsel
#    (neue Electron-Version) bestehen.
$res = "$src\dist\win-unpacked\resources"
$appUpd = "$src\website\updates\app"
New-Item -ItemType Directory -Force $appUpd | Out-Null
$files = @()
foreach ($it in 'app.asar','gruppe.php','PresentMon.exe','HDRCmd.exe','AMDFamily17.bin','IntelMSR.bin') {
  if (Test-Path "$res\$it") { $files += Get-Item "$res\$it" }
}
$files += Get-ChildItem -Recurse -File "$res\app.asar.unpacked" -ErrorAction SilentlyContinue
$files += Get-ChildItem -Recurse -File "$res\bin" -ErrorAction SilentlyContinue
$oldMf = $null
if (Test-Path "$appUpd\manifest.json") { $oldMf = [System.IO.File]::ReadAllText("$appUpd\manifest.json") | ConvertFrom-Json }
$entries = @()
$changedList = @()
foreach ($f in $files) {
  $rel = $f.FullName.Substring($res.Length + 1) -replace '\\','/'
  $sha = (Get-FileHash -Algorithm SHA512 $f.FullName).Hash.ToLower()
  $entries += [pscustomobject]@{ path = $rel; size = $f.Length; sha512 = $sha }
  $dst = Join-Path $appUpd ($rel -replace '/','\')
  New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
  Copy-Item $f.FullName $dst -Force
  $old = $null
  if ($oldMf) { $old = $oldMf.files | Where-Object { $_.path -eq $rel } }
  if (-not $old -or $old.sha512 -ne $sha) { $changedList += $rel }
}
$notesText = ''
if (Test-Path $notesFile) { $notesText = ([System.IO.File]::ReadAllText($notesFile)).Trim() }
$elRaw = (Get-Content "$src\package.json" -Raw | ConvertFrom-Json).devDependencies.electron
$elMajor = [int][regex]::Match($elRaw, '\d+').Value
$mf = [pscustomobject]@{ version = $ver; minElectron = $elMajor; notes = $notesText; files = $entries }
[System.IO.File]::WriteAllText("$appUpd\manifest.json", ($mf | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Datei-Update-Manifest: " + $entries.Count + " Dateien erfasst, " + $changedList.Count + " geaendert seit letztem Manifest.")

Write-Host "`nBereit zum Hochladen:"
Write-Host "  1) IMMER: Datei-Update fuer Bestandsclients (ab 2.2.12) nach updates/app/:"
Write-Host "       updates/app/manifest.json"
foreach ($c in $changedList) { Write-Host ("       updates/app/" + $c) }
Write-Host "  2) NUR BEI BASIS-WECHSEL (neue Electron-Version) oder Erst-Go-Live:"
Write-Host "     die 3 Dateien in website\updates\ + website\download.php."
Write-Host "     Sonst Setup/latest.yml/download.php auf dem Server EINGEFROREN lassen -"
Write-Host "     der Basis-Installer sammelt SmartScreen-Reputation nur, wenn er sich nicht aendert."
Get-ChildItem $upd -File | Select-Object Name, @{n='MB';e={[math]::Round($_.Length/1MB,2)}} | Format-Table -AutoSize
