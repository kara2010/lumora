# Lumora deployen: asar bauen + in die installierte App kopieren.
# WICHTIG: beendet Lumora ZUERST – ein laufender Prozess sperrt app.asar,
# sonst schlaegt die Kopie still fehl und die App laeuft mit altem Stand weiter.
#
#   ./deploy.ps1                      # bauen + deployen
#   ./deploy.ps1 -Verify "muster"     # zusaetzlich pruefen, dass <muster> im Deploy steckt
param([string]$Verify = "")
$ErrorActionPreference = "Stop"
$src = (Resolve-Path "$PSScriptRoot\..").Path
$tmp = Join-Path $env:TEMP "lumora-asar-stage"

# Syntax-Check
node -c "$src\main.js"; if (-not $?) { throw "Syntaxfehler in main.js" }
Write-Host "main.js OK"

# Staging (Build-Dateien)
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force $tmp | Out-Null
foreach ($f in 'main.js','index.html','osd.html','styles.css','icon.ico','icon-64.png','package.json') {
  Copy-Item "$src\$f" $tmp
}

# Produktions-node_modules mitpacken (z.B. electron-updater) – sonst stuerzt die
# App an require('electron-updater') ab bzw. der Updater fehlt. Liste dynamisch
# via npm, damit sich das bei neuen Abhaengigkeiten selbst pflegt.
Push-Location $src
$prod = & npm ls --omit=dev --parseable --all 2>$null | Where-Object { $_ -match '\\node_modules\\' }
Pop-Location
foreach ($p in $prod) {
  $rel = $p.Substring($src.Length + 1)         # z.B. node_modules\electron-updater
  $dest = Join-Path $tmp $rel
  if (-not (Test-Path $dest)) {
    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    Copy-Item $p $dest -Recurse -Force -ErrorAction SilentlyContinue
  }
}
Write-Host "Prod-Module gestaged: $($prod.Count)"
$out = "$src\dist\win-unpacked\resources\app.asar"
# --unpack: native .node (koffi/XInput) MUSS entpackt neben dem asar liegen –
# aus einem asar heraus laesst sich keine .node laden. Erzeugt app.asar.unpacked.
$unpackDir = "$out.unpacked"
if (Test-Path $unpackDir) { Remove-Item -Recurse -Force $unpackDir }
& "$src\node_modules\.bin\asar.cmd" pack $tmp $out --unpack "**/*.node"

# 1) ERST beenden  2) DANN kopieren
# Der FPS-Broker laeuft ELEVATED (geplante Aufgabe) und laesst sich mit normalen
# Rechten NICHT per Stop-Process killen ("Zugriff verweigert") – er sperrt aber
# app.asar mit. Darum zuerst die Aufgabe sauber beenden (das darf man am eigenen
# Task ohne Elevation), dann die normale App.
schtasks /End /TN "LumoraOSD-FPS" 2>$null | Out-Null
Start-Sleep -Milliseconds 500
$running = Get-Process -Name "Lumora","HDR Launcher" -ErrorAction SilentlyContinue
if ($running) { $running | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 1000; Write-Host "Lumora beendet (fuer Deploy)." }
$targets = @(
  "$env:LOCALAPPDATA\Programs\hdr-launcher\resources\app.asar",
  "$env:LOCALAPPDATA\Programs\lumora\resources\app.asar"
)
foreach ($t in $targets) {
  if (Test-Path (Split-Path $t)) {
    Copy-Item $out $t -Force
    # app.asar.unpacked mitliefern (native koffi/XInput-.node). Ziel vorher leeren,
    # damit keine veralteten Dateien zurueckbleiben.
    $tUnpack = "$t.unpacked"
    if (Test-Path $tUnpack) { Remove-Item -Recurse -Force $tUnpack }
    if (Test-Path $unpackDir) { Copy-Item $unpackDir $tUnpack -Recurse -Force }
    Write-Host "Deployed -> $t  ($((Get-Item $t).LastWriteTime.ToString('HH:mm:ss')))"
  }
}

# app-update.yml mitkopieren: electron-updater liest daraus die Feed-URL. Fehlt sie
# (installiert aus altem Build ohne publish-Config), schlaegt die Update-Pruefung
# fehl. Quelle ist der letzte electron-builder-Build; sonst inline erzeugen.
$updSrc = "$src\dist\win-unpacked\resources\app-update.yml"
if (-not (Test-Path $updSrc)) {
  @"
provider: generic
url: https://kara-webdesign.de/hdr-launcher/updates/
updaterCacheDirName: lumora-updater
"@ | Set-Content -Encoding UTF8 $updSrc
}
foreach ($t in $targets) {
  $resDir = Split-Path $t
  if (Test-Path $resDir) { Copy-Item $updSrc (Join-Path $resDir "app-update.yml") -Force }
}
Write-Host "app-update.yml mitgeliefert."

# Gebuendelte Binaries (extraResources, liegen NEBEN dem asar). Muessen beim
# Dev-Deploy mitkopiert werden, da sie nicht im asar stecken.
foreach ($bin in 'PresentMon.exe','PresentMon-LICENSE.txt') {
  $binSrc = Join-Path $src $bin
  if (Test-Path $binSrc) {
    foreach ($t in $targets) {
      $resDir = Split-Path $t
      if (Test-Path $resDir) { Copy-Item $binSrc (Join-Path $resDir $bin) -Force }
    }
  }
}
Write-Host "PresentMon mitgeliefert."

# Optionale Verifikation: main.js/index.html/styles.css aus dem DEPLOYTEN asar
# ins TEMP extrahieren und ueber ALLE pruefen (Muster kann in jeder Datei stehen).
# (NIE package.json extrahieren – electron-builder minimiert sie und wuerde den
#  build-Block der Projekt-package.json ueberschreiben.)
if ($Verify) {
  $dep = "$env:LOCALAPPDATA\Programs\lumora\resources\app.asar"
  $found = $false
  Push-Location $tmp
  foreach ($f in 'main.js','index.html','styles.css') {
    & "$src\node_modules\.bin\asar.cmd" extract-file $dep $f | Out-Null
    if (Select-String -Path (Join-Path $tmp $f) -Pattern $Verify -SimpleMatch -Quiet) { $found = $true }
  }
  Pop-Location
  Write-Host "Verify '$Verify' im Deploy:" $found
}
Write-Host "Fertig. Lumora bitte neu starten."
