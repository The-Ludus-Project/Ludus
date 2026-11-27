# Ludus::TerrainFormat: header-only LUTRN/LUTPG cooked terrain revision contract shared by
# the ContentTerrain cooker and reader (owned by neither tools nor runtime). Terrain
# generation architecture, "Cooked format and validation". Depends only on FoundationBase.
# Installed with Ludus::ContentTerrain so SDK consumers read cooked revisions.
add_library(ludus_terrain_format INTERFACE)
add_library(Ludus::TerrainFormat ALIAS ludus_terrain_format)
set_target_properties(ludus_terrain_format PROPERTIES EXPORT_NAME TerrainFormat)
target_sources(ludus_terrain_format INTERFACE FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/terrain_format/terrain_format.h include/ludus/content/terrain_format/limits.h)
target_link_libraries(ludus_terrain_format INTERFACE Ludus::FoundationBase)
install(TARGETS ludus_terrain_format EXPORT LudusTargets
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
