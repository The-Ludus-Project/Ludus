// Material rule contracts: validation, feathered memberships, normalization
// and the unmatched fallback, patch tiling, shared-edge halos, ascending palettes,
// exact-sum quantization, overflow reports and failure preservation. See
// docs/architecture/terrain.md#surface-materials-and-attached-systems.

#include <ludus/content/terrain/terrain_materials.h>

#include <ludus/foundation/containers/array.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::content::terrain;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;

namespace
{
constexpr uint16 GRASS = 10;
constexpr uint16 ROCK = 20;
constexpr uint16 MUD = 30;
constexpr uint16 SNOW = 40;
constexpr uint16 SAND = 50;
/// Two patches of two cells along X, one patch along Z.
constexpr uint32 TEST_COLUMNS = 5;
constexpr uint32 TEST_ROWS = 3;
constexpr uint32 CELLS_PER_PATCH = 2;
constexpr float32 SNOW_LINE_METRES = 100;
constexpr float32 CLIFF_SLOPE = 1;
constexpr float32 WET_THRESHOLD = 0.7F;
constexpr float32 LARGEST_FINITE = 3.0e38F;

/// Infinity produced at run time, so no literal exceeds the float range.
[[nodiscard]] float32 MakeNonFinite()
{
    const float32 large = LARGEST_FINITE;
    return large * large;
}

/// A range spelled by its three members, so partial designated initializers are not needed.
[[nodiscard]] TerrainMaterialRange MakeRange(float32 minimum, float32 maximum, float32 featherWidth)
{
    TerrainMaterialRange range;
    range.Minimum = minimum;
    range.Maximum = maximum;
    range.FeatherWidth = featherWidth;
    return range;
}

[[nodiscard]] TerrainMaterialRange Below(float32 maximum, float32 featherWidth)
{
    return MakeRange(-TERRAIN_MATERIAL_RANGE_UNBOUNDED, maximum, featherWidth);
}

[[nodiscard]] TerrainMaterialRange Above(float32 minimum, float32 featherWidth)
{
    return MakeRange(minimum, TERRAIN_MATERIAL_RANGE_UNBOUNDED, featherWidth);
}

[[nodiscard]] TerrainMaterialRule MakeRule(uint16 materialId,
                                           float32 weight,
                                           TerrainMaterialRange elevationMetres,
                                           TerrainMaterialRange slope,
                                           TerrainMaterialRange wetness)
{
    TerrainMaterialRule rule;
    rule.MaterialId = materialId;
    rule.Weight = weight;
    rule.ElevationMetres = elevationMetres;
    rule.Slope = slope;
    rule.Wetness = wetness;
    return rule;
}

/// Fields and storage for one grid; the tests fill the samples they care about.
struct Scene final
{
    TerrainFieldGrid Grid{ .Columns = TEST_COLUMNS, .Rows = TEST_ROWS, .SpacingXMetres = 1, .SpacingZMetres = 1 };
    Array<float32> Elevation;
    Array<float32> Slope;
    Array<float32> Wetness;

    Scene()
        : Elevation(GetTerrainSampleCount(Grid), 0.0F), Slope(GetTerrainSampleCount(Grid), 0.0F),
          Wetness(GetTerrainSampleCount(Grid), 0.0F)
    {
    }

    [[nodiscard]] TerrainMaterialFields GetFields() const
    {
        return
        {
            .ElevationMetres = BorrowedView<const float32>(Elevation.GetData(), Elevation.GetSize()),
            .Slope = BorrowedView<const float32>(Slope.GetData(), Slope.GetSize()),
            .Wetness = BorrowedView<const float32>(Wetness.GetData(), Wetness.GetSize()),
        };
    }
};

/// Grass everywhere low and gentle, rock on cliffs, mud when wet, snow above the snow line.
[[nodiscard]] Array<TerrainMaterialRule> MakeRules()
{
    Array<TerrainMaterialRule> rules;
    const TerrainMaterialRange any;
    rules.Add(MakeRule(GRASS, 1, any, Below(CLIFF_SLOPE, 0.5F), any));
    rules.Add(MakeRule(ROCK, 1, any, Above(CLIFF_SLOPE, 0.5F), any));
    rules.Add(MakeRule(MUD, 2, any, any, Above(WET_THRESHOLD, 0)));
    rules.Add(MakeRule(SNOW, 4, Above(SNOW_LINE_METRES, 0), any, any));
    return rules;
}

[[nodiscard]] BorrowedView<const TerrainMaterialRule> View(const Array<TerrainMaterialRule>& rules)
{
    return {rules.GetData(), rules.GetSize()};
}

[[nodiscard]] Array<float32>
EvaluateWeights(const Scene& scene, const Array<TerrainMaterialRule>& rules, TerrainMaterialWeightSummary& outSummary)
{
    Array<float32> weights(rules.GetSize() * GetTerrainSampleCount(scene.Grid), -1.0F);
    REQUIRE(TryEvaluateTerrainMaterialWeights(scene.Grid,
                                              View(rules),
                                              scene.GetFields(),
                                              BorrowedView<float32>(weights.GetData(), weights.GetSize()),
                                              outSummary) == TerrainMaterialStatus::Success);
    return weights;
}

/// Palettes and weights of one build.
struct Patches final
{
    Array<TerrainPatchPalette> Palettes;
    Array<TerrainMaterialWeights> Weights;
    TerrainPaletteSummary Summary;

    Patches(const TerrainFieldGrid& grid, const TerrainPatchLayout& layout)
    {
        uint32 patchesX = 0;
        uint32 patchesZ = 0;
        REQUIRE(TryGetTerrainPatchCounts(grid, layout, patchesX, patchesZ) == true);
        Palettes.Resize(static_cast<usize>(patchesX) * patchesZ);
        Weights.Resize(Palettes.GetSize() * GetTerrainPatchSampleCount(layout));
    }

    [[nodiscard]] TerrainMaterialStatus Build(const TerrainFieldGrid& grid,
                                              const TerrainPatchLayout& layout,
                                              const Array<TerrainMaterialRule>& rules,
                                              const Array<float32>& ruleWeights)
    {
        return TryBuildTerrainPatchPalettes(grid,
                                            layout,
                                            View(rules),
                                            BorrowedView<const float32>(ruleWeights.GetData(), ruleWeights.GetSize()),
                                            BorrowedView<TerrainPatchPalette>(Palettes.GetData(), Palettes.GetSize()),
                                            BorrowedView<TerrainMaterialWeights>(Weights.GetData(), Weights.GetSize()),
                                            Summary);
    }
};

[[nodiscard]] uint32 SumChannels(const TerrainMaterialWeights& weights)
{
    uint32 total = 0;
    for (usize channel = 0; channel < TERRAIN_PATCH_MATERIAL_CHANNELS; ++channel)
    {
        total += weights.Channels[channel];
    }
    return total;
}
} // namespace

TEST_CASE("terrain material rule validation", "[content][terrain]")
{
    Array<TerrainMaterialRule> rules = MakeRules();
    CHECK(ValidateTerrainMaterialRules(View(rules)).Status == TerrainMaterialStatus::Success);

    CHECK(ValidateTerrainMaterialRules(BorrowedView<const TerrainMaterialRule>()).Issue ==
          TerrainMaterialIssue::RuleCount);
    Array<TerrainMaterialRule> tooMany(MAX_TERRAIN_MATERIAL_RULES + 1, rules[0]);
    CHECK(ValidateTerrainMaterialRules(View(tooMany)).Issue == TerrainMaterialIssue::RuleCount);

    rules[1].MaterialId = TERRAIN_MATERIAL_NONE;
    TerrainMaterialValidation validation = ValidateTerrainMaterialRules(View(rules));
    CHECK(validation.Status == TerrainMaterialStatus::InvalidRule);
    CHECK(validation.Issue == TerrainMaterialIssue::MaterialNone);
    CHECK(validation.Index == 1);

    rules = MakeRules();
    rules[2].Weight = 0;
    validation = ValidateTerrainMaterialRules(View(rules));
    CHECK(validation.Issue == TerrainMaterialIssue::Weight);
    CHECK(validation.Index == 2);

    rules = MakeRules();
    rules[3].Slope = MakeRange(2, 1, 0);
    validation = ValidateTerrainMaterialRules(View(rules));
    CHECK(validation.Issue == TerrainMaterialIssue::Range);
    CHECK(validation.Index == 3);

    rules = MakeRules();
    rules[0].Wetness.FeatherWidth = -1;
    CHECK(ValidateTerrainMaterialRules(View(rules)).Issue == TerrainMaterialIssue::Range);
}

TEST_CASE("terrain material weights follow the fields and normalize", "[content][terrain]")
{
    Scene scene;
    const Array<TerrainMaterialRule> rules = MakeRules();
    const usize samples = GetTerrainSampleCount(scene.Grid);
    // Sample 0: flat, dry, low -> grass only. Sample 1: cliff -> rock only.
    scene.Slope[1] = 2 * CLIFF_SLOPE;
    // Sample 2: half way up the rock feather, so grass and rock share it.
    scene.Slope[2] = CLIFF_SLOPE;
    // Sample 3: wet -> mud at weight two beside grass at weight one.
    scene.Wetness[3] = 1;
    // Sample 4: above the snow line -> snow at weight four beside grass.
    scene.Elevation[4] = SNOW_LINE_METRES + 1;
    TerrainMaterialWeightSummary summary;
    const Array<float32> weights = EvaluateWeights(scene, rules, summary);
    CHECK(summary.UnmatchedSamples == 0);
    for (usize sample = 0; sample < samples; ++sample)
    {
        float32 total = 0;
        for (usize rule = 0; rule < rules.GetSize(); ++rule)
        {
            CHECK(weights[rule * samples + sample] >= 0);
            total += weights[rule * samples + sample];
        }
        CHECK(total == Catch::Approx(1));
    }
    CHECK(weights[0] == 1);
    CHECK(weights[samples + 1] == 1);
    CHECK(weights[2] == Catch::Approx(0.5F));
    CHECK(weights[samples + 2] == Catch::Approx(0.5F));
    CHECK(weights[2 * samples + 3] == Catch::Approx(2.0F / 3));
    CHECK(weights[3 * samples + 4] == Catch::Approx(4.0F / 5));

    SECTION("a sample no rule admits takes the first rule and is counted")
    {
        Array<TerrainMaterialRule> narrow;
        const TerrainMaterialRange any;
        narrow.Add(MakeRule(SAND, 1, MakeRange(1, 2, 0), any, any));
        narrow.Add(MakeRule(ROCK, 1, MakeRange(5, 6, 0), any, any));
        TerrainMaterialWeightSummary narrowSummary;
        const Array<float32> narrowWeights = EvaluateWeights(scene, narrow, narrowSummary);
        CHECK(narrowSummary.UnmatchedSamples == samples);
        CHECK(narrowWeights[0] == 1);
        CHECK(narrowWeights[samples] == 0);
    }

    SECTION("failures preserve the output")
    {
        Array<float32> untouched(rules.GetSize() * samples, -1.0F);
        const BorrowedView<float32> outWeights(untouched.GetData(), untouched.GetSize());
        CHECK(TryEvaluateTerrainMaterialWeights(scene.Grid,
                                                View(rules),
                                                scene.GetFields(),
                                                BorrowedView<float32>(untouched.GetData(), 3),
                                                summary) == TerrainMaterialStatus::InvalidStorage);
        Scene broken;
        broken.Wetness[2] = MakeNonFinite();
        CHECK(TryEvaluateTerrainMaterialWeights(broken.Grid, View(rules), broken.GetFields(), outWeights, summary) ==
              TerrainMaterialStatus::NonFiniteInput);
        Array<TerrainMaterialRule> invalid = rules;
        invalid[0].Weight = -1;
        CHECK(TryEvaluateTerrainMaterialWeights(scene.Grid, View(invalid), scene.GetFields(), outWeights, summary) ==
              TerrainMaterialStatus::InvalidRule);
        for (usize index = 0; index < untouched.GetSize(); ++index)
        {
            CHECK(untouched[index] == -1.0F);
        }
    }
}

TEST_CASE("terrain patch layout must tile the grid", "[content][terrain]")
{
    const TerrainFieldGrid grid{ .Columns = TEST_COLUMNS, .Rows = TEST_ROWS, .SpacingXMetres = 1, .SpacingZMetres = 1 };
    uint32 patchesX = 0;
    uint32 patchesZ = 0;
    CHECK(TryGetTerrainPatchCounts(grid, { .CellsPerPatch = CELLS_PER_PATCH, .HaloSamples = 1 }, patchesX, patchesZ) ==
          true);
    CHECK(patchesX == 2);
    CHECK(patchesZ == 1);
    CHECK(TryGetTerrainPatchCounts(grid, { .CellsPerPatch = 3, .HaloSamples = 1 }, patchesX, patchesZ) == false);
    CHECK(TryGetTerrainPatchCounts(grid, { .CellsPerPatch = 0, .HaloSamples = 1 }, patchesX, patchesZ) == false);
    CHECK(TryGetTerrainPatchCounts(grid,
                                   { .CellsPerPatch = MAX_TERRAIN_PATCH_CELLS + 1, .HaloSamples = 0 },
                                   patchesX,
                                   patchesZ) == false);
    CHECK(TryGetTerrainPatchCounts(grid,
                                   { .CellsPerPatch = 2, .HaloSamples = MAX_TERRAIN_PATCH_HALO_SAMPLES + 1 },
                                   patchesX,
                                   patchesZ) == false);
    CHECK(GetTerrainPatchSampleCount({ .CellsPerPatch = CELLS_PER_PATCH, .HaloSamples = 1 }) == 9);
}

TEST_CASE("terrain patch palettes are ascending, shared at edges and quantized to one", "[content][terrain]")
{
    Scene scene;
    const Array<TerrainMaterialRule> rules = MakeRules();
    const usize samples = GetTerrainSampleCount(scene.Grid);
    // Column 4 is the far edge of the east patch only; a cliff there must not enter the west palette with a halo
    // of one, while column 2 is shared by both patches.
    for (uint32 z = 0; z < scene.Grid.Rows; ++z)
    {
        scene.Slope[z * scene.Grid.Columns + 4] = 2 * CLIFF_SLOPE;
        scene.Wetness[z * scene.Grid.Columns + 2] = 1;
    }
    // Snow only at the west corner, which the east patch's one-sample halo does not reach.
    scene.Elevation[0] = SNOW_LINE_METRES + 1;
    TerrainMaterialWeightSummary weightSummary;
    const Array<float32> weights = EvaluateWeights(scene, rules, weightSummary);
    const TerrainPatchLayout layout{ .CellsPerPatch = CELLS_PER_PATCH, .HaloSamples = 1 };
    Patches patches(scene.Grid, layout);
    REQUIRE(patches.Build(scene.Grid, layout, rules, weights) == TerrainMaterialStatus::Success);
    CHECK(patches.Summary.OverflowPatches == 0);
    CHECK(patches.Summary.MaximumQuantizationError <= 1.0F / TERRAIN_MATERIAL_WEIGHT_ONE);

    const TerrainPatchPalette& west = patches.Palettes[0];
    REQUIRE(west.Count == 3);
    CHECK(west.MaterialIds[0] == GRASS);
    CHECK(west.MaterialIds[1] == MUD);
    CHECK(west.MaterialIds[2] == SNOW);
    CHECK(west.MaterialIds[3] == TERRAIN_MATERIAL_NONE);
    const TerrainPatchPalette& east = patches.Palettes[1];
    REQUIRE(east.Count == 3);
    CHECK(east.MaterialIds[0] == GRASS);
    CHECK(east.MaterialIds[1] == ROCK);
    CHECK(east.MaterialIds[2] == MUD);

    const usize patchSamples = GetTerrainPatchSampleCount(layout);
    for (usize index = 0; index < patches.Weights.GetSize(); ++index)
    {
        CHECK(SumChannels(patches.Weights[index]) == TERRAIN_MATERIAL_WEIGHT_ONE);
    }
    // The shared column is the west patch's local column 2 and the east patch's local column 0.
    for (uint32 localZ = 0; localZ <= CELLS_PER_PATCH; ++localZ)
    {
        const TerrainMaterialWeights& westSample = patches.Weights[localZ * (CELLS_PER_PATCH + 1) + 2];
        const TerrainMaterialWeights& eastSample = patches.Weights[patchSamples + localZ * (CELLS_PER_PATCH + 1)];
        // Mud (weight two) over grass (weight one): 170 and 85 of 255.
        CHECK(westSample.Channels[1] == 170);
        CHECK(westSample.Channels[0] == 85);
        CHECK(eastSample.Channels[2] == 170);
        CHECK(eastSample.Channels[0] == 85);
    }
    (void)samples;

    SECTION("a halo of zero drops the neighbour's edge material")
    {
        const TerrainPatchLayout noHalo{ .CellsPerPatch = CELLS_PER_PATCH, .HaloSamples = 0 };
        Patches bare(scene.Grid, noHalo);
        REQUIRE(bare.Build(scene.Grid, noHalo, rules, weights) == TerrainMaterialStatus::Success);
        CHECK(bare.Palettes[0].Count == 3);
        CHECK(bare.Palettes[1].Count == 3);
        // With a halo of two the west patch sees the cliff at column 4.
        const TerrainPatchLayout wideHalo{ .CellsPerPatch = CELLS_PER_PATCH, .HaloSamples = 2 };
        Patches wide(scene.Grid, wideHalo);
        REQUIRE(wide.Build(scene.Grid, wideHalo, rules, weights) == TerrainMaterialStatus::Success);
        CHECK(wide.Palettes[0].Count == 4);
        CHECK(wide.Palettes[0].MaterialIds[1] == ROCK);
    }
}

TEST_CASE("terrain patch palette overflow is reported, not dropped", "[content][terrain]")
{
    Scene scene;
    Array<TerrainMaterialRule> rules = MakeRules();
    const TerrainMaterialRange any;
    rules.Add(MakeRule(SAND, 1, MakeRange(-1, -0.5F, 0), any, any));
    // Put every material into the west patch: cliff, wet, snow and sand samples.
    scene.Slope[1] = 2 * CLIFF_SLOPE;
    scene.Wetness[2] = 1;
    scene.Elevation[TEST_COLUMNS] = SNOW_LINE_METRES + 1;
    scene.Elevation[TEST_COLUMNS + 1] = -0.75F;
    TerrainMaterialWeightSummary weightSummary;
    const Array<float32> weights = EvaluateWeights(scene, rules, weightSummary);
    const TerrainPatchLayout layout{ .CellsPerPatch = CELLS_PER_PATCH, .HaloSamples = 0 };
    Patches patches(scene.Grid, layout);
    patches.Palettes[0].Count = 9;
    CHECK(patches.Build(scene.Grid, layout, rules, weights) == TerrainMaterialStatus::PaletteOverflow);
    CHECK(patches.Summary.OverflowPatches == 1);
    CHECK(patches.Summary.FirstOverflow.PatchX == 0);
    CHECK(patches.Summary.FirstOverflow.PatchZ == 0);
    CHECK(patches.Summary.FirstOverflow.MaterialCount == 5);
    CHECK(patches.Palettes[0].Count == 9);

    SECTION("storage and layout failures preserve the outputs")
    {
        CHECK(TryBuildTerrainPatchPalettes(
                  scene.Grid,
                  { .CellsPerPatch = 3, .HaloSamples = 0 },
                  View(rules),
                  BorrowedView<const float32>(weights.GetData(), weights.GetSize()),
                  BorrowedView<TerrainPatchPalette>(patches.Palettes.GetData(), patches.Palettes.GetSize()),
                  BorrowedView<TerrainMaterialWeights>(patches.Weights.GetData(), patches.Weights.GetSize()),
                  patches.Summary) == TerrainMaterialStatus::InvalidLayout);
        CHECK(TryBuildTerrainPatchPalettes(
                  scene.Grid,
                  layout,
                  View(rules),
                  BorrowedView<const float32>(weights.GetData(), 4),
                  BorrowedView<TerrainPatchPalette>(patches.Palettes.GetData(), patches.Palettes.GetSize()),
                  BorrowedView<TerrainMaterialWeights>(patches.Weights.GetData(), patches.Weights.GetSize()),
                  patches.Summary) == TerrainMaterialStatus::InvalidStorage);
        CHECK(patches.Palettes[0].Count == 9);
    }
}
