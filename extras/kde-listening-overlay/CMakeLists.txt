cmake_minimum_required(VERSION 3.20)
project(voxtype_listening_overlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(LayerShellQt REQUIRED)

add_executable(voxtype-listening-overlay
    src/main.cpp
)

target_link_libraries(voxtype-listening-overlay PRIVATE Qt6::Widgets LayerShellQt::Interface)
