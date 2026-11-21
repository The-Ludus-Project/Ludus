# Mobile M0 probe targets (docs/development/mobile-support-plan.md#acceptance-probes).
#
# Reached only through the root project when LUDUS_MOBILE_PROBE_ONLY is ON, which
# the ios-* and android-* presets set: FoundationBase still rejects iOS and
# Android, so these presets build the probes instead of the engine. The same
# translation units are compiled by scripts/mobile-probe for the SDK evidence.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
message(STATUS "Ludus mobile probes: flavor ${LUDUS_BUILD_FLAVOR}, build type ${CMAKE_BUILD_TYPE}")

set(LUDUS_FOUNDATION_BASE_INCLUDE "${PROJECT_SOURCE_DIR}/modules/foundation/base/include")

add_library(ludus_mobile_probe_cxx23 STATIC probe_cxx23.cpp)
target_include_directories(ludus_mobile_probe_cxx23 PUBLIC "${LUDUS_FOUNDATION_BASE_INCLUDE}")

add_executable(ludus_mobile_probe_runner probe_main.cpp)
target_link_libraries(ludus_mobile_probe_runner PRIVATE ludus_mobile_probe_cxx23)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 23)
    add_library(ludus_mobile_probe_objcxx STATIC probe_objcxx_arc.mm)
    target_include_directories(ludus_mobile_probe_objcxx PUBLIC "${LUDUS_FOUNDATION_BASE_INCLUDE}")
    target_compile_options(ludus_mobile_probe_objcxx PRIVATE -fobjc-arc -fno-objc-exceptions)
    target_link_libraries(ludus_mobile_probe_runner PRIVATE ludus_mobile_probe_objcxx "-framework Foundation")
    target_compile_definitions(ludus_mobile_probe_runner PRIVATE LUDUS_MOBILE_PROBE_HAS_OBJCXX=1)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_compile_definitions(ludus_mobile_probe_objcxx PRIVATE LUDUS_PROBE_REQUIRE_UIKIT=1)
        target_link_libraries(ludus_mobile_probe_runner PRIVATE "-framework UIKit")
    endif()
endif()

if(ANDROID)
    add_library(ludus_mobile_probe_jni SHARED probe_jni.cpp)
    target_include_directories(ludus_mobile_probe_jni PRIVATE "${LUDUS_FOUNDATION_BASE_INCLUDE}")
endif()

# Match the warning set of the engine (cmake/EngineCompilerWarnings.cmake) and its
# exception-free rule (AGENTS.md, "No C++ exceptions").
get_property(probe_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(probe_target IN LISTS probe_targets)
    target_compile_options(${probe_target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wundef -Wformat=2 -Werror -fno-exceptions)
endforeach()

if(BUILD_TESTING)
    # With a target toolchain CMake wraps the command in CMAKE_CROSSCOMPILING_EMULATOR
    # (scripts/mobile-device-runner), which refuses to run without a selected device.
    add_test(NAME ludus_mobile_probe_runner COMMAND ludus_mobile_probe_runner)
endif()
