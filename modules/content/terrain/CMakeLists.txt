# Ludus::ContentTerrain: canonical tiled surface (T0) and serial reference kernels of terrain slice T1
# (docs/architecture/terrain-generation.md#ordered-pipeline): the addressed
# gradient-noise elevation recipe, derived slope/drainage/wetness fields and
# material rules with bounded patch palettes, and the T2 river channel network
# and carving (terrain_rivers.h). Foundation-only, so it builds for
# native and browser targets. In-tree only until an SDK consumer needs it.
add_library(ludus_content_terrain STATIC
    src/terrain_tile.cpp
    src/terrain_surface.cpp
    src/terrain_noise.cpp
    src/terrain_fields.cpp
    src/terrain_materials.cpp
    src/terrain_rivers.cpp
)
add_library(Ludus::ContentTerrain ALIAS ludus_content_terrain)
target_sources(ludus_content_terrain PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES
        include/ludus/content/terrain/terrain_tile.h
        include/ludus/content/terrain/terrain_surface.h
        include/ludus/content/terrain/terrain_noise.h
        include/ludus/content/terrain/terrain_fields.h
        include/ludus/content/terrain/terrain_materials.h
        include/ludus/content/terrain/terrain_rivers.h
)
target_link_libraries(ludus_content_terrain
    PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationMath)
ludus_apply_project_defaults(ludus_content_terrain)
# Keep the canonical decode reproducible and opposite ray edge functions antisymmetric.
set_property(SOURCE src/terrain_tile.cpp src/terrain_surface.cpp APPEND PROPERTY COMPILE_OPTIONS
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)

if(LUDUS_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_content_terrain_tests
        tests/terrain_tile_tests.cpp
        tests/terrain_noise_tests.cpp
        tests/terrain_fields_tests.cpp
        tests/terrain_materials_tests.cpp
        tests/terrain_rivers_tests.cpp
    )
    target_link_libraries(ludus_content_terrain_tests
        PRIVATE Ludus::ContentTerrain Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_content_terrain_tests)
    ludus_enable_test_exceptions(ludus_content_terrain_tests)
    add_test(NAME ludus_content_terrain_tests COMMAND ludus_content_terrain_tests)
    set_tests_properties(ludus_content_terrain_tests PROPERTIES LABELS "unit;content;terrain" TIMEOUT 120)
endif()
