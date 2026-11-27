# Ludus::ContentTerrain: canonical tiled surface (T0) and serial reference kernels of terrain slice T1
# (docs/architecture/terrain-generation.md#ordered-pipeline): the addressed
# gradient-noise elevation recipe, derived slope/drainage/wetness fields and
# material rules with bounded patch palettes, and the T2 river channel network
# and carving (terrain_rivers.h), and versioned page cooking into the Ludus::TerrainFormat
# manifest and page pack (terrain_cook.h). Foundation-only, so it builds for
# native and browser targets. Installed in the SDK, so a consumer cooks and reads the same revision
# bytes as the Editor (tests/sdk_consumer/terrain_readback_corpus.h).
add_library(ludus_content_terrain STATIC
    src/terrain_tile.cpp
    src/terrain_surface.cpp
    src/terrain_noise.cpp
    src/terrain_fields.cpp
    src/terrain_materials.cpp
    src/terrain_rivers.cpp
    src/terrain_cook.cpp
)
add_library(Ludus::ContentTerrain ALIAS ludus_content_terrain)
set_target_properties(ludus_content_terrain PROPERTIES EXPORT_NAME ContentTerrain)
target_sources(ludus_content_terrain PUBLIC FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES
        include/ludus/content/terrain/terrain_tile.h
        include/ludus/content/terrain/terrain_surface.h
        include/ludus/content/terrain/terrain_noise.h
        include/ludus/content/terrain/terrain_fields.h
        include/ludus/content/terrain/terrain_materials.h
        include/ludus/content/terrain/terrain_rivers.h
        include/ludus/content/terrain/terrain_cook.h
)
target_link_libraries(ludus_content_terrain
    PUBLIC Ludus::FoundationBase Ludus::FoundationContainers Ludus::FoundationMath Ludus::FoundationHash
    PRIVATE Ludus::TerrainFormat)
ludus_apply_project_defaults(ludus_content_terrain)
# Keep the canonical decode reproducible and opposite ray edge functions antisymmetric.
set_property(SOURCE src/terrain_tile.cpp src/terrain_surface.cpp APPEND PROPERTY COMPILE_OPTIONS
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-ffp-contract=off>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)
install(TARGETS ludus_content_terrain EXPORT LudusTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The readback corpus lives beside the installed SDK consumer, which compiles it against the installed targets.
# The module tests and the Editor tests share this in-tree copy, so every build checks the same manifest digests.
if(LUDUS_BUILD_TESTS)
    add_library(ludus_terrain_readback_corpus OBJECT "${PROJECT_SOURCE_DIR}/tests/sdk_consumer/terrain_readback_corpus.cpp")
    target_include_directories(ludus_terrain_readback_corpus PUBLIC "${PROJECT_SOURCE_DIR}/tests/sdk_consumer")
    target_link_libraries(ludus_terrain_readback_corpus PUBLIC Ludus::ContentTerrain Ludus::FoundationHash)
    ludus_apply_project_defaults(ludus_terrain_readback_corpus)

    find_package(Catch2 3 REQUIRED CONFIG)
    add_executable(ludus_content_terrain_tests
        tests/terrain_tile_tests.cpp
        tests/terrain_noise_tests.cpp
        tests/terrain_fields_tests.cpp
        tests/terrain_materials_tests.cpp
        tests/terrain_rivers_tests.cpp
        tests/terrain_cook_tests.cpp
        tests/terrain_drainage_cook_tests.cpp
        tests/terrain_readback_tests.cpp
    )
    target_link_libraries(ludus_content_terrain_tests
        PRIVATE ludus_terrain_readback_corpus Ludus::ContentTerrain Ludus::TerrainFormat Catch2::Catch2WithMain)
    ludus_apply_project_defaults(ludus_content_terrain_tests)
    ludus_enable_test_exceptions(ludus_content_terrain_tests)
    add_test(NAME ludus_content_terrain_tests COMMAND ludus_content_terrain_tests)
    set_tests_properties(ludus_content_terrain_tests PROPERTIES LABELS "unit;content;terrain" TIMEOUT 120)
endif()
