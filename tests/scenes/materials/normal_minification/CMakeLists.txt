cmake_minimum_required(VERSION 3.25)
project(NormalMinification LANGUAGES CXX)

find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRenderer Resources FoundationThreading FoundationTime)

add_executable(minification_capture capture.cpp)
target_compile_features(minification_capture PRIVATE cxx_std_23)
target_link_libraries(minification_capture PRIVATE
    Ludus::GraphicsRenderer
    Ludus::Resources
    Ludus::FoundationTime
    Ludus::FoundationThreading)
ludus_compile_renderer_shaders(TARGET minification_capture SHADERS renderer_composite)
