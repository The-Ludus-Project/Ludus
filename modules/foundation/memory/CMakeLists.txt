add_library(ludus_foundation_memory STATIC)
add_library(Ludus::FoundationMemory ALIAS ludus_foundation_memory)
set_target_properties(ludus_foundation_memory PROPERTIES OUTPUT_NAME ludus_foundation_memory EXPORT_NAME FoundationMemory)
target_sources(ludus_foundation_memory
    PRIVATE
        src/allocation_domain.cpp
        src/shared_controller.cpp
    PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
        include/ludus/foundation/memory/allocation_domain.hpp
        include/ludus/foundation/memory/owned_object.hpp
        include/ludus/foundation/memory/shared_ptr.hpp
        include/ludus/foundation/memory/detail/object_layout.hpp
        include/ludus/foundation/memory/detail/shared_controller.hpp
)
target_link_libraries(ludus_foundation_memory PUBLIC Ludus::FoundationBase)
ludus_apply_project_defaults(ludus_foundation_memory)
install(TARGETS ludus_foundation_memory EXPORT LudusTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_foundation_memory_tests
        tests/owned_object_tests.cpp tests/recording_domain.hpp tests/shared_ptr_tests.cpp)
    # Reduced-width saturation tests instantiate the private count transitions.
    target_include_directories(ludus_foundation_memory_tests PRIVATE src)
    target_link_libraries(ludus_foundation_memory_tests
        PRIVATE Ludus::FoundationMemory Ludus::FoundationContainers Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_foundation_memory_tests)
    ludus_enable_test_exceptions(ludus_foundation_memory_tests)
    add_test(NAME ludus_foundation_memory_tests COMMAND ludus_foundation_memory_tests)
    set_tests_properties(ludus_foundation_memory_tests PROPERTIES LABELS "unit;foundation;memory" TIMEOUT 60)
endif()
