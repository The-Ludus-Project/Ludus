cmake_minimum_required(VERSION 3.29)

project(LudusSdkConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Ludus CONFIG REQUIRED COMPONENTS Resources GameplayCamera FoundationReflection FoundationSerialization GamePropertyBinding Ui FoundationParsing FoundationParsingJson FoundationConfig FoundationConfigJson RuntimeConfiguration Localization StringDictionary StringDictionaryWriter OPTIONAL_COMPONENTS MissingUiTestComponent)
# Direct CMake exports and CLI/editor project admission must agree. A target can
# be installed successfully while missing from the identity manifest (#196), so
# every exported Ludus module must be listed. Bundled third-party libraries are
# exported only for the static link closure and are inventoried as dependencies.
set(_ludus_bundled_third_party_targets Ludus::ludus_miniaudio Ludus::StbImage Ludus::Yyjson)
file(READ "${Ludus_SDK_MANIFEST}" _ludus_manifest)
get_directory_property(_ludus_imported_targets IMPORTED_TARGETS)
foreach(_field components features)
    string(JSON _ludus_entry_count LENGTH "${_ludus_manifest}" "${_field}")
    set(_ludus_entries "")
    if(_ludus_entry_count GREATER 0)
        math(EXPR _ludus_last_entry "${_ludus_entry_count} - 1")
        foreach(_ludus_index RANGE ${_ludus_last_entry})
            string(JSON _ludus_entry GET "${_ludus_manifest}" "${_field}" ${_ludus_index})
            list(APPEND _ludus_entries "${_ludus_entry}")
        endforeach()
    endif()
    foreach(_ludus_target IN LISTS _ludus_imported_targets)
        if(NOT _ludus_target MATCHES "^Ludus::(.+)$" OR _ludus_target IN_LIST _ludus_bundled_third_party_targets)
            continue()
        endif()
        if(NOT CMAKE_MATCH_1 IN_LIST _ludus_entries)
            message(FATAL_ERROR "Installed ${_ludus_target} target is absent from SDK manifest ${_field}")
        endif()
    endforeach()
endforeach()
if(NOT "Ludus::Resources" IN_LIST _ludus_imported_targets)
    message(FATAL_ERROR "SDK manifest audit did not observe the imported Ludus export set")
endif()
unset(_ludus_bundled_third_party_targets)
unset(_ludus_manifest)
unset(_ludus_imported_targets)
unset(_ludus_entry_count)
unset(_ludus_last_entry)
unset(_ludus_entries)
if(Ludus_MissingUiTestComponent_FOUND)
    message(FATAL_ERROR "Unknown optional SDK component was reported as available")
endif()
# An unknown required component must fail for that precise reason, rather than
# being silently accepted or failing because of a missing compiler/dependency.
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${CMAKE_CURRENT_SOURCE_DIR}/components" -B "${CMAKE_CURRENT_BINARY_DIR}/missing-component"
    -G "${CMAKE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    "-DCMAKE_FIND_ROOT_PATH=${CMAKE_FIND_ROOT_PATH}"
    "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
    "-DLudus_DIR=${Ludus_DIR}"
    RESULT_VARIABLE _missing_result OUTPUT_VARIABLE _missing_output ERROR_VARIABLE _missing_error)
if(_missing_result EQUAL 0 OR NOT "${_missing_output}${_missing_error}" MATCHES
        "Requested Ludus SDK component 'MissingUiTestComponent' is unavailable")
    message(FATAL_ERROR "Required missing-component rejection failed: ${_missing_output}${_missing_error}")
endif()

ludus_generate_reflection(TARGET consumer_reflection SCHEMA reflection_record.schema.json)

if(NOT EMSCRIPTEN)
    # A relocated SDK must also support codecs inside a private gameplay module.
    add_library(ludus_sdk_reflection_module MODULE reflection_module.cpp)
    target_link_libraries(ludus_sdk_reflection_module PRIVATE consumer_reflection)
    ludus_apply_app_policy(ludus_sdk_reflection_module)
    set_target_properties(ludus_sdk_reflection_module PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(ludus_sdk_reflection_module PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
    add_executable(ludus_sdk_consumer main.cpp reflection.cpp network.cpp policy.cpp world.cpp strings.cpp string_dictionary.cpp time.cpp identity.cpp fluid.cpp filesystem.cpp pack.cpp async.cpp persistence.cpp ui.cpp containers.cpp threading.cpp parsing.cpp localization.cpp configuration.cpp curves.cpp geometry.cpp content.cpp audio.cpp camera.cpp denoiser.cpp)
    target_link_libraries(ludus_sdk_consumer PRIVATE consumer_reflection Ludus::AudioContent Ludus::Content Ludus::FoundationConfigJson Ludus::RuntimeConfiguration Ludus::NetworkCore Ludus::FoundationParsing Ludus::FoundationParsingJson Ludus::Localization Ludus::StringDictionary Ludus::StringDictionaryWriter Ludus::FoundationContainers Ludus::FoundationFilesystem Ludus::FoundationStrings Ludus::FoundationBase Ludus::FoundationMath Ludus::FoundationGeometry Ludus::FoundationThreading Ludus::FoundationTime Ludus::FoundationIdentity Ludus::Platform Ludus::GraphicsRhi Ludus::Input Ludus::GameplayWorld Ludus::GameplayCamera Ludus::PhysicsFluid Ludus::Ui Ludus::GraphicsRenderer)
    target_compile_definitions(ludus_sdk_consumer PRIVATE
        EXPECTED_DENOISER=$<BOOL:${Ludus_ENABLE_OIDN}>
        EXPECTED_FLAVOR=${Ludus_BUILD_FLAVOR_ID}
        EXPECTED_ASSERTS=${Ludus_ENABLE_ASSERTS}
        EXPECTED_CHECK_BREAK=${Ludus_BREAK_ON_CHECK})
endif()
option(LUDUS_SDK_RENDER_TESTS "Build generic public rendering consumer" OFF)
if(LUDUS_SDK_RENDER_TESTS)
    add_executable(ludus_sdk_render_consumer render.cpp)
    target_link_libraries(ludus_sdk_render_consumer PRIVATE Ludus::GraphicsRhi)
    target_compile_options(ludus_sdk_render_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
    ludus_compile_shader(TARGET ludus_sdk_render_consumer NAME diagnostic SOURCE shaders/diagnostic.slang
        VERTEX vertexMain FRAGMENT fragmentMain INCLUDES shaders)
    if(EMSCRIPTEN)
        target_link_options(ludus_sdk_render_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
        set_target_properties(ludus_sdk_render_consumer PROPERTIES OUTPUT_NAME render SUFFIX ".js")
        configure_file(render.html index.html COPYONLY)
    endif()
endif()

if(LUDUS_SDK_RENDER_TESTS)
    add_executable(ludus_sdk_raster_consumer raster.cpp)
    target_link_libraries(ludus_sdk_raster_consumer PRIVATE Ludus::GraphicsRhi Ludus::FoundationTime)
    target_compile_options(ludus_sdk_raster_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-exceptions)
    ludus_compile_shader(TARGET ludus_sdk_raster_consumer NAME cull SOURCE shaders/cull.slang COMPUTE computeMain)
    ludus_compile_shader(TARGET ludus_sdk_raster_consumer NAME raster RASTER SOURCE shaders/raster.slang
        VERTEX vertexMain FRAGMENT fragmentMain)
    if(EMSCRIPTEN)
        target_link_options(ludus_sdk_raster_consumer PRIVATE -fno-exceptions -g2 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1)
        set_target_properties(ludus_sdk_raster_consumer PROPERTIES OUTPUT_NAME raster SUFFIX ".js")
        configure_file(raster.html raster.html COPYONLY)
    endif()
endif()
