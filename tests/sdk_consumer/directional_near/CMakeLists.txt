cmake_minimum_required(VERSION 3.29)
project(LudusDirectionalNearReplay LANGUAGES CXX)
find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRenderer FoundationFilesystem FoundationTime FoundationThreading)
find_package(Catch2 3 CONFIG REQUIRED)
add_executable(ludus_directional_near_replay
    ../../../modules/graphics/renderer/tests/directional_near_render_tests.cpp)
target_compile_features(ludus_directional_near_replay PRIVATE cxx_std_23)
target_link_libraries(ludus_directional_near_replay PRIVATE Ludus::GraphicsRenderer
    Ludus::FoundationFilesystem Ludus::FoundationTime Ludus::FoundationThreading Catch2::Catch2WithMain)
ludus_compile_renderer_shaders(TARGET ludus_directional_near_replay SHADERS renderer_composite)
get_filename_component(prefix "${Ludus_DIR}/../../.." ABSOLUTE)
set(staged "${CMAKE_CURRENT_BINARY_DIR}/default")
file(COPY "${prefix}/share/Ludus/render-graphs/default/" DESTINATION "${staged}")
include(../renderer/linear_graph.cmake)
ludus_disable_staged_tone_maps("${staged}")
ludus_compile_render_graph(TARGET ludus_directional_near_replay NAME rg2_clustered_Default
    MANIFEST "${staged}/main.lushader.json" CONFIGURATION Default RUNTIME_PACKAGE)
cmake_path(GET rg2_clustered_Default_RUNTIME_PACKAGE PARENT_PATH package_directory)
target_compile_definitions(ludus_directional_near_replay PRIVATE LUDUS_RG3_CLUSTERED_DEFAULT_DIRECTORY="${package_directory}")
