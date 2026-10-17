function(ludus_configure_target_identity target_name visibility)
    # Validate compiler detection against the configured target, not the host.
    # FoundationBase exports this guard without exporting private build options.
    # Reference: Kitware, "CMAKE_SYSTEM_NAME" (target system versus host system):
    # https://cmake.org/cmake/help/latest/variable/CMAKE_SYSTEM_NAME.html
    if(EMSCRIPTEN)
        set(ludus_target_os WEB)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(ludus_target_os WINDOWS)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(ludus_target_os MACOS)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
        set(ludus_target_os ANDROID)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(ludus_target_os IOS)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(ludus_target_os LINUX)
    else()
        message(FATAL_ERROR "Ludus target detection does not recognize CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}'")
    endif()
    target_compile_definitions(${target_name} ${visibility} LUDUS_EXPECTED_TARGET_OS=LUDUS_OS_${ludus_target_os})
endfunction()

function(ludus_configure_project_options target_name)
    add_library(${target_name} INTERFACE)

    target_compile_features(${target_name} INTERFACE cxx_std_23)

    target_compile_definitions(${target_name} INTERFACE
        LUDUS_BUILD_${LUDUS_BUILD_FLAVOR_DEFINE}=1
    )
    ludus_configure_target_identity(${target_name} INTERFACE)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_link_options(${target_name} INTERFACE -fuse-ld=lld)
        # Map diagnostic literals without breaking DWARF's source/comp-dir pair.
        target_compile_options(${target_name} INTERFACE "-fmacro-prefix-map=${PROJECT_SOURCE_DIR}/=")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        # The Microsoft CRT deprecates portable ISO C calls such as fopen in favour
        # of its non-portable Annex K variants. Ludus bounds and checks those calls
        # explicitly, so keep the portable spelling warning-clean on Windows.
        target_compile_definitions(${target_name} INTERFACE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(EMSCRIPTEN)
        # Keep __FILE__/source_location diagnostics portable in shipped wasm.
        target_compile_options(${target_name} INTERFACE "-fmacro-prefix-map=${PROJECT_SOURCE_DIR}/=")
    endif()

    # Ludus does not use C++ exceptions (see AGENTS.md and the steering rule
    # "no-exceptions"). Compile them out entirely so error handling stays
    # explicit (status/optional/return values) and so any accidental throw /
    # try / catch fails to compile rather than slipping through review. Test
    # executables re-enable exceptions via ludus_enable_test_exceptions()
    # because Catch2 reports failures by throwing.
    #
    # The exception flag is gated on the per-target LUDUS_TEST_EXCEPTIONS
    # property rather than emitted unconditionally. Emitting both
    # `-fexceptions` (from the test helper) and `-fno-exceptions` (from this
    # interface) on the same command line let the *last* flag win, and CMake
    # orders linked-interface options after a target's own options, so
    # `-fno-exceptions` silently won even in test targets (the helper's
    # documented "last -f wins" intent did not hold). Gating removes the flag
    # entirely for opted-in test targets, so there is no conflicting pair and
    # the result is order-independent. See EngineTargets.cmake.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        target_compile_options(${target_name} INTERFACE
            $<$<NOT:$<BOOL:$<TARGET_PROPERTY:LUDUS_TEST_EXCEPTIONS>>>:-fno-exceptions>)
    elseif(MSVC)
        target_compile_options(${target_name} INTERFACE
            $<$<NOT:$<BOOL:$<TARGET_PROPERTY:LUDUS_TEST_EXCEPTIONS>>>:/EHs-c->)
    endif()

    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${target_name} INTERFACE $<$<CXX_COMPILER_ID:Clang,GNU>:-UNDEBUG>)
    endif()

    if(LUDUS_ENABLE_COVERAGE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target_name} INTERFACE -O0 -g --coverage)
            target_link_options(${target_name} INTERFACE --coverage)
        else()
            message(FATAL_ERROR "LUDUS_ENABLE_COVERAGE is only supported for Clang and GCC in Milestone 0")
        endif()
    endif()

    # Build-time profiling. -ftime-trace makes Clang emit a per-translation-unit
    # JSON flame graph (next to each .o) describing where compile time went:
    # headers parsed, template instantiations, constexpr evaluation. The traces
    # are aggregated by scripts/profile-build (see ClangBuildAnalyzer). This is a
    # diagnostic build mode only; leave it OFF for normal builds.
    if(LUDUS_ENABLE_TIME_TRACE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            target_compile_options(${target_name} INTERFACE -ftime-trace)
        else()
            message(FATAL_ERROR "LUDUS_ENABLE_TIME_TRACE requires Clang")
        endif()
    endif()
endfunction()

# Read the LLVM major pinned by config/tool_versions.json (minimum.clang) so
# compiler gates follow the single toolchain pin instead of a literal.
function(ludus_get_pinned_llvm_major output)
    set(pin "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../config/tool_versions.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pin}")
    file(READ "${pin}" document)
    string(JSON clang ERROR_VARIABLE pin_error GET "${document}" minimum clang)
    if(pin_error OR NOT clang MATCHES "^([0-9]+)\\.")
        message(FATAL_ERROR "config/tool_versions.json must pin minimum.clang as a version")
    endif()
    set(${output} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
