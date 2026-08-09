cmake_minimum_required(VERSION 3.20)
project(lumora_shell CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# WIN32 = GUI-Subsystem (kein Konsolenfenster)
add_executable(lumora_shell WIN32 main.cpp app.rc
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ViGEmClient/src/ViGEmClient.cpp")
target_compile_definitions(lumora_shell PRIVATE UNICODE _UNICODE)
# Quelle + narrow-Literale als UTF-8 (nur CXX, nicht die .rc). Ohne das interpretiert
# MSVC direkte UTF-8-Zeichen in WIDE-Literalen (z.B. L"• ...") als CP1252 -> Kauderwelsch.
target_compile_options(lumora_shell PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
target_include_directories(lumora_shell PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/webview2/build/native/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/json"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ViGEmClient/include")
target_link_libraries(lumora_shell PRIVATE user32 gdi32 shell32 ole32 advapi32 version shlwapi
  setupapi hid dwmapi
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/webview2/build/native/x64/WebView2LoaderStatic.lib")
