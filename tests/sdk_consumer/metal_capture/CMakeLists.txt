cmake_minimum_required(VERSION 3.29)
project(LudusMetalCaptureConsumer LANGUAGES CXX)

# G2 standalone fixture: an installed-SDK application captures its own Metal frames to
# .gputrace documents. Guide: docs/architecture/graphics-debugging.md, "Metal on macOS".
if(NOT APPLE)
    message(FATAL_ERROR "The Metal capture fixture requires macOS")
endif()

find_package(Ludus CONFIG REQUIRED)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

add_executable(ludus_metal_capture_consumer main.cpp)
target_link_libraries(ludus_metal_capture_consumer PRIVATE
    Ludus::GraphicsRhi Ludus::FoundationThreading Ludus::FoundationTime)
target_compile_features(ludus_metal_capture_consumer PRIVATE cxx_std_23)
set_target_properties(ludus_metal_capture_consumer PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(ludus_metal_capture_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
ludus_compile_shader(TARGET ludus_metal_capture_consumer NAME diagnostic
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/diagnostic.lusl"
    VERTEX VertexMain FRAGMENT FragmentMain)

enable_testing()
add_test(NAME ludus_installed_metal_capture COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify.py"
    "$<TARGET_FILE:ludus_metal_capture_consumer>" "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
# The first trace a process writes takes seconds; the fixture writes two in one run.
set_tests_properties(ludus_installed_metal_capture PROPERTIES TIMEOUT 240)
