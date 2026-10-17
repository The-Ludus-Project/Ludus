include("${CMAKE_CURRENT_LIST_DIR}/LudusDependencies.cmake")

option(LUDUS_SANITIZER_RUNTIME_PROBE
    "Fail configuration when a trivial sanitized program cannot start on this host" ON)

# A sanitizer runtime that cannot initialize does not fail tests: it can hang
# every test process indefinitely. Build and run a trivial program with the
# selected sanitizers once per compiler/flags/host identity, bounded by a
# timeout, so an incompatible runtime stops configuration with a diagnosis.
#
# Thanks to Andrew Haberlandt (Apple), "[sanitizer_common] [Darwin] Adopt
# _dyld_get_dyld_header", llvm/llvm-project PR #182943 (rdar://167854578),
# backported to LLVM 22.1.3. Before it, compiler-rt's get_dyld_hdr() called
# dyld_shared_cache_iterate_text() during runtime initialization; on macOS 26
# that call allocates and uses dispatch_once, re-entering the uninitialized
# runtime (ASan deadlocks on its init mutex; TSan dereferences a null thread
# state). See docs/development/building.md, "macOS sanitizer runtimes".
# https://github.com/llvm/llvm-project/pull/182943
function(ludus_probe_sanitizer_runtime sanitizer_flags)
    if(NOT LUDUS_SANITIZER_RUNTIME_PROBE OR CMAKE_CROSSCOMPILING)
        return()
    endif()

    get_filename_component(compiler "${CMAKE_CXX_COMPILER}" REALPATH)
    set(identity "${compiler};${CMAKE_CXX_COMPILER_VERSION};${sanitizer_flags};${CMAKE_HOST_SYSTEM_NAME};${CMAKE_HOST_SYSTEM_VERSION}")
    if(LUDUS_SANITIZER_RUNTIME_PROBE_PASSED STREQUAL identity)
        return()
    endif()

    set(directory "${CMAKE_BINARY_DIR}/CMakeFiles/LudusSanitizerRuntimeProbe")
    set(source "${directory}/probe.cpp")
    set(executable "${directory}/probe${CMAKE_EXECUTABLE_SUFFIX}")
    file(WRITE "${source}" "int main() { delete[] new char[16]; return 0; }\n")
    file(REMOVE "${executable}")
    try_compile(compiled SOURCES "${source}"
        COMPILE_DEFINITIONS -fsanitize=${sanitizer_flags} -fno-omit-frame-pointer
        LINK_OPTIONS -fsanitize=${sanitizer_flags}
        OUTPUT_VARIABLE build_output
        COPY_FILE "${executable}"
        NO_CACHE)
    if(NOT compiled)
        ludus_dependency_error("Sanitizer configuration" "a compiler that can link -fsanitize=${sanitizer_flags}"
            "Compiler '${compiler}' (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}) could not build a trivial sanitized program:\n${build_output}"
            "Install the sanitizer runtime for this compiler, or select a non-sanitizer preset.")
    endif()

    # Ten seconds is orders of magnitude above a healthy runtime's startup
    # (milliseconds) and bounds a deadlocked one. CMake's own TIMEOUT does not
    # kill the child, so the bounded runner owns and kills its process group.
    set(probe_timeout_seconds 10)
    ludus_require_python("Sanitizer runtime probe")
    execute_process(COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/python/sanitizer_runtime_probe.py"
            --child "${executable}" --log "${directory}/probe.log" --timeout ${probe_timeout_seconds}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result STREQUAL "0")
        set(LUDUS_SANITIZER_RUNTIME_PROBE_PASSED "${identity}" CACHE INTERNAL
            "Compiler, sanitizers and host for which a sanitized program last started")
        return()
    endif()
    unset(LUDUS_SANITIZER_RUNTIME_PROBE_PASSED CACHE)

    set(cause "")
    if(CMAKE_HOST_APPLE AND CMAKE_HOST_SYSTEM_VERSION VERSION_GREATER_EQUAL 25
            AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 22.1.3)
        string(CONCAT cause
            "\nKnown cause: LLVM sanitizer runtimes before 22.1.3 hang (ASan) or crash (TSan) while "
            "initializing on macOS 26 and newer (Darwin ${CMAKE_HOST_SYSTEM_VERSION} here); fixed upstream "
            "by llvm/llvm-project PR #182943. Tests built with this runtime hang instead of failing.")
    endif()
    string(CONCAT detail
        "A trivial program built with -fsanitize=${sanitizer_flags} by '${compiler}' "
        "(${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}) did not start and exit cleanly:\n"
        "${output}${error}${cause}")
    string(CONCAT recovery
        "Validate sanitizers in CI or on an unaffected host; see docs/development/building.md, "
        "\"macOS sanitizer runtimes\".\n"
        "  Local onboarding can skip this preset: ./init.sh --validate --skip-sanitizers\n"
        "  Bypass this check only to investigate the runtime: -DLUDUS_SANITIZER_RUNTIME_PROBE=OFF")
    ludus_dependency_error("Sanitizer configuration" "a sanitizer runtime that starts on this host"
        "${detail}" "${recovery}")
endfunction()

function(ludus_configure_project_sanitizers target_name)
    if(LUDUS_ENABLE_ASAN AND LUDUS_ENABLE_TSAN)
        message(FATAL_ERROR "LUDUS_ENABLE_ASAN and LUDUS_ENABLE_TSAN cannot be enabled together")
    endif()

    if(LUDUS_ENABLE_TSAN AND LUDUS_ENABLE_COVERAGE)
        message(FATAL_ERROR "LUDUS_ENABLE_TSAN and LUDUS_ENABLE_COVERAGE cannot be enabled together")
    endif()

    set(enabled_sanitizers "")

    if(LUDUS_ENABLE_ASAN)
        list(APPEND enabled_sanitizers "address")
    endif()

    if(LUDUS_ENABLE_UBSAN)
        list(APPEND enabled_sanitizers "undefined")
        # Thanks to LLVM, Clang 23 "UndefinedBehaviorSanitizer", Usage: the
        # default recovers after most findings. A diagnostic must fail a test.
        # https://releases.llvm.org/23.1.0/tools/clang/docs/UndefinedBehaviorSanitizer.html
        target_compile_options(${target_name} INTERFACE -fno-sanitize-recover=undefined)
    endif()

    if(LUDUS_ENABLE_TSAN)
        list(APPEND enabled_sanitizers "thread")
    endif()

    if(enabled_sanitizers)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            message(FATAL_ERROR "Sanitizer presets require Clang or GCC")
        endif()

        list(JOIN enabled_sanitizers "," sanitizer_flags)
        ludus_probe_sanitizer_runtime("${sanitizer_flags}")
        target_compile_definitions(${target_name} INTERFACE LUDUS_BUILD_SANITIZED=1)
        target_compile_options(${target_name} INTERFACE -fsanitize=${sanitizer_flags} -fno-omit-frame-pointer)
        target_link_options(${target_name} INTERFACE -fsanitize=${sanitizer_flags} -fno-omit-frame-pointer)
    endif()
endfunction()
