# Ludus::FoliagePages encodes LUFOL foliage placement page sets for cookers and
# validates and reads them at runtime without allocating (foliage F0). The
# golden fixture is shared with the WebAssembly math probe.
add_library(ludus_foliage_pages STATIC src/foliage_pages.cpp)
add_library(Ludus::FoliagePages ALIAS ludus_foliage_pages)
set_target_properties(ludus_foliage_pages PROPERTIES EXPORT_NAME FoliagePages)
target_sources(ludus_foliage_pages PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/foliage_pages/foliage_pages.h)
target_link_libraries(ludus_foliage_pages
    PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoliageFormat)
ludus_apply_project_defaults(ludus_foliage_pages)

install(TARGETS ludus_foliage_pages EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_foliage_pages_tests tests/foliage_pages_tests.cpp)
    target_link_libraries(ludus_foliage_pages_tests PRIVATE Ludus::FoliagePages Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_foliage_pages_tests)
    ludus_enable_test_exceptions(ludus_foliage_pages_tests)
    add_test(NAME ludus_foliage_pages_tests COMMAND ludus_foliage_pages_tests)
    set_tests_properties(ludus_foliage_pages_tests PROPERTIES LABELS "unit;content;foliage" TIMEOUT 120)
endif()
