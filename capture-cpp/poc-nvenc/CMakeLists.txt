cmake_minimum_required(VERSION 3.20)
project(poc_nvenc CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(poc_nvenc main.cpp)
target_include_directories(poc_nvenc PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nv-codec-headers/include/ffnvcodec")
target_link_libraries(poc_nvenc PRIVATE d3d11 dxgi)
