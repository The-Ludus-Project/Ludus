# Only Crashpad is built from Sentry's complete, checksummed source distribution.
# No Sentry SDK or network transport is linked into the editor/engine.
include_guard(GLOBAL)
include(FetchContent)

function(ludus_editor_add_crash_capture)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../config/editor_crash_reporting.json")
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../config/editor_crash_reporting.json" pin)
    string(JSON source_url GET "${pin}" source_url)
    string(JSON source_sha256 GET "${pin}" source_sha256)
    FetchContent_Declare(ludus_crashpad_source URL "${source_url}"
        URL_HASH "SHA256=${source_sha256}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(ludus_crashpad_source)
    if(NOT ludus_crashpad_source_POPULATED)
        FetchContent_Populate(ludus_crashpad_source)
    endif()
    set(ludus_crashpad_source_SOURCE_DIR "${ludus_crashpad_source_SOURCE_DIR}" PARENT_SCOPE)
    set(CRASHPAD_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(CRASHPAD_ENABLE_INSTALL_DEV OFF CACHE BOOL "" FORCE)
    # Upstream's older policy baseline predates Apple linker's library deduplication.
    set(CMAKE_POLICY_DEFAULT_CMP0156 NEW)
    add_subdirectory("${ludus_crashpad_source_SOURCE_DIR}/external/crashpad"
        "${CMAKE_CURRENT_BINARY_DIR}/crashpad" EXCLUDE_FROM_ALL)
    # Treat upstream includes as system headers, without hiding first-party warnings.
    get_target_property(crashpad_includes crashpad_interface INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET crashpad_interface PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${crashpad_includes}")
    add_library(ludus_editor_crash_capture STATIC
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/crash_capture_macos.mm")
    target_include_directories(ludus_editor_crash_capture PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src")
    target_link_libraries(ludus_editor_crash_capture PRIVATE
        crashpad::client crashpad::interface Ludus::FoundationBase "-framework Foundation")
    ludus_apply_project_defaults(ludus_editor_crash_capture)
endfunction()

function(ludus_editor_bundle_crash_capture target)
    target_link_libraries(${target} PRIVATE ludus_editor_crash_capture)
    add_dependencies(${target} crashpad_handler)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:crashpad_handler>"
            "$<TARGET_FILE_DIR:${target}>/ludus_crash_handler"
        VERBATIM)
    # Preserve the upstream notices, including mini_chromium's ICU routines and
    # the mpack library linked by the handler, plus Sentry's CMake integration.
    foreach(notice
        "external/crashpad/LICENSE"
        "external/crashpad/third_party/mini_chromium/mini_chromium/LICENSE"
        "external/crashpad/third_party/mini_chromium/mini_chromium/base/third_party/icu/LICENSE"
        "external/crashpad/third_party/mpack/LICENSE"
        "LICENSE")
        string(REPLACE "/" "-" notice_name "${notice}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/../Resources/crash-reporting-licenses"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ludus_crashpad_source_SOURCE_DIR}/${notice}"
                "$<TARGET_FILE_DIR:${target}>/../Resources/crash-reporting-licenses/${notice_name}.txt"
            VERBATIM)
    endforeach()
endfunction()
