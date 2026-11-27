// Installed-SDK evidence for cooked terrain pages: the installed Ludus::ContentTerrain and Ludus::TerrainFormat cook
// the Editor's corpus terrain for each noise layout into the pinned revision, then read it back page by page from
// each page's own byte range. The Editor's Show cooked pages inspection writes the same manifest bytes
// (apps/editor/tests/world_terrain_cook_tests.cpp). See
// docs/modules/content/terrain.md#inspect-and-read-a-cooked-revision.
#include "terrain_readback_corpus.h"

bool ExerciseInstalledTerrainReadback() noexcept
{
    return ludus::content::terrain::testing::RunTerrainReadbackCorpus() == true;
}
