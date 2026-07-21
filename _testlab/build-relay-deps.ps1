# Baut die statischen Abhaengigkeiten fuer den eigenen C++-Relay (lumora-relay):
#   1) MbedTLS 3.6 (statisch, Release) -> third_party/mbedtls/install
#   2) libdatachannel v0.23 (statisch, USE_MBEDTLS, ohne WebSocket/Examples/Tests)
#      -> third_party/libdatachannel-relay/build/Release/datachannel-static.lib
# Einmalig pro PC auszufuehren (Build-Caches sind PC-spezifisch).
$ErrorActionPreference = "Stop"
$root  = Split-Path $PSScriptRoot -Parent
$tp    = "$root\capture-cpp\third_party"
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake).Source }

# --- 0) Quellen beziehen (nicht im Repo - zu gross; Versionen GEPINNT) ---
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
$ldc = "$tp\libdatachannel-relay"
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
