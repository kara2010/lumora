# lumora-media-relay: eigener WHEP-Relay (mediamtx-Ersatz)
# Voraussetzung: deps\build-deps.ps1 einmal gelaufen (MbedTLS + libdatachannel statisch,
# inkl. der Lumora-Patches in deps\patches - ohne sie liefert der Relay kein AV1-Bild).
cmake_minimum_required(VERSION 3.20)
project(lumora_relay CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
set(LDC ${TP}/libdatachannel-relay)
set(MBED ${TP}/mbedtls/install)

# Statisch gegen libdatachannel + Submodule + MbedTLS. /utf-8 wie die Geschwisterprojekte
# (deutsche Kommentare/Ausgaben). /MD (Default) - MUSS zum Runtime der vorgebauten deps
# passen (libdatachannel baut /MD).
function(lumora_relay_link tgt)
  target_include_directories(${tgt} PRIVATE ${LDC}/include ${TP}/json)
  target_compile_definitions(${tgt} PRIVATE RTC_STATIC WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(${tgt} PRIVATE
    ${LDC}/build/Release/datachannel-static.lib
    ${LDC}/build/deps/libjuice/Release/juice-static.lib
    ${LDC}/build/deps/libsrtp/Release/srtp2.lib
    ${LDC}/build/deps/usrsctp/usrsctplib/Release/usrsctp.lib
    ${MBED}/lib/mbedtls.lib ${MBED}/lib/mbedx509.lib ${MBED}/lib/mbedcrypto.lib
    ws2_32 bcrypt iphlpapi)
  target_compile_options(${tgt} PRIVATE /utf-8)
endfunction()

add_executable(lumora_relay
  main.cpp relay_core.cpp ts_demux.cpp ingest.cpp whep_server.cpp ctrl_server.cpp
  app.rc)
lumora_relay_link(lumora_relay)

# BAU-SPERRE: AV1-RTP-Packetizer-Test (test/av1_packetizer_test.cpp). Prueft die Invarianten,
# auf die libwebrtc beim Zusammensetzen eines Bildes angewiesen ist - fuer BEIDE erlaubten
# Bitstrom-Formen (ein OBU_FRAME wie AMD/aeltere NVIDIA-Treiber, getrenntes FRAME_HEADER +
# TILE_GROUP wie neuere NVIDIA-Treiber). Schlaegt an, sobald libdatachannel aktualisiert
# oder der Lumora-Patch (deps/patches) verloren wird. Laeuft nach jedem Relay-Build; ein
# Fehlschlag bricht den Build ab.
add_executable(av1_packetizer_test test/av1_packetizer_test.cpp)
lumora_relay_link(av1_packetizer_test)
add_dependencies(lumora_relay av1_packetizer_test)
add_custom_command(TARGET lumora_relay POST_BUILD
  COMMAND $<TARGET_FILE:av1_packetizer_test>
  COMMENT "Bau-Sperre: AV1-RTP-Packetizer-Test")
