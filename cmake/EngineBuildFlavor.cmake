# Build flavor is engine policy; CMake configuration controls optimization and
# dependency ABI. The Visual Studio source build names all four flavors;
# Development and Profile use RelWithDebInfo compiler and dependency settings.
set(LUDUS_BUILD_FLAVOR "" CACHE STRING "Ludus flavor: Debug, Development, Profile, Release")
set(LUDUS_BUILD_FLAVORS Debug Development Profile Release)
set_property(CACHE LUDUS_BUILD_FLAVOR PROPERTY STRINGS ${LUDUS_BUILD_FLAVORS})

function(ludus_get_build_flavor_id flavor output)
    list(FIND LUDUS_BUILD_FLAVORS "${flavor}" index)
    if(index LESS 0)
        message(FATAL_ERROR "Unknown Ludus build flavor '${flavor}'")
    endif()
    math(EXPR id "${index} + 1")
    set(${output} "${id}" PARENT_SCOPE)
endfunction()

function(ludus_format_sdk_variant output)
    set(variant "assert-v${LUDUS_ASSERT_POLICY_VERSION}-${LUDUS_BUILD_FLAVOR}-${CMAKE_BUILD_TYPE}-dialogs-${LUDUS_ASSERT_DIALOGS_AVAILABLE}-asan-${LUDUS_ENABLE_ASAN}-ubsan-${LUDUS_ENABLE_UBSAN}-tsan-${LUDUS_ENABLE_TSAN}")
    if(EMSCRIPTEN)
        string(APPEND variant "-web-wasm32")
    endif()
    set(${output} "${variant}" PARENT_SCOPE)
endfunction()

set(LUDUS_VISUAL_STUDIO_MULTI_CONFIG OFF)
if(CMAKE_CONFIGURATION_TYPES)
    if(NOT (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_GENERATOR MATCHES "^Visual Studio"))
        message(FATAL_ERROR "Ludus SDK variants currently require a single-config generator (Ninja)")
    endif()

    if(LUDUS_BUILD_FLAVOR STREQUAL "")
        set(LUDUS_VISUAL_STUDIO_MULTI_CONFIG ON)
        set(CMAKE_CONFIGURATION_TYPES ${LUDUS_BUILD_FLAVORS})
    else()
        # Preserve existing solutions explicitly configured for a single flavor.
        if(LUDUS_BUILD_FLAVOR STREQUAL "Debug")
            set(CMAKE_BUILD_TYPE Debug)
        elseif(LUDUS_BUILD_FLAVOR MATCHES "^(Development|Profile)$")
            set(CMAKE_BUILD_TYPE RelWithDebInfo)
        elseif(LUDUS_BUILD_FLAVOR STREQUAL "Release")
            set(CMAKE_BUILD_TYPE Release)
        endif()
        set(CMAKE_CONFIGURATION_TYPES "${CMAKE_BUILD_TYPE}")
    endif()
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Ludus Visual Studio configurations" FORCE)
endif()

if(LUDUS_VISUAL_STUDIO_MULTI_CONFIG)
    set(LUDUS_BUILD_FLAVOR_ID "")
    foreach(flavor IN LISTS LUDUS_BUILD_FLAVORS)
        ludus_get_build_flavor_id("${flavor}" id)
        string(APPEND LUDUS_BUILD_FLAVOR_ID "$<$<CONFIG:${flavor}>:${id}>")
    endforeach()
elseif(LUDUS_BUILD_FLAVOR STREQUAL "Debug" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    ludus_get_build_flavor_id(Debug LUDUS_BUILD_FLAVOR_ID)
elseif(LUDUS_BUILD_FLAVOR STREQUAL "Development" AND CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    ludus_get_build_flavor_id(Development LUDUS_BUILD_FLAVOR_ID)
elseif(LUDUS_BUILD_FLAVOR STREQUAL "Profile" AND CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    ludus_get_build_flavor_id(Profile LUDUS_BUILD_FLAVOR_ID)
elseif(LUDUS_BUILD_FLAVOR STREQUAL "Release" AND CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
    ludus_get_build_flavor_id(Release LUDUS_BUILD_FLAVOR_ID)
else()
    message(FATAL_ERROR
        "Invalid Ludus flavor/configuration: '${LUDUS_BUILD_FLAVOR}' / '${CMAKE_BUILD_TYPE}'. "
        "Set LUDUS_BUILD_FLAVOR explicitly: Debug/Debug, Development or Profile/RelWithDebInfo, "
        "Release/Release or MinSizeRel.")
endif()

foreach(policy LUDUS_ENABLE_ASSERTS LUDUS_BREAK_ON_CHECK LUDUS_ASSERT_POLICY_VERSION LUDUS_ASSERT_DIALOGS_AVAILABLE)
    if(DEFINED ${policy})
        message(FATAL_ERROR "${policy} is generated from LUDUS_BUILD_FLAVOR; overrides are forbidden")
    endif()
endforeach()

# Bumped to 2: enabled ASSERT/ASSERT_F is now resumable through explicit
# developer action (debugger continue, or the external-helper Continue-once
# dialog) in eligible builds. Old headers/runtime assume ASSERT is terminal, so
# the SDK policy version changes to force a recompile and reject a mismatch.
set(LUDUS_ASSERT_POLICY_VERSION 2)
set(LUDUS_ENABLE_ASSERTS 0)
set(LUDUS_BREAK_ON_CHECK 0)
if(LUDUS_VISUAL_STUDIO_MULTI_CONFIG)
    set(LUDUS_ENABLE_ASSERTS "$<CONFIG:Debug,Development>")
    set(LUDUS_BREAK_ON_CHECK "${LUDUS_ENABLE_ASSERTS}")
elseif(LUDUS_BUILD_FLAVOR_ID LESS 3)
    set(LUDUS_ENABLE_ASSERTS 1)
    set(LUDUS_BREAK_ON_CHECK 1)
endif()

# Interactive-dialog capability. This is build-time ELIGIBILITY only, generated
# from the explicit flavor and the CI build setting; a runtime CI veto and the
# control-endpoint handshake gate whether a prompt actually appears. It can be 1
# only for a non-CI Debug build. CI is a build input here (defaulting from the CI
# environment) so a locally built dialog-capable binary is still marked eligible;
# the runtime veto stops it from prompting when later executed under CI.
set(LUDUS_CI_BUILD "$ENV{CI}" CACHE STRING "Set for a CI build; forces dialogs unavailable")
set(LUDUS_ASSERT_DIALOGS_AVAILABLE 0)
if(LUDUS_BUILD_FLAVOR_ID EQUAL 1 AND NOT LUDUS_CI_BUILD AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    set(LUDUS_ASSERT_DIALOGS_AVAILABLE 1)
endif()

if(LUDUS_VISUAL_STUDIO_MULTI_CONFIG)
    set(LUDUS_BUILD_FLAVOR_DEFINE "$<UPPER_CASE:$<CONFIG>>")
    # Install metadata is configured independently for each Windows flavor.
    return()
endif()

string(TOUPPER "${LUDUS_BUILD_FLAVOR}" LUDUS_BUILD_FLAVOR_DEFINE)
ludus_format_sdk_variant(LUDUS_SDK_VARIANT)
