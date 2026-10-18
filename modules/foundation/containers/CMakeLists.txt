add_library(ludus_foundation_containers STATIC)
add_library(Ludus::FoundationContainers ALIAS ludus_foundation_containers)

set_target_properties(
    ludus_foundation_containers
    PROPERTIES
        OUTPUT_NAME ludus_foundation_containers
        EXPORT_NAME FoundationContainers
)

target_sources(
    ludus_foundation_containers
    PRIVATE
        src/array_support.cpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS
            include
        FILES
            include/ludus/foundation/containers/borrowed_view.hpp
            include/ludus/foundation/containers/static_array.hpp
            include/ludus/foundation/containers/static_bit_set.hpp
            include/ludus/foundation/containers/fixed_array.hpp
            include/ludus/foundation/containers/ring_buffer.hpp
            include/ludus/foundation/containers/stable_sort.hpp
            include/ludus/foundation/containers/sort.hpp
            include/ludus/foundation/containers/array.hpp
            include/ludus/foundation/containers/sorted_map.hpp
            include/ludus/foundation/containers/relocation.hpp
            include/ludus/foundation/containers/containers.hpp
            include/ludus/foundation/containers/detail/contiguous_storage.hpp
            include/ludus/foundation/containers/detail/inline_slots.hpp
)

# Dependency direction: FoundationContainers may use FoundationBase (types,
# assertions, compiler macros) and FoundationMemory (the Array storage domain),
# never the reverse. Memory stays private: no public container header names it.
target_link_libraries(
    ludus_foundation_containers
    PUBLIC
        Ludus::FoundationBase
    PRIVATE
        Ludus::FoundationMemory
)

ludus_apply_project_defaults(ludus_foundation_containers)

install(
    TARGETS ludus_foundation_containers
    EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    find_package(Threads REQUIRED)

    add_executable(ludus_foundation_containers_tests)
    target_sources(
        ludus_foundation_containers_tests
        PRIVATE
            tests/borrowed_view_tests.cpp
            tests/static_array_tests.cpp
            tests/fixed_array_tests.cpp
            tests/ring_buffer_tests.cpp
            tests/static_bit_set_tests.cpp
            tests/stable_sort_tests.cpp
            tests/sort_tests.cpp
            tests/array_tests.cpp
            tests/sorted_map_tests.cpp
            tests/relocation_tests.cpp
            tests/array_accounting_tests.cpp
    )
    target_include_directories(ludus_foundation_containers_tests PRIVATE tests)
    target_link_libraries(
        ludus_foundation_containers_tests
        PRIVATE
            Ludus::FoundationContainers
            Ludus::FoundationMemory
            Catch2::Catch2WithMain
            Threads::Threads
    )
    ludus_apply_project_defaults(ludus_foundation_containers_tests)
    ludus_enable_test_exceptions(ludus_foundation_containers_tests)

    add_test(NAME ludus_foundation_containers_tests COMMAND ludus_foundation_containers_tests)
    set_tests_properties(ludus_foundation_containers_tests PROPERTIES LABELS "unit;foundation;containers")

    # Zero-allocation / allocation-count assertions reuse the malloc-wrap pattern
    # used by the assertion tests: the container's operator-new path is observed
    # by wrapping the C allocator underneath. Runs only when not sanitized (the
    # sanitizer runtime intercepts these symbols itself).
    if(NOT LUDUS_ENABLE_ASAN AND NOT LUDUS_ENABLE_UBSAN AND NOT LUDUS_ENABLE_TSAN)
        add_executable(ludus_containers_alloc_tests)
        target_sources(ludus_containers_alloc_tests PRIVATE tests/allocation_tests.cpp)
        target_include_directories(ludus_containers_alloc_tests PRIVATE tests)
        target_link_libraries(
            ludus_containers_alloc_tests
            PRIVATE
                Ludus::FoundationContainers
                Catch2::Catch2WithMain
        )
        ludus_apply_project_defaults(ludus_containers_alloc_tests)
        ludus_enable_test_exceptions(ludus_containers_alloc_tests)
        add_test(NAME ludus_containers_alloc_tests COMMAND ludus_containers_alloc_tests)
        set_tests_properties(ludus_containers_alloc_tests PROPERTIES LABELS "unit;foundation;containers;alloc")
    endif()
endif()
