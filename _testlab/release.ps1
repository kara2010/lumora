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
Write-Host "`nBereit in website\updates\ (zum Hochladen):"
Get-ChildItem $upd | Select-Object Name, @{n='MB';e={[math]::Round($_.Length/1MB,2)}} | Format-Table -AutoSize
