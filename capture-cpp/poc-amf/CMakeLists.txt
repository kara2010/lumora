cmake_minimum_required(VERSION 3.20)
project(poc_amf CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(poc_amf main.cpp)
target_include_directories(poc_amf PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/AMF/amf")
target_link_libraries(poc_amf PRIVATE d3d11 dxgi windowsapp)
