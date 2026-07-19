cmake_minimum_required(VERSION 3.20)
project(lumora_shell CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# WIN32 = GUI-Subsystem (kein Konsolenfenster)
add_executable(lumora_shell WIN32 main.cpp app.rc)
target_compile_definitions(lumora_shell PRIVATE UNICODE _UNICODE)
target_include_directories(lumora_shell PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/webview2/build/native/include")
target_link_libraries(lumora_shell PRIVATE user32 gdi32 shell32 ole32 advapi32 version shlwapi
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/webview2/build/native/x64/WebView2LoaderStatic.lib")
