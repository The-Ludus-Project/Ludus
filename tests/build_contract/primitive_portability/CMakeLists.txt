cmake_minimum_required(VERSION 3.29)
project(LudusPrimitivePortability LANGUAGES CXX)

# This isolated D0 probe deliberately does not admit the engine, SDK or Behavior
# compiler profiles. It reuses the production headers/representation checks and
# the existing primitive tests with a compiler-native Catch2 dependency.
get_filename_component(ludus_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES Debug RelWithDebInfo CACHE STRING "Probe configurations" FORCE)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$" OR
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.43)
        message(FATAL_ERROR "D0 primitive probe requires an x64 MSVC toolset with /std:c++23preview")
    endif()
    # Thanks to Microsoft, "/std (Specify Language Standard Version)":
    # https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version
    # CMake 3.29 maps cxx_std_23 to c++latest, which also admits later drafts.
    # The candidate probe uses the C++23-only mode explicitly; this is not a
    # declaration of complete C++23 support or a qualified engine toolset pin.
    set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23preview")
    set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++23preview")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

include(CTest)
find_package(Catch2 3 REQUIRED CONFIG)
add_library(ludus_primitive_representation OBJECT "${ludus_root}/modules/foundation/base/src/types.cpp")
add_executable(ludus_primitive_portability
    "${ludus_root}/modules/foundation/base/tests/checked_integer_tests.cpp"
    "${ludus_root}/modules/foundation/base/tests/byte_order_tests.cpp"
    compiler_tests.cpp
    $<TARGET_OBJECTS:ludus_primitive_representation>)
target_link_libraries(ludus_primitive_portability PRIVATE Catch2::Catch2WithMain)
foreach(target ludus_primitive_representation ludus_primitive_portability)
    target_include_directories(${target} PRIVATE "${ludus_root}/modules/foundation/base/include")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(ludus_primitive_representation PRIVATE /EHs-c-)
    target_compile_options(ludus_primitive_portability PRIVATE /EHsc)
else()
    target_compile_options(ludus_primitive_representation PRIVATE -fno-exceptions)
    target_compile_options(ludus_primitive_portability PRIVATE -fexceptions)
endif()
add_test(NAME ludus_primitive_portability COMMAND ludus_primitive_portability)

# Preserve the actual candidate identity beside each generated build tree.
file(WRITE "${CMAKE_BINARY_DIR}/compiler-identity.txt"
    "compiler=${CMAKE_CXX_COMPILER}\nversion=${CMAKE_CXX_COMPILER_VERSION}\n"
    "generator=${CMAKE_GENERATOR}\nplatform=${CMAKE_GENERATOR_PLATFORM}\n"
    "toolset=${CMAKE_VS_PLATFORM_TOOLSET_VERSION}\nsdk=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}\n"
    "cxx23=${CMAKE_CXX23_STANDARD_COMPILE_OPTION}\ncrt=${CMAKE_MSVC_RUNTIME_LIBRARY}\n"
    "environment-toolset=$ENV{VCToolsVersion}\nenvironment-sdk=$ENV{WindowsSDKVersion}\n")
