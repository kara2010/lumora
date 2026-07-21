# Baut den Lumora-Native-Installer (Phase 4): Shell bauen -> Staging -> NSIS -> signieren.
# Reproduzierbar; laeuft neben der Electron-App (eigener Ordner/Uninstall-Eintrag).
$ErrorActionPreference = "Stop"
$root = "C:\Users\kara\Documents\HDR-Launcher"
$shell = "$root\capture-cpp\lumora-shell"
$stage = "$shell\stage"
$version = "0.1.0"

# 1) Shell frisch bauen
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build "$shell\build" --config Release | Out-Null
if (-not (Test-Path "$shell\build\Release\lumora_shell.exe")) { throw "Shell-Build fehlgeschlagen" }

# 2) Staging aufbauen
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$shell\build\Release\lumora_shell.exe" "$stage\lumora-shell.exe"

# UI-Assets (was das WebView2 laedt) - genau die Dateien, die die Shell via app.lumora mappt
foreach ($f in "index.html","styles.css","player.html","osd.html","doorman.html","icon.ico","icon-64.png") {
  if (Test-Path "$root\$f") { Copy-Item "$root\$f" $stage }
}
# UI-Bilder/Assets (Logos etc.), die index.html/styles.css referenzieren
foreach ($f in Get-ChildItem "$root\*.png","$root\*.svg" -ErrorAction SilentlyContinue) {
  if ($f.Length -lt 500KB) { Copy-Item $f.FullName $stage }   # Screenshots (mehrere MB) NICHT ins Paket
}

# Native Binaries (nur die, die die Shell wirklich startet - FFmpeg/C#-Helfer entfallen).
# lumora-media-relay.exe ist jetzt der EIGENE C++-Relay (capture-cpp/lumora-relay);
# mediamtx.exe wird EIN Release lang als Legacy-Fallback (useLegacyRelay) mitgeliefert,
# mediamtx.default.yml entfaellt (der eigene Relay braucht keine YAML).
New-Item -ItemType Directory -Force "$stage\bin" | Out-Null
# libvpl.dll entfaellt: oneVPL-Dispatcher ist jetzt statisch in lumora-capture-native gelinkt.
foreach ($b in "lumora-capture-native.exe","lumora-media-relay.exe","mediamtx.exe","lumora-elevate.exe") {
  if (Test-Path "$root\bin\$b") { Copy-Item "$root\bin\$b" "$stage\bin" }
}
# Sensor-Module (OSD) neben die Shell (wie in der Electron-Struktur).
# HDRCmd.exe entfaellt (eigener Code: launch_game.h setHDR),
# PresentMon.exe entfaellt (eigener ETW-Consumer: etw_present.h).
foreach ($b in "AMDFamily17.bin","IntelMSR.bin") {
  if (Test-Path "$root\$b") { Copy-Item "$root\$b" $stage }
}
# Lizenztexte (Distributionspflicht)
foreach ($f in Get-ChildItem "$root\*-LICENSE.txt","$root\*LICENSE*.txt" -ErrorAction SilentlyContinue) { Copy-Item $f.FullName $stage }

# WebView2-Evergreen-Bootstrapper (Win10-Absicherung; ~2 MB, laedt bei Bedarf nach)
$bootstrap = "$stage\MicrosoftEdgeWebview2Setup.exe"
if (-not (Test-Path "$shell\MicrosoftEdgeWebview2Setup.exe")) {
  Invoke-WebRequest "https://go.microsoft.com/fwlink/p/?LinkId=2124703" -OutFile "$shell\MicrosoftEdgeWebview2Setup.exe"
}
Copy-Item "$shell\MicrosoftEdgeWebview2Setup.exe" $bootstrap

$stageSize = [math]::Round((Get-ChildItem $stage -Recurse | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Output "Staging: $stageSize MB"

# 2b) EIGENE EXE signieren - PFLICHT: Smart App Control / SmartScreen verlangen einen
# bestaetigten Herausgeber PRO ausfuehrbarer Datei, nicht nur fuer den Installer.
# Fremd-Binaries (lumora-elevate, lumora-media-relay, PresentMon, HDRCmd) sind bereits signiert.
$signtool = "$root\_testlab\tools\signtool\signtool.exe"
if (Test-Path $signtool) {
  foreach ($exe in "$stage\lumora-shell.exe", "$stage\bin\lumora-capture-native.exe", "$stage\bin\lumora-media-relay.exe", "$stage\bin\mediamtx.exe", "$stage\bin\lumora-elevate.exe") {
    if (Test-Path $exe) {
      $st = (Get-AuthenticodeSignature $exe).Status
      if ($st -ne "Valid") {
        & $signtool sign /sha1 EC6B6B6FDEBDB88941519F15E9570994CE3E14E3 /fd sha256 /tr http://time.certum.pl /td sha256 $exe | Out-Null
      }
      Write-Output ("Signatur {0}: {1}" -f (Split-Path $exe -Leaf), (Get-AuthenticodeSignature $exe).Status)
    }
  }
} else { Write-Warning "signtool fehlt - EXE bleiben UNSIGNIERT (Smart App Control blockiert sie!)" }

# 3) NSIS-Installer bauen
$makensis = (Get-ChildItem "$env:LOCALAPPDATA\electron-builder\Cache\nsis" -Recurse -Filter makensis.exe | Select-Object -First 1).FullName
if (-not $makensis) { throw "makensis.exe nicht gefunden" }
Push-Location $shell
& $makensis "/DVERSION=$version" "/DSRCDIR=stage" installer.nsi
Pop-Location
$out = "$shell\Lumora-Native-Setup-$version.exe"
if (-not (Test-Path $out)) { throw "NSIS-Build fehlgeschlagen" }

# 4) Signieren (Certum, wie alle unsere EXEs)
$signtool = "$root\_testlab\tools\signtool\signtool.exe"
if (Test-Path $signtool) {
  & $signtool sign /sha1 EC6B6B6FDEBDB88941519F15E9570994CE3E14E3 /fd sha256 /tr http://time.certum.pl /td sha256 $out | Select-Object -Last 1
  $sig = (Get-AuthenticodeSignature $out).Status
  Write-Output "Signatur: $sig"
}
$mb = [math]::Round((Get-Item $out).Length / 1MB, 1)
Write-Output "FERTIG: $out ($mb MB)"
