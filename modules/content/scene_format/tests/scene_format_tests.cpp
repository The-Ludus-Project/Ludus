// Header self-sufficiency and layout contract for Ludus::SceneFormat.

#include <ludus/content/scene_format/luscn.h>
#include <ludus/content/scene_format/material.h>

#include <catch2/catch_test_macros.hpp>

namespace format = ludus::content::scene_format;

TEST_CASE("LUSCN record layouts match the wire format", "[scene_format]")
{
    REQUIRE(format::MAGIC.size() == 8);
    REQUIRE(format::MAGIC.back() == '\0');
    REQUIRE(sizeof(format::Header) == 68);
    REQUIRE(sizeof(format::VertexRecord) == 32);
    REQUIRE(sizeof(format::ObjectRecord) == 56);
    REQUIRE(format::NO_TEXTURE == 0xFFFFFFFFU);
}

TEST_CASE("Material capabilities decide pass requirements without model names", "[scene_format][materials]")
{
    using format::MaterialModel;
    const format::MaterialRequirement restricted = format::FIXED_DIMENSION_REFLECTION_REQUIREMENT;
    // The restricted sampler admits exactly the models the former model-name predicate listed.
    CHECK(format::SupportsFixedDimensionReflection(MaterialModel::MetallicRoughness) == true);
    CHECK(format::SupportsFixedDimensionReflection(MaterialModel::Diffuse) == true);
    CHECK(format::SupportsFixedDimensionReflection(MaterialModel::Conductor) == true);
    CHECK(format::SupportsFixedDimensionReflection(MaterialModel::DiffuseConductorMix) == true);
    CHECK(format::SupportsFixedDimensionReflection(MaterialModel::CoatedDiffuse) == false);
    CHECK(format::SupportsFixedDimensionReflection(MaterialModel::Dielectric) == false);
    CHECK(format::SupportsFixedDimensionReflection(MaterialModel::ThinDielectric) == false);
    CHECK(format::SupportsFixedDimensionReflection(MaterialModel::Unresolved) == false);
    CHECK(format::SupportsFixedDimensionReflection(MaterialModel::ProjectBrdf) == false);
    CHECK(format::GetMaterialCapabilities(MaterialModel::Unresolved) == 0);
    CHECK(format::SatisfiesMaterialRequirement(format::GetMaterialCapabilities(MaterialModel::CoatedDiffuse),
                                               restricted) == false);
    CHECK(format::SatisfiesMaterialRequirement(format::GetMaterialCapabilities(MaterialModel::CoatedDiffuse),
                                               format::MaterialRequirement{}) == true);

    format::MaterialScattering program = { .Model = MaterialModel::ProjectBrdf, .CoatingThickness = 0 };
    CHECK(format::SupportsFixedDimensionReflection(program) == false);
    program.CoatingThickness = 1;
    CHECK(format::SupportsFixedDimensionReflection(program) == true);
    CHECK(format::GetMaterialCapabilities(program) == (format::LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION |
                                                       format::LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER));
    // A declared sampler does not hide a transport event the restricted estimator cannot consume.
    const format::MaterialCapabilityBits walking =
        format::GetMaterialCapabilities(program) | format::LUDUS_MATERIAL_CAPABILITY_INTERNAL_WALK;
    CHECK(format::SatisfiesMaterialRequirement(walking, restricted) == false);
}

TEST_CASE("Triangle bake charts are deterministic, guttered and capacity bounded", "[scene_format]")
{
    format::BakeChartLayout layout;
    REQUIRE(format::TryMakeBakeChartLayout(5, layout) == true);
    REQUIRE(layout.Width == 16);
    REQUIRE(layout.Height == 8);
    REQUIRE(layout.TriangleCount == 5);
    format::float32 u = -1;
    format::float32 v = -1;
    REQUIRE(format::TryGetBakeChartUv(layout, 4, 0, u, v) == true);
    REQUIRE(u == 1.5F / 16);
    REQUIRE(v == 5.5F / 8);
    REQUIRE(format::TryGetBakeChartUv(layout, 4, 1, u, v) == true);
    REQUIRE(u == 2.5F / 16);
    REQUIRE(format::TryGetBakeChartUv(layout, 4, 2, u, v) == true);
    REQUIRE(v == 6.5F / 8);
    REQUIRE(format::TryGetBakeChartUv(layout, 5, 0, u, v) == false);
    REQUIRE(u == 1.5F / 16);
    REQUIRE(v == 6.5F / 8);
    REQUIRE(format::TryMakeBakeChartLayout(0, layout) == false);
    REQUIRE(layout.TriangleCount == 5);
    constexpr format::uint32 MAX_TRIANGLES = (format::MAX_BAKE_CHART_DIMENSION / format::BAKE_CHART_TILE_PIXELS) *
                                             (format::MAX_BAKE_CHART_DIMENSION / format::BAKE_CHART_TILE_PIXELS);
    REQUIRE(format::TryMakeBakeChartLayout(MAX_TRIANGLES, layout) == true);
    REQUIRE(layout.Width == format::MAX_BAKE_CHART_DIMENSION);
    REQUIRE(layout.Height == format::MAX_BAKE_CHART_DIMENSION);
    REQUIRE(format::TryMakeBakeChartLayout(MAX_TRIANGLES + 1, layout) == false);
    REQUIRE(layout.TriangleCount == MAX_TRIANGLES);
}
