cmake_minimum_required(VERSION 3.29)
project(LudusWindowsSdkConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23preview")
string(REGEX REPLACE "(^| )[/-]EHsc( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "(^| )[/-]W[0-4]( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

find_package(Ludus CONFIG REQUIRED COMPONENTS FoundationBase FoundationFilesystem GraphicsRhi Platform
    PATHS "${CMAKE_PREFIX_PATH}" NO_DEFAULT_PATH)
if(NOT Ludus_BUILD_FLAVOR STREQUAL EXPECTED_FLAVOR OR
   NOT Ludus_SDK_CONFIGURATION STREQUAL EXPECTED_FLAVOR)
    message(FATAL_ERROR "Expected the ${EXPECTED_FLAVOR} SDK, found ${Ludus_BUILD_FLAVOR} / ${Ludus_SDK_CONFIGURATION}.")
endif()

file(READ "${Ludus_SDK_MANIFEST}" manifest)
foreach(pair "build_flavor;${EXPECTED_FLAVOR}" "build_type;${EXPECTED_FLAVOR}" "sdk_variant;${Ludus_SDK_VARIANT}"
        "build_flavor_id;${Ludus_BUILD_FLAVOR_ID}" "enable_asserts;${Ludus_ENABLE_ASSERTS}")
    list(GET pair 0 key)
    list(GET pair 1 expected)
    string(JSON actual GET "${manifest}" "${key}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "SDK manifest ${key} is '${actual}', expected '${expected}'.")
    endif()
endforeach()

string(TOUPPER "${EXPECTED_FLAVOR}" sdk_configuration)
get_target_property(archive Ludus::GraphicsRhi "IMPORTED_LOCATION_${sdk_configuration}")
cmake_path(IS_PREFIX CMAKE_PREFIX_PATH "${archive}" NORMALIZE is_in_prefix)
if(NOT is_in_prefix OR NOT EXISTS "${archive}")
    message(FATAL_ERROR "The exported ${EXPECTED_FLAVOR} RHI archive is outside the installed SDK: ${archive}")
endif()

add_executable(ludus_windows_sdk_smoke smoke.cpp)
target_link_libraries(ludus_windows_sdk_smoke PRIVATE Ludus::FoundationFilesystem Ludus::GraphicsRhi Ludus::Platform)
ludus_apply_app_policy(ludus_windows_sdk_smoke)
target_compile_options(ludus_windows_sdk_smoke PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor)
target_compile_definitions(ludus_windows_sdk_smoke PRIVATE
    LUDUS_EXPECTED_SDK_FLAVOR_ID=${Ludus_BUILD_FLAVOR_ID}
    LUDUS_EXPECTED_SDK_ASSERTS=${Ludus_ENABLE_ASSERTS})
ludus_compile_shader(TARGET ludus_windows_sdk_smoke NAME diagnostic
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/diagnostic.slang" VERTEX VertexMain FRAGMENT FragmentMain)

enable_testing()
add_test(NAME ludus_windows_sdk_smoke COMMAND ludus_windows_sdk_smoke)
set_tests_properties(ludus_windows_sdk_smoke PROPERTIES TIMEOUT 30)
