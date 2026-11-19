cmake_minimum_required(VERSION 3.29)
project(LudusEnvironmentQualification LANGUAGES CXX)
find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRenderer SceneCook Resources FoundationTime FoundationThreading)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_executable(ludus_sdk_environment capture.cpp)
target_compile_features(ludus_sdk_environment PRIVATE cxx_std_23)
set_target_properties(ludus_sdk_environment PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(ludus_sdk_environment PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
target_link_libraries(ludus_sdk_environment PRIVATE Ludus::GraphicsRenderer Ludus::SceneCook Ludus::Resources
    Ludus::FoundationTime Ludus::FoundationThreading)
ludus_compile_renderer_shaders(TARGET ludus_sdk_environment SHADERS
    renderer_flat renderer_composite renderer_output renderer_shadow renderer_authored renderer_authored_shadow)
add_test(NAME ludus_sdk_environment COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/qualify.py"
    "$<TARGET_FILE:ludus_sdk_environment>")
set_tests_properties(ludus_sdk_environment PROPERTIES TIMEOUT 600)
