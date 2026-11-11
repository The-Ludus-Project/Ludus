add_library(ludus_input STATIC)
add_library(Ludus::Input ALIAS ludus_input)

set_target_properties(
    ludus_input
    PROPERTIES
        OUTPUT_NAME ludus_input
        EXPORT_NAME Input
)

target_sources(
    ludus_input
    PRIVATE
        src/key.cpp
        src/keyboard.cpp
        src/input_debug.cpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS
            include
        FILES
            include/ludus/input/key.h
            include/ludus/input/keyboard_event.h
            include/ludus/input/keyboard.h
            include/ludus/input/actions.h
            include/ludus/input/input_debug.h
)

target_include_directories(
    ludus_input
    PRIVATE
        src
)

# Input publicly depends on FoundationBase for the fixed-width vocabulary and
# UniquePtr, and on FoundationMemory for the DomainDeleter of its reducer owner.
# FoundationContainers (StaticArray) and FoundationLogging are private
# implementation details and must not leak into the installed interface.
target_link_libraries(
    ludus_input
    PUBLIC
        Ludus::FoundationBase
        Ludus::FoundationMemory
    PRIVATE
        Ludus::FoundationContainers
        Ludus::FoundationLogging
)

ludus_apply_project_defaults(ludus_input)

install(
    TARGETS ludus_input
    EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)

    add_executable(ludus_input_tests)
    target_sources(
        ludus_input_tests
        PRIVATE
            tests/key_tests.cpp
            tests/reducer_tests.cpp
            tests/action_tests.cpp
            tests/trace_tests.cpp
    )
    # Tests reach into src/internal to instantiate the capacity-parameterized
    # reducer directly (tiny-capacity overflow / sequence-exhaustion coverage).
    target_include_directories(ludus_input_tests PRIVATE src include)
    target_link_libraries(
        ludus_input_tests
        PRIVATE
            Ludus::Input
            Ludus::FoundationContainers
            Catch2::Catch2WithMain
    )
    ludus_apply_project_defaults(ludus_input_tests)
    ludus_enable_test_exceptions(ludus_input_tests)
    add_test(NAME ludus_input_tests COMMAND ludus_input_tests)
    set_tests_properties(ludus_input_tests PROPERTIES LABELS "unit;input")

    # Zero-allocation assertion over repeated hot operations. Uses the same
    # malloc-wrap pattern as the containers alloc tests; excluded under sanitizers
    # which intercept the allocator themselves.
    if(NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
        add_executable(ludus_input_alloc_tests)
        target_sources(ludus_input_alloc_tests PRIVATE tests/allocation_tests.cpp)
        target_include_directories(ludus_input_alloc_tests PRIVATE src include)
        target_link_libraries(
            ludus_input_alloc_tests
            PRIVATE
                Ludus::Input
                Catch2::Catch2WithMain
        )
        ludus_apply_project_defaults(ludus_input_alloc_tests)
        ludus_enable_test_exceptions(ludus_input_alloc_tests)
        add_test(NAME ludus_input_alloc_tests COMMAND ludus_input_alloc_tests)
        set_tests_properties(ludus_input_alloc_tests PROPERTIES LABELS "unit;input;alloc")
    endif()

    # Performance-evidence harness (M4). Built with tests but NOT a ctest: it is
    # run manually to produce the numbers committed in the evidence doc. Excluded
    # under sanitizers (they skew timings and intercept the allocator).
    if(NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
        add_executable(ludus_input_bench benchmarks/input_bench.cpp)
        target_link_libraries(ludus_input_bench PRIVATE Ludus::FoundationTime Ludus::Input Ludus::FoundationContainers)
        ludus_apply_project_defaults(ludus_input_bench)
    endif()
endif()
