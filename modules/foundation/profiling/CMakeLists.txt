add_library(ludus_foundation_profiling STATIC)
add_library(Ludus::FoundationProfiling ALIAS ludus_foundation_profiling)

set_target_properties(
    ludus_foundation_profiling
    PROPERTIES
        OUTPUT_NAME ludus_foundation_profiling
        EXPORT_NAME FoundationProfiling
)

# CPU-trace instrumentation is compiled in for Debug/Development/Profile and
# compiled out for Release, matching the build-flavor policy (final design §20).
# LUDUS_PROFILING_ENABLED is SDK-owned and injected via the generated
# profiling_config.hpp so a consumer cannot silently disagree; predefining it is
# a hard error (see cmake/profiling_config.hpp.in). Flavor ids: 1=Debug,
# 2=Development, 3=Profile, 4=Release.
if(NOT DEFINED LUDUS_PROFILING_ENABLED)
    if(LUDUS_BUILD_FLAVOR_ID LESS 4)
        set(LUDUS_PROFILING_ENABLED 1)
    else()
        set(LUDUS_PROFILING_ENABLED 0)
    endif()
endif()
set(LUDUS_PROFILING_ENABLED "${LUDUS_PROFILING_ENABLED}" CACHE STRING
    "Compile CPU-trace instrumentation in (1) or out (0)")

set(LUDUS_FOUNDATION_PROFILING_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/profiling_config.hpp.in"
    "${LUDUS_FOUNDATION_PROFILING_GENERATED_INCLUDE_DIR}/ludus/foundation/profiling/profiling_config.hpp"
    @ONLY
    NEWLINE_STYLE UNIX
)

target_sources(ludus_foundation_profiling PRIVATE src/clock.cpp)

if(EMSCRIPTEN AND NOT LUDUS_PROFILING_ENABLED)
    target_sources(ludus_foundation_profiling PRIVATE src/profiling_disabled.cpp)
else()
    target_sources(ludus_foundation_profiling PRIVATE
        src/recorder.cpp src/profiling.cpp src/export_perfetto.cpp src/memory_gauges.cpp)
endif()

target_sources(
    ludus_foundation_profiling
    PRIVATE
        src/internal/trace_event.hpp
        src/internal/trace_chunk.hpp
        src/internal/recorder.hpp
        src/internal/memory_gauge.hpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS
            include
            "${LUDUS_FOUNDATION_PROFILING_GENERATED_INCLUDE_DIR}"
        FILES
            include/ludus/foundation/profiling/profiling.hpp
            include/ludus/foundation/profiling/clock.hpp
            include/ludus/foundation/profiling/zone.hpp
            include/ludus/foundation/profiling/trace_system.hpp
            include/ludus/foundation/profiling/integration.hpp
            include/ludus/foundation/profiling/memory_gauges.hpp
            "${LUDUS_FOUNDATION_PROFILING_GENERATED_INCLUDE_DIR}/ludus/foundation/profiling/profiling_config.hpp"
)

# Private include root so translation units can reference "internal/..." headers,
# matching the FoundationBase / FoundationLogging convention.
target_include_directories(
    ludus_foundation_profiling
    PRIVATE
        src
)

# Dependency direction (AGENTS.md): FoundationProfiling may use FoundationTime/Base
# (types, defines) and FoundationMemory (the aligned trace slab and the
# allocation-domain gauges of memory_gauges.hpp), never a
# higher-level module. It deliberately does NOT depend
# on FoundationLogging — it keeps its own thread identity so the ubiquitous
# instrumentation header stays minimal and the layering clean.
target_link_libraries(
    ludus_foundation_profiling
    PUBLIC
        Ludus::FoundationBase
        Ludus::FoundationContainers
    PRIVATE
        Ludus::FoundationMemory
        Ludus::FoundationTime
)

if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(ludus_foundation_profiling PUBLIC Threads::Threads)
endif()

ludus_apply_project_defaults(ludus_foundation_profiling)

install(
    TARGETS ludus_foundation_profiling
    EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)

    add_executable(ludus_foundation_profiling_tests)
    target_sources(
        ludus_foundation_profiling_tests
        PRIVATE
            tests/clock_tests.cpp
            tests/scope_tests.cpp
            tests/export_tests.cpp
            tests/recorder_tests.cpp
            tests/disabled_tests.cpp
            tests/memory_gauge_tests.cpp
    )
    target_link_libraries(
        ludus_foundation_profiling_tests
        PRIVATE
            Ludus::FoundationProfiling
            Ludus::FoundationMemory
            Ludus::FoundationTime
            Catch2::Catch2WithMain
    )
    # Tests reach internal headers to verify implementation-level contracts;
    # these headers are never installed.
    target_include_directories(ludus_foundation_profiling_tests PRIVATE src)

    ludus_apply_project_defaults(ludus_foundation_profiling_tests)
    ludus_enable_test_exceptions(ludus_foundation_profiling_tests)

    add_test(NAME ludus_foundation_profiling_tests COMMAND ludus_foundation_profiling_tests)
    set_tests_properties(ludus_foundation_profiling_tests PROPERTIES LABELS "unit;foundation;profiling")
endif()
