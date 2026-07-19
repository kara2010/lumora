cmake_minimum_required(VERSION 3.20)
project(lumora_elevate CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# WIN32 = GUI-Subsystem -> kein Konsolenfenster blitzt auf
add_executable(lumora_elevate WIN32 main.cpp app.rc)
target_link_libraries(lumora_elevate PRIVATE shell32)
