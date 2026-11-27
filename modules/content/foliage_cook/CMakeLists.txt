# Ludus::FoliageCook: bounded terrain foliage placement cook (foliage F0). It
# samples ContentTerrain's canonical quantized surface with exact fixed-point
# arithmetic, thins addressed lattice candidates by ordered strata and writes
# canonical LUFOL page sets through Ludus::FoliagePages. Foundation and content
# only, so it builds for native and browser targets. In-tree only until an
# SDK consumer needs it. The golden cook is shared with
# the WebAssembly math probe.
add_library(ludus_foliage_cook STATIC
    src/foliage_fixed_point.cpp
    src/foliage_terrain_sampler.cpp
    src/foliage_cook_validation.cpp
    src/foliage_cook.cpp
    src/foliage_page_writing.cpp
)
add_library(Ludus::FoliageCook ALIAS ludus_foliage_cook)
target_sources(ludus_foliage_cook PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/foliage_cook/foliage_cook.h)
target_link_libraries(ludus_foliage_cook
    PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationMath Ludus::ContentTerrain
        Ludus::FoliageFormat Ludus::FoliagePages
    PRIVATE Ludus::Content)
ludus_apply_project_defaults(ludus_foliage_cook)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_foliage_cook_tests tests/foliage_cook_tests.cpp tests/foliage_cook_fixed_point_tests.cpp)
    target_link_libraries(ludus_foliage_cook_tests
        PRIVATE Ludus::FoliageCook Ludus::FoundationThreading Catch2::Catch2WithMain)
    # The fixed-point tests exercise the private rounding helpers directly; the FNV digest helper is the
    # FoliagePages golden fixture's.
    target_include_directories(ludus_foliage_cook_tests PRIVATE src
        "${PROJECT_SOURCE_DIR}/modules/content/foliage_pages/tests")
    ludus_apply_project_defaults(ludus_foliage_cook_tests)
    ludus_enable_test_exceptions(ludus_foliage_cook_tests)
    add_test(NAME ludus_foliage_cook_tests COMMAND ludus_foliage_cook_tests)
    set_tests_properties(ludus_foliage_cook_tests PROPERTIES LABELS "unit;content;foliage" TIMEOUT 120)
endif()
