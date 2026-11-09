// Header self-sufficiency and layout contract for Ludus::SceneFormat.

#include <ludus/content/scene_format/luscn.h>
#include <ludus/content/scene_format/material.h>
#include <ludus/content/scene_format/material_program.h>

#include <limits>

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

namespace
{
/// A diffuse lobe weighted by one minus metallic, and a conductor weighted by metallic with
/// roughness from the surface: the built-in Mix written as a closure program.
format::MaterialProgram MakeMixProgram()
{
    using format::MaterialProgramOpcode;
    format::MaterialProgram program;
    program.InstructionCount = 3;
    program.Instructions[0] =
    {
        .Opcode = MaterialProgramOpcode::SurfaceInput,
        .Destination = 0,
        .OperandA = static_cast<ludus::foundation::uint8>(format::MaterialProgramInput::Metallic),
    };
    program.Instructions[1] = { .Opcode = MaterialProgramOpcode::OneMinus, .Destination = 1, .OperandA = 0 };
    program.Instructions[2] =
    {
        .Opcode = MaterialProgramOpcode::SurfaceInput,
        .Destination = 2,
        .OperandA = static_cast<ludus::foundation::uint8>(format::MaterialProgramInput::Roughness),
    };
    program.LobeCount = 2;
    program.Lobes[0] =
    {
        .Operation = format::MaterialLobeOperation::Diffuse,
        .WeightRegister = 1,
        .RoughnessRegister = format::NO_MATERIAL_PROGRAM_REGISTER,
    };
    program.Lobes[1] =
    {
        .Operation = format::MaterialLobeOperation::Conductor,
        .WeightRegister = 0,
        .RoughnessRegister = 2,
        .Eta = {{0.2F, 0.9F, 1.1F}},
        .K = {{3.9F, 2.4F, 2.2F}},
    };
    return program;
}
} // namespace

TEST_CASE("Closure programs validate every operand, slot and parameter", "[scene_format][materials]")
{
    const format::MaterialProgram valid = MakeMixProgram();
    REQUIRE(format::IsMaterialProgramValid(valid) == true);
    CHECK(sizeof(format::MaterialProgram) == format::MATERIAL_PROGRAM_BYTES);
    CHECK(format::GetMaterialProgramCapabilities(valid) == (format::LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION |
                                                            format::LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER));
    CHECK(format::SatisfiesMaterialRequirement(format::GetMaterialProgramCapabilities(valid),
                                               format::FIXED_DIMENSION_REFLECTION_REQUIREMENT) == true);

    format::MaterialProgram program = valid;
    program.Version = 2;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    // Register 3 is read before any instruction writes it.
    program.Instructions[1].OperandA = 3;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    constexpr ludus::foundation::uint8 UNKNOWN_TAG = 200;
    program.Instructions[1].Opcode = static_cast<format::MaterialProgramOpcode>(UNKNOWN_TAG);
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Instructions[0].OperandA = UNKNOWN_TAG;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Instructions[1].Reserved[0] = 1;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Instructions[3].Destination = 1;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Instructions[1].Destination = format::MAX_MATERIAL_PROGRAM_REGISTERS;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Instructions[0] = { .Opcode = format::MaterialProgramOpcode::Constant, .Destination = 0, .OperandA = 0 };
    CHECK(format::IsMaterialProgramValid(program) == false);
    program.ConstantCount = 1;
    program.Constants[0] = {{0.5F, 0.5F, 0.5F, 0}};
    CHECK(format::IsMaterialProgramValid(program) == true);
    program.Constants[0][1] = std::numeric_limits<format::float32>::quiet_NaN();
    CHECK(format::IsMaterialProgramValid(program) == false);
    program.Constants[0][1] = format::MAX_MATERIAL_PROGRAM_CONSTANT * 2;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Constants[1][0] = 1;
    CHECK(format::IsMaterialProgramValid(program) == false);

    program = valid;
    program.Lobes[0].RoughnessRegister = 2;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Lobes[1].RoughnessRegister = format::NO_MATERIAL_PROGRAM_REGISTER;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Lobes[1].Eta[0] = 0;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Lobes[0].Power = 1;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Lobes[1].Operation = format::MaterialLobeOperation::CosinePower;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program.Lobes[1].Eta = {};
    program.Lobes[1].K = {};
    program.Lobes[1].Power = format::LUDUS_PROJECT_BRDF_MAX_POWER;
    CHECK(format::IsMaterialProgramValid(program) == true);
    program.Lobes[1].Power = format::LUDUS_PROJECT_BRDF_MAX_POWER + 1;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Lobes[2].WeightRegister = 1;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.LobeCount = 0;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program = valid;
    program.Lobes[0].Operation = static_cast<format::MaterialLobeOperation>(UNKNOWN_TAG);
    CHECK(format::IsMaterialProgramValid(program) == false);
}

TEST_CASE("Closure-program materials are stored but not yet evaluated", "[scene_format][materials]")
{
    format::MaterialScattering scattering;
    scattering.Model = format::MaterialModel::Program;
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == true);
    // No renderer admits a program until an evaluator lands, so installation rejects it.
    CHECK(format::IsMaterialScatteringValid(scattering) == false);
    CHECK(format::GetMaterialCapabilities(scattering) == 0);
    CHECK(format::SupportsFixedDimensionReflection(scattering) == false);
    CHECK(format::GetMaterialModelBundleVersion(format::MaterialModel::Program) == format::VERSION_11);
    CHECK(format::IsSupportedVersion(format::VERSION_11) == true);
    // Its scattering fields keep their defaults, so equal programs cook to equal bytes.
    scattering.Eta[0] = 2;
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == false);
    scattering = {};
    scattering.Model = format::MaterialModel::Program;
    scattering.CoatingThickness = 1;
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == false);
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
