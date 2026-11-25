# Ludus::FoliageFormat: header-only LUFOL cooked foliage placement page contract
# shared by the placement cooker, the encoder and the validating runtime reader
# (owned by neither tools nor runtime). Foliage architecture, F0.
add_library(ludus_foliage_format INTERFACE)
add_library(Ludus::FoliageFormat ALIAS ludus_foliage_format)
set_target_properties(ludus_foliage_format PROPERTIES EXPORT_NAME FoliageFormat)
target_sources(ludus_foliage_format INTERFACE FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/foliage_format/lufol.h include/ludus/content/foliage_format/limits.h)
target_link_libraries(ludus_foliage_format INTERFACE Ludus::FoundationBase)
install(TARGETS ludus_foliage_format EXPORT LudusTargets
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_foliage_format_tests tests/foliage_format_tests.cpp)
    target_link_libraries(ludus_foliage_format_tests PRIVATE Ludus::FoliageFormat Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_foliage_format_tests)
    ludus_enable_test_exceptions(ludus_foliage_format_tests)
    add_test(NAME ludus_foliage_format_tests COMMAND ludus_foliage_format_tests)
    set_tests_properties(ludus_foliage_format_tests PROPERTIES LABELS "unit;content;foliage")
endif()
