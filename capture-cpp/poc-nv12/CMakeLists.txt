cmake_minimum_required(VERSION 3.20)
project(poc_nv12 CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(poc_nv12 main.cpp)
target_include_directories(poc_nv12 PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nv-codec-headers/include/ffnvcodec")
target_link_libraries(poc_nv12 PRIVATE d3d11 dxgi windowsapp)
