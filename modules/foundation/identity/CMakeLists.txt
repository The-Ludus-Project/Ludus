# FoundationIdentity: typed runtime handle values, persistent UUID values and
# codecs, generation advancement and the process lifetime-token source
# (docs/architecture/object-handles-uuid.md). Depends only on FoundationBase and
# builds for native and browser targets.
add_library(ludus_foundation_identity STATIC)
add_library(Ludus::FoundationIdentity ALIAS ludus_foundation_identity)
set_target_properties(ludus_foundation_identity PROPERTIES
    OUTPUT_NAME ludus_foundation_identity EXPORT_NAME FoundationIdentity)
target_sources(ludus_foundation_identity
    PRIVATE
        src/lifetime.cpp
        src/uuid.cpp
    PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include FILES
        include/ludus/foundation/identity/handle.hpp
        include/ludus/foundation/identity/lifetime.hpp
        include/ludus/foundation/identity/uuid.hpp
)
target_link_libraries(ludus_foundation_identity PUBLIC Ludus::FoundationBase)
ludus_apply_project_defaults(ludus_foundation_identity)
install(TARGETS ludus_foundation_identity EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    find_package(Threads REQUIRED)
    add_executable(ludus_foundation_identity_tests
        tests/handle_tests.cpp
        tests/lifetime_tests.cpp
        tests/uuid_tests.cpp)
    # The lifetime tests drive a local counter through the private issuance seam.
    target_include_directories(ludus_foundation_identity_tests PRIVATE src)
    target_link_libraries(ludus_foundation_identity_tests PRIVATE Ludus::FoundationThreading
        Ludus::FoundationIdentity Catch2::Catch2WithMain Threads::Threads)
    ludus_apply_project_defaults(ludus_foundation_identity_tests)
    ludus_enable_test_exceptions(ludus_foundation_identity_tests)
    add_test(NAME ludus_foundation_identity_tests COMMAND ludus_foundation_identity_tests)
    set_tests_properties(ludus_foundation_identity_tests PROPERTIES LABELS "unit;foundation;identity" TIMEOUT 60)
endif()
