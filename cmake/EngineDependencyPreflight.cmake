# Optional dependencies are checked only for enabled features.
include("${CMAKE_CURRENT_LIST_DIR}/LudusDependencies.cmake")
if(LUDUS_BUILD_SHADER_PROBE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ludus_dependency_error("Shader feasibility probe" "the Linux Vulkan backend"
        "LUDUS_BUILD_SHADER_PROBE is enabled, but this optional Vulkan probe is unsupported on ${CMAKE_SYSTEM_NAME}. macOS uses Metal."
        "Rerun ./init.sh --no-shader-probe to update saved local setup choices.\n  For custom configurations without saved init choices, set LUDUS_BUILD_SHADER_PROBE=OFF.\n  Metal shader compilation, world_demo and the Cornell box sample do not require this probe.")
endif()
get_filename_component(setup_preset "${CMAKE_BINARY_DIR}" NAME)
set(setup_recovery "From the repository root: ./init.sh --cli --preset ${setup_preset} --preset-only --locked")
if(NOT setup_preset MATCHES "^(linux|macos)-clang-(debug|development|profile|release|asan-ubsan|tsan)$")
    set(setup_recovery "From the repository root: ./init.sh --cli --locked")
endif()
if(LUDUS_BUILD_TESTS)
    ludus_require_python("Engine tests")
    find_package(Catch2 3 QUIET CONFIG)
    if(NOT Catch2_FOUND)
        ludus_dependency_error("Engine tests" "Catch2 3"
            "Conan package metadata is missing or incompatible. CMAKE_PREFIX_PATH='${CMAKE_PREFIX_PATH}'."
            "${setup_recovery} --with-tests\n  For custom builds, set Catch2_DIR to its package configuration directory.")
    endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(volk QUIET CONFIG)
    if(NOT volk_FOUND)
        ludus_dependency_error("Vulkan rendering" "volk"
            "Conan package metadata is missing. CMAKE_PREFIX_PATH='${CMAKE_PREFIX_PATH}'."
            "${setup_recovery}\n  For custom builds, set volk_DIR to its package configuration directory.")
    endif()
endif()

# The Windows configuration builds only the FoundationThreading closure (no text).
if(NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    foreach(package freetype harfbuzz)
        find_package(${package} QUIET CONFIG)
        if(NOT ${package}_FOUND)
            ludus_dependency_error("Text rendering" "${package}"
                "Conan package metadata is missing. CMAKE_PREFIX_PATH='${CMAKE_PREFIX_PATH}'."
                "${setup_recovery}\n  For custom builds, set ${package}_DIR to its package configuration directory.")
        endif()
    endforeach()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    include(CheckCXXSourceCompiles)
    # Recheck after SDK/compiler changes, even when their cache paths are unchanged.
    unset(LUDUS_MACOS_SDK_MATH_WORKS CACHE)
    # <cfloat> after <cmath> matches init's probe: C++23 libc++ 22 with the macOS 27
    # SDK leaves it empty (llvm/llvm-project#175849).
    check_cxx_source_compiles("#include <cmath>
#include <cfloat>
#ifndef NAN
#error LudusSDK_missing_NAN
#endif
#ifndef INFINITY
#error LudusSDK_missing_INFINITY
#endif
#ifndef DBL_MAX_10_EXP
#error LudusSDK_missing_DBL_MAX_10_EXP
#endif
int main() { volatile double value = NAN; return std::isnan(value) ? 0 : 1; }"
        LUDUS_MACOS_SDK_MATH_WORKS)
    if(NOT LUDUS_MACOS_SDK_MATH_WORKS)
        ludus_dependency_error("macOS dependencies" "compatible compiler, libc++ and SDK headers"
            "The NAN/INFINITY/<cfloat> compile-link probe failed (the contract HarfBuzz and Catch2 need). Compiler: '${CMAKE_CXX_COMPILER}'; SDK: '${CMAKE_OSX_SYSROOT}'. See CMakeFiles/CMakeConfigureLog.yaml for compiler output."
            "${setup_recovery}\n  Init tests installed SDKs before building dependencies. If SDKROOT is explicitly set, unset it or select a compatible installed SDK. A deployment-target change alone does not change SDK headers.")
    endif()
endif()
