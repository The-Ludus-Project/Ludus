# Ludus::TerrainFormat: header-only LUTRN/LUTPG cooked terrain revision contract shared by
# the ContentTerrain cooker and reader (owned by neither tools nor runtime). Terrain
# generation architecture, "Cooked format and validation". Depends only on FoundationBase.
# In-tree only, like Ludus::ContentTerrain, until an SDK consumer needs it.
add_library(ludus_terrain_format INTERFACE)
add_library(Ludus::TerrainFormat ALIAS ludus_terrain_format)
set_target_properties(ludus_terrain_format PROPERTIES EXPORT_NAME TerrainFormat)
target_sources(ludus_terrain_format INTERFACE FILE_SET public_headers TYPE HEADERS BASE_DIRS include
    FILES include/ludus/content/terrain_format/terrain_format.h include/ludus/content/terrain_format/limits.h)
target_link_libraries(ludus_terrain_format INTERFACE Ludus::FoundationBase)
