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

# Staging (nur die Build-Dateien)
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force $tmp | Out-Null
foreach ($f in 'main.js','index.html','styles.css','icon.ico','icon-64.png','package.json') {
  Copy-Item "$src\$f" $tmp
}
$out = "$src\dist\win-unpacked\resources\app.asar"
& "$src\node_modules\.bin\asar.cmd" pack $tmp $out

# 1) ERST beenden  2) DANN kopieren
$running = Get-Process -Name "Lumora","HDR Launcher" -ErrorAction SilentlyContinue
if ($running) { $running | Stop-Process -Force; Start-Sleep -Milliseconds 1000; Write-Host "Lumora beendet (fuer Deploy)." }
$targets = @(
  "$env:LOCALAPPDATA\Programs\hdr-launcher\resources\app.asar",
  "$env:LOCALAPPDATA\Programs\lumora\resources\app.asar"
)
foreach ($t in $targets) {
  if (Test-Path (Split-Path $t)) { Copy-Item $out $t -Force; Write-Host "Deployed -> $t  ($((Get-Item $t).LastWriteTime.ToString('HH:mm:ss')))" }
}

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
