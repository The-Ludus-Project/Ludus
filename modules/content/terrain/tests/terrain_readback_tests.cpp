// Source digests and the shared readback corpus (tests/sdk_consumer/terrain_readback_corpus.h): the revision the
// Editor cooks for its Show cooked pages inspection and the installed SDK consumer reads back page by page.

#include "terrain_readback_corpus.h"

#include <ludus/content/terrain/terrain_cook.h>
#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/content/terrain/terrain_noise.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/hash/hash.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace terrain = ludus::content::terrain;
namespace corpus = ludus::content::terrain::testing;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::Fingerprint128;
using ludus::foundation::float32;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using terrain::TerrainCookStatus;

constexpr Fingerprint128 SENTINEL_DIGEST{ .Low = 1, .High = 2 };

[[nodiscard]] Fingerprint128 GetFieldDigest(const corpus::TerrainReadbackField& field)
{
    Fingerprint128 digest = SENTINEL_DIGEST;
    REQUIRE(terrain::TryComputeTerrainFieldDigest(field.Grid, field.ElevationsMetres.AsView(), digest) ==
            TerrainCookStatus::Success);
    return digest;
}
} // namespace

TEST_CASE("Terrain field digests identify the grid and every elevation bit", "[content][terrain][cook]")
{
    corpus::TerrainReadbackField field;
    REQUIRE(corpus::TryBuildReadbackField(terrain::TerrainNoiseLayout::QuinticFade, field));
    const Fingerprint128 digest = GetFieldDigest(field);
    CHECK(GetFieldDigest(field) == digest);

    corpus::TerrainReadbackField changed;
    REQUIRE(corpus::TryBuildReadbackField(terrain::TerrainNoiseLayout::QuinticFade, changed));
    changed.ElevationsMetres[changed.ElevationsMetres.GetSize() / 2] = -changed.ElevationsMetres[0];
    CHECK(GetFieldDigest(changed) != digest);
    corpus::TerrainReadbackField respaced;
    REQUIRE(corpus::TryBuildReadbackField(terrain::TerrainNoiseLayout::QuinticFade, respaced));
    respaced.Grid.SpacingZMetres = respaced.Grid.SpacingZMetres * 2;
    CHECK(GetFieldDigest(respaced) != digest);

    Fingerprint128 preserved = SENTINEL_DIGEST;
    const BorrowedView<const float32> shortView{field.ElevationsMetres.GetData(), field.ElevationsMetres.GetSize() - 1};
    CHECK(terrain::TryComputeTerrainFieldDigest(field.Grid, shortView, preserved) == TerrainCookStatus::InvalidStorage);
    terrain::TerrainFieldGrid invalid = field.Grid;
    invalid.SpacingXMetres = 0;
    CHECK(terrain::TryComputeTerrainFieldDigest(invalid, field.ElevationsMetres.AsView(), preserved) ==
          TerrainCookStatus::InvalidArgument);
    CHECK(preserved == SENTINEL_DIGEST);
}

TEST_CASE("The terrain readback corpus pins one revision per noise layout", "[content][terrain][cook]")
{
    CHECK(corpus::RunTerrainReadbackCorpus());

    terrain::TerrainCookedRevision cubic;
    terrain::TerrainCookedRevision quintic;
    Fingerprint128 cubicDigest;
    Fingerprint128 quinticDigest;
    REQUIRE(corpus::TryCookReadbackRevision(terrain::TerrainNoiseLayout::CubicFade, cubic, cubicDigest));
    REQUIRE(corpus::TryCookReadbackRevision(terrain::TerrainNoiseLayout::QuinticFade, quintic, quinticDigest));
    CHECK(cubicDigest == corpus::EXPECTED_CUBIC_READBACK_MANIFEST_DIGEST);
    CHECK(quinticDigest == corpus::EXPECTED_QUINTIC_READBACK_MANIFEST_DIGEST);
    CHECK(cubicDigest != quinticDigest);

    corpus::TerrainReadback readback;
    REQUIRE(corpus::TryReadTerrainReadback(cubic.GetManifestBytes(), cubic.GetPagePackBytes(), readback));
    CHECK(readback.RecipeVersion == static_cast<ludus::foundation::uint32>(terrain::TerrainNoiseLayout::CubicFade));
    CHECK(readback.PagesRead == static_cast<usize>(corpus::READBACK_PAGES_PER_AXIS) * corpus::READBACK_PAGES_PER_AXIS);
    CHECK(readback.Grid.Columns == corpus::READBACK_SAMPLES_PER_SIDE);
    CHECK(readback.Grid.Rows == corpus::READBACK_SAMPLES_PER_SIDE);
}

TEST_CASE("Terrain readback rejects a corrupted page and preserves its output", "[content][terrain][cook]")
{
    terrain::TerrainCookedRevision revision;
    Fingerprint128 digest;
    REQUIRE(corpus::TryCookReadbackRevision(terrain::TerrainNoiseLayout::QuinticFade, revision, digest));
    const BorrowedView<const uint8> pack = revision.GetPagePackBytes();
    Array<uint8> corrupted;
    REQUIRE(corrupted.TryResize(pack.GetSize()));
    for (usize i = 0; i < pack.GetSize(); ++i)
    {
        corrupted[i] = pack[i];
    }
    corrupted[corrupted.GetSize() - 1] = static_cast<uint8>(corrupted[corrupted.GetSize() - 1] ^ 1U);
    corpus::TerrainReadback readback;
    readback.PagesRead = 1;
    CHECK(corpus::TryReadTerrainReadback(revision.GetManifestBytes(), corrupted.AsView(), readback) == false);
    CHECK(readback.PagesRead == 1);
    CHECK(readback.HeightsMetres.IsEmpty());
}
