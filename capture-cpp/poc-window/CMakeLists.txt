cmake_minimum_required(VERSION 3.20)
project(poc_window CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(poc_window main.cpp)
target_include_directories(poc_window PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nv-codec-headers/include/ffnvcodec")
target_link_libraries(poc_window PRIVATE d3d11 dxgi windowsapp dwmapi)
