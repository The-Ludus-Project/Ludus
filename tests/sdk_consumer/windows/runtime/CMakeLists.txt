cmake_minimum_required(VERSION 3.29)
project(LudusWindowsRuntimeConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23preview")
string(REGEX REPLACE "(^| )[/-]EHsc( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "(^| )[/-]W[0-4]( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# The installed runtime closure, resolved only from the relocated prefix.
find_package(Ludus CONFIG REQUIRED
    COMPONENTS GraphicsRenderer GraphicsText Text Resources Content GameplayWorld GameplayCamera Ui Localization GameHost
    PATHS "${CMAKE_PREFIX_PATH}" NO_DEFAULT_PATH)
if(NOT Ludus_BUILD_FLAVOR STREQUAL "Development")
    message(FATAL_ERROR "Expected the Development runtime SDK, found ${Ludus_BUILD_FLAVOR}.")
endif()
foreach(dependency IN ITEMS freetype harfbuzz)
    cmake_path(IS_PREFIX CMAKE_PREFIX_PATH "${${dependency}_DIR}" NORMALIZE is_bundled)
    if(NOT is_bundled)
        message(FATAL_ERROR "${dependency} resolved outside the installed SDK: ${${dependency}_DIR}")
    endif()
endforeach()
if(NOT COMMAND ludus_compile_renderer_shaders)
    message(FATAL_ERROR "The installed runtime SDK lacks the renderer shader recipes")
endif()

add_executable(ludus_windows_runtime_consumer runtime.cpp)
target_link_libraries(ludus_windows_runtime_consumer PRIVATE
    Ludus::GraphicsRenderer Ludus::GraphicsText Ludus::Text Ludus::Resources Ludus::Content
    Ludus::GameplayWorld Ludus::GameplayCamera Ludus::Ui Ludus::Localization Ludus::GameHost
    Ludus::FoundationTime)
ludus_apply_app_policy(ludus_windows_runtime_consumer)
target_compile_options(ludus_windows_runtime_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor)
ludus_compile_renderer_shaders(TARGET ludus_windows_runtime_consumer SHADERS renderer_flat)

enable_testing()
add_test(NAME ludus_windows_runtime_consumer COMMAND ludus_windows_runtime_consumer)
set_tests_properties(ludus_windows_runtime_consumer PROPERTIES TIMEOUT 60)
