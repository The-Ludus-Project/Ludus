# Build the same manifest recipe in the source tree and an external SDK consumer.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.29)
    project(LudusLuslLayoutProbe LANGUAGES CXX)
    find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRhi)
endif()

add_library(ludus_lusl_layout_probe STATIC layout_probe.cpp)
target_link_libraries(ludus_lusl_layout_probe PRIVATE Ludus::GraphicsRhi)
ludus_compile_renderer_shaders(TARGET ludus_lusl_layout_probe SHADERS renderer_lusl_layout_probe)
