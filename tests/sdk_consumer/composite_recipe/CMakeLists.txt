# The renderer_composite recipe fixture of LUSL L5d wave 2 (issue 1736) against an installed SDK.
# It builds inside the renderer consumer project and as its own project, which the macOS batch
# plan configures against a relocated copy of the SDK (scripts/ci/macos_native.py).
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.29)
    project(LudusCompositeRecipeConsumer LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    find_package(Ludus CONFIG REQUIRED COMPONENTS GraphicsRhi FoundationThreading FoundationTime)
    enable_testing()
endif()

add_executable(ludus_sdk_composite_recipe composite_recipe.cpp)
target_include_directories(ludus_sdk_composite_recipe PRIVATE ..)
target_link_libraries(ludus_sdk_composite_recipe PRIVATE Ludus::GraphicsRhi Ludus::FoundationThreading Ludus::FoundationTime)
target_compile_options(ludus_sdk_composite_recipe PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
# The installed recipe and its generated renderer_composite_slots.h, which the fixture binds.
ludus_compile_renderer_shaders(TARGET ludus_sdk_composite_recipe SHADERS renderer_composite)
add_test(NAME ludus_sdk_composite_recipe COMMAND ludus_sdk_composite_recipe)
set_tests_properties(ludus_sdk_composite_recipe PROPERTIES PASS_REGULAR_EXPRESSION "composite recipe passed" TIMEOUT 120)
