# lumora-media-relay: eigener WHEP-Relay (mediamtx-Ersatz)
# Voraussetzung: _testlab\build-relay-deps.ps1 einmal gelaufen (MbedTLS + libdatachannel statisch).
cmake_minimum_required(VERSION 3.20)
project(lumora_relay CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
set(LDC ${TP}/libdatachannel-relay)
set(MBED ${TP}/mbedtls/install)

add_executable(lumora_relay
  main.cpp relay_core.cpp ts_demux.cpp ingest.cpp whep_server.cpp ctrl_server.cpp)

target_include_directories(lumora_relay PRIVATE
  ${LDC}/include
  ${TP}/json)

# Statisch gegen libdatachannel + Submodule + MbedTLS
target_compile_definitions(lumora_relay PRIVATE RTC_STATIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(lumora_relay PRIVATE
  ${LDC}/build/Release/datachannel-static.lib
  ${LDC}/build/deps/libjuice/Release/juice-static.lib
  ${LDC}/build/deps/libsrtp/Release/srtp2.lib
  ${LDC}/build/deps/usrsctp/usrsctplib/Release/usrsctp.lib
  ${MBED}/lib/mbedtls.lib ${MBED}/lib/mbedx509.lib ${MBED}/lib/mbedcrypto.lib
  ws2_32 bcrypt iphlpapi)

# /utf-8 wie die Geschwisterprojekte (deutsche Kommentare/Ausgaben)
# /MD (Default) - MUSS zum Runtime der vorgebauten deps passen (libdatachannel baut /MD)
target_compile_options(lumora_relay PRIVATE /utf-8)
