# Baut die statischen Abhaengigkeiten fuer den eigenen C++-Relay (lumora-relay):
#   1) MbedTLS 3.6 (statisch, Release) -> capture-cpp/third_party/mbedtls/install
#   2) libdatachannel v0.23.1 (statisch, USE_MBEDTLS, ohne WebSocket/Examples/Tests)
#      -> capture-cpp/third_party/libdatachannel-relay/build/Release/datachannel-static.lib
# Einmalig pro PC auszufuehren (Build-Caches sind PC-spezifisch). Idempotent: bereits
# geklonte Quellen und bereits angewendete Patches werden erkannt und uebersprungen.
#
# WICHTIG - Patches in .\patches\ sind Teil des Produkts, nicht optional:
#   libdatachannel-lumora.patch     AV1-RTP-Packetizer (Chrome verwirft OBUs mit size-Feld;
#                                   ganze Temporal Unit als EINE Paketkette, sonst Schwarzbild
#                                   bei Encodern, die FRAME_HEADER+TILE_GROUP getrennt liefern)
#                                   + SSRC-Cache aus lokaler SDP (sonst kein NACK/PLI-Empfang)
#   libjuice-registry-mutex-leak.patch  1 Kernel-Handle-Leak je ICE-Mux-Session-Zyklus
# Die geklonten Quellen liegen NICHT im Repo (zu gross) - ohne diese Patches baut man einen
# Relay, der mit aktuellen Treibern kein Bild liefert.
$ErrorActionPreference = "Stop"
$root  = (Resolve-Path "$PSScriptRoot\..\..\..").Path     # Repo-Wurzel
$tp    = "$root\capture-cpp\third_party"
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake).Source }

# --- 0) Quellen beziehen (Versionen GEPINNT) ---
if (-not (Test-Path "$tp\libdatachannel-relay\CMakeLists.txt")) {
  git clone --depth 1 --branch v0.23.1 --recurse-submodules --shallow-submodules `
    https://github.com/paullouisageneau/libdatachannel.git "$tp\libdatachannel-relay"
  if ($LASTEXITCODE -ne 0) { throw "libdatachannel-Clone fehlgeschlagen" }
}
if (-not (Test-Path "$tp\mbedtls\CMakeLists.txt")) {
  git clone --depth 1 --branch v3.6.2 --recurse-submodules --shallow-submodules `
    https://github.com/Mbed-TLS/mbedtls.git "$tp\mbedtls"
  if ($LASTEXITCODE -ne 0) { throw "MbedTLS-Clone fehlgeschlagen" }
}

# --- 0b) Lumora-Patches auf die geklonten Quellen (idempotent ueber git apply --check) ---
$ldc = "$tp\libdatachannel-relay"
function Apply-Patch([string]$name) {
  $patch = "$PSScriptRoot\patches\$name"
  # Schon drin? Dann laesst sich der Patch rueckwaerts anwenden.
  git -C $ldc apply --reverse --check $patch 2>$null
  if ($LASTEXITCODE -eq 0) { Write-Output "Patch bereits angewendet: $name"; return }
  git -C $ldc apply --check $patch
  if ($LASTEXITCODE -ne 0) { throw "Patch passt nicht auf die Quellen: $name" }
  git -C $ldc apply $patch
  if ($LASTEXITCODE -ne 0) { throw "Patch fehlgeschlagen: $name" }
  Write-Output "Patch angewendet: $name"
}
Apply-Patch "libdatachannel-lumora.patch"
Apply-Patch "libjuice-registry-mutex-leak.patch"

# --- 1) MbedTLS ---
# WICHTIG: libdatachannel braucht DTLS-SRTP; in MbedTLS default AUS. Idempotent einschalten.
$mb = "$tp\mbedtls"
python "$mb\scripts\config.py" -f "$mb\include\mbedtls\mbedtls_config.h" set MBEDTLS_SSL_DTLS_SRTP
if ($LASTEXITCODE -ne 0) { throw "MbedTLS-Config (DTLS_SRTP) fehlgeschlagen" }
& $cmake -S $mb -B "$mb\build" -G "Visual Studio 17 2022" -A x64 `
  -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DMBEDTLS_FATAL_WARNINGS=OFF `
  -DCMAKE_INSTALL_PREFIX="$mb\install"
if ($LASTEXITCODE -ne 0) { throw "MbedTLS-Configure fehlgeschlagen" }
& $cmake --build "$mb\build" --config Release --target install -- /m
if ($LASTEXITCODE -ne 0) { throw "MbedTLS-Build fehlgeschlagen" }

# --- 2) libdatachannel (+ libjuice/libsrtp/usrsctp als Submodule) ---
& $cmake -S $ldc -B "$ldc\build" -G "Visual Studio 17 2022" -A x64 `
  -DUSE_MBEDTLS=ON -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON `
  -DCMAKE_PREFIX_PATH="$mb\install"
if ($LASTEXITCODE -ne 0) { throw "libdatachannel-Configure fehlgeschlagen" }
& $cmake --build "$ldc\build" --config Release --target datachannel-static -- /m
if ($LASTEXITCODE -ne 0) { throw "libdatachannel-Build fehlgeschlagen" }

Write-Output "FERTIG:"
Get-ChildItem "$ldc\build" -Recurse -Filter "*.lib" | ForEach-Object {
  "{0,10:N0} KB  {1}" -f ($_.Length/1KB), $_.FullName.Substring($root.Length+1)
}
