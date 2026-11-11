# Ludus::ContentTerrain: headless serial reference kernels of terrain slice T1
# (docs/architecture/terrain-generation.md#ordered-pipeline): the addressed
# gradient-noise elevation recipe, derived slope/drainage/wetness fields and
# material rules with bounded patch palettes. Foundation-only, so it builds for
# native and browser targets. In-tree only until an SDK consumer needs it.
add_library(ludus_content_terrain STATIC
    src/terrain_noise.cpp
    src/terrain_fields.cpp
    src/terrain_materials.cpp
)
add_library(Ludus::ContentTerrain ALIAS ludus_content_terrain)
target_sources(ludus_content_terrain PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES
        include/ludus/content/terrain/terrain_noise.h
        include/ludus/content/terrain/terrain_fields.h
        include/ludus/content/terrain/terrain_materials.h
)
target_link_libraries(ludus_content_terrain
    PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationMath)
ludus_apply_project_defaults(ludus_content_terrain)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_content_terrain_tests
        tests/terrain_noise_tests.cpp
        tests/terrain_fields_tests.cpp
        tests/terrain_materials_tests.cpp
    )
    target_link_libraries(ludus_content_terrain_tests
        PRIVATE Ludus::ContentTerrain Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_content_terrain_tests)
    ludus_enable_test_exceptions(ludus_content_terrain_tests)
    add_test(NAME ludus_content_terrain_tests COMMAND ludus_content_terrain_tests)
    set_tests_properties(ludus_content_terrain_tests PROPERTIES LABELS "unit;content;terrain" TIMEOUT 120)
endif()
