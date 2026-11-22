cmake_minimum_required(VERSION 3.29)
get_filename_component(ludus_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
file(READ "${ludus_root}/config/tool_versions.json" ludus_tool_pins)
string(JSON ludus_probe_cmake GET "${ludus_tool_pins}" windows_msvc_probe cmake)
if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND CMAKE_VERSION VERSION_LESS ludus_probe_cmake)
    # Thanks to Kitware, cmGlobalVisualStudio10Generator::SetGeneratorToolset:
    # https://github.com/Kitware/CMake/blob/v3.31.10/Source/cmGlobalVisualStudio10Generator.cxx
    # 3.29.6 rejects explicit 14.44 minor versions under the v143 toolset.
    message(FATAL_ERROR "The D0 Visual Studio probe requires CMake ${ludus_probe_cmake} from config/tool_versions.json (windows_msvc_probe.cmake)")
endif()
project(LudusPrimitivePortability LANGUAGES CXX)

# This isolated D0 probe deliberately does not admit the engine, SDK or Behavior
# compiler profiles. It reuses the production headers/representation checks and
# the existing primitive tests with a compiler-native Catch2 dependency.
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
    # CMake maps cxx_std_23 to c++latest, which also admits later drafts.
    # The candidate probe uses the C++23-only mode explicitly; this is not a
    # declaration of complete C++23 support or a qualified engine toolset pin.
    set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23preview")
    set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++23preview")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    # CMake adds /EHsc by default. Select one policy per target below instead
    # of overriding it with /EHs-c- (MSVC diagnoses that conflicting pair).
    string(REGEX REPLACE "(^| )[/-]EH[a-zA-Z0-9-]+( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

include(CTest)
find_package(Catch2 3 REQUIRED CONFIG)

# Production headers reach core.h (borrowed_view.hpp through byte_order.hpp), whose
# assert.hpp includes the SDK-generated assert_config.hpp. The probe links no
# assertion runtime, so it generates the Release policy (assertions compiled out)
# from the engine's template and policy version rather than admitting its flavors.
file(STRINGS "${ludus_root}/cmake/EngineBuildFlavor.cmake" ludus_policy_line
    REGEX "^set\\(LUDUS_ASSERT_POLICY_VERSION [0-9]+\\)$")
string(REGEX REPLACE "^set\\(LUDUS_ASSERT_POLICY_VERSION ([0-9]+)\\)$" "\\1" LUDUS_ASSERT_POLICY_VERSION "${ludus_policy_line}")
if(NOT LUDUS_ASSERT_POLICY_VERSION MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Cannot read LUDUS_ASSERT_POLICY_VERSION from cmake/EngineBuildFlavor.cmake")
endif()
set(LUDUS_BUILD_FLAVOR_ID 4) # Release: the fourth of EngineBuildFlavor.cmake's LUDUS_BUILD_FLAVORS.
set(LUDUS_ENABLE_ASSERTS 0)
set(LUDUS_BREAK_ON_CHECK 0)
set(LUDUS_ASSERT_DIALOGS_AVAILABLE 0)
set(ludus_probe_generated_include "${CMAKE_BINARY_DIR}/generated/include")
configure_file("${ludus_root}/cmake/assert_config.hpp.in"
    "${ludus_probe_generated_include}/ludus/foundation/base/assert_config.hpp" @ONLY NEWLINE_STYLE UNIX)
add_library(ludus_primitive_representation OBJECT "${ludus_root}/modules/foundation/base/src/types.cpp")
add_executable(ludus_primitive_portability
    "${ludus_root}/modules/foundation/base/tests/checked_integer_tests.cpp"
    "${ludus_root}/modules/foundation/base/tests/byte_order_tests.cpp"
    compiler_tests.cpp
    $<TARGET_OBJECTS:ludus_primitive_representation>)
target_link_libraries(ludus_primitive_portability PRIVATE Catch2::Catch2WithMain)
foreach(target ludus_primitive_representation ludus_primitive_portability)
    target_include_directories(${target} PRIVATE "${ludus_root}/modules/foundation/base/include"
        "${ludus_probe_generated_include}")
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
    "cmake=${CMAKE_VERSION}\ncompiler=${CMAKE_CXX_COMPILER}\nversion=${CMAKE_CXX_COMPILER_VERSION}\n"
    "generator=${CMAKE_GENERATOR}\nplatform=${CMAKE_GENERATOR_PLATFORM}\n"
    "toolset=${CMAKE_VS_PLATFORM_TOOLSET_VERSION}\nsdk=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}\n"
    "cxx23=${CMAKE_CXX23_STANDARD_COMPILE_OPTION}\ncrt=${CMAKE_MSVC_RUNTIME_LIBRARY}\n"
    "environment-toolset=$ENV{VCToolsVersion}\nenvironment-sdk=$ENV{WindowsSDKVersion}\n")
