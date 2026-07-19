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
target_link_libraries(lumora_capture PRIVATE d3d11 dxgi windowsapp ws2_32 dwmapi ole32
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/opus/build/Release/opus.lib"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libvpl/build/Release/vpl.lib")
