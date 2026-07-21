cmake_minimum_required(VERSION 3.20)
project(lumora_capture CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(lumora_capture main.cpp app.rc)
target_include_directories(lumora_capture PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nv-codec-headers/include/ffnvcodec"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/AMF/amf"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/opus/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libvpl/api")
# vpl.lib STATISCH (build-static, BUILD_SHARED_LIBS=OFF) - libvpl.dll entfaellt im Paket.
# Einmalig bauen: cmake -S third_party/libvpl -B third_party/libvpl/build-static
#   -G "Visual Studio 17 2022" -A x64 -DBUILD_SHARED_LIBS=OFF, dann --target VPL.
target_link_libraries(lumora_capture PRIVATE d3d11 dxgi windowsapp ws2_32 dwmapi ole32
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/opus/build/Release/opus.lib"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libvpl/build-static/Release/vpl.lib")
