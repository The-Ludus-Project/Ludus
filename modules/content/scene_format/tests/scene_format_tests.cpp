// Header self-sufficiency and layout contract for Ludus::SceneFormat.

#include <ludus/content/scene_format/luscn.h>
#include <ludus/content/scene_format/material.h>
#include <ludus/content/scene_format/material_program.h>

#include <ludus/foundation/base/types.h>

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
    // Oren-Nayar reads its sigma register and no conductor or cosine-power parameter.
    program = valid;
    program.Lobes[1].Operation = format::MaterialLobeOperation::OrenNayar;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program.Lobes[1].Eta = {};
    program.Lobes[1].K = {};
    CHECK(format::IsMaterialProgramValid(program) == true);
    CHECK(format::GetMaterialProgramCapabilities(program) ==
          (format::LUDUS_MATERIAL_CAPABILITY_CONTINUOUS_REFLECTION |
           format::LUDUS_MATERIAL_CAPABILITY_FIXED_DIMENSION_SAMPLER));
    program.Lobes[1].Power = 1;
    CHECK(format::IsMaterialProgramValid(program) == false);
    program.Lobes[1].Power = 0;
    program.Lobes[1].RoughnessRegister = format::NO_MATERIAL_PROGRAM_REGISTER;
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

TEST_CASE("Closure-program materials keep default scattering and take the program's capabilities",
          "[scene_format][materials]")
{
    format::MaterialScattering scattering;
    scattering.Model = format::MaterialModel::Program;
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == true);
    CHECK(format::IsMaterialScatteringValid(scattering) == true);
    // The model alone guarantees nothing; the program declares the capabilities.
    CHECK(format::GetMaterialCapabilities(scattering) == 0);
    CHECK(format::SupportsFixedDimensionReflection(scattering) == false);
    const format::MaterialProgram program = MakeMixProgram();
    CHECK(format::SupportsFixedDimensionReflection(scattering, program) == true);
    format::MaterialScattering coated;
    coated.Model = format::MaterialModel::CoatedDiffuse;
    CHECK(format::SupportsFixedDimensionReflection(coated, program) == false);
    CHECK(format::GetMaterialModelBundleVersion(format::MaterialModel::Program) == format::VERSION_11);
    CHECK(format::IsSupportedVersion(format::VERSION_11) == true);
    // Its scattering fields keep their defaults, so equal programs cook to equal bytes.
    scattering.Eta[0] = 2;
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == false);
    scattering = {};
    scattering.Model = format::MaterialModel::Program;
    scattering.CoatingThickness = 1;
    CHECK(format::IsMaterialScatteringValid(scattering) == false);
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

TEST_CASE("Hair cards store strand controls in LUSCN v13 and reflect as metallic/roughness",
          "[scene_format][materials][hair]")
{
    format::HairCardParameters parameters;
    parameters.StrandDirectionDegrees = 90;
    parameters.PrimaryShiftDegrees = -8;
    parameters.SecondaryShiftDegrees = 12;
    parameters.PrimaryWeight = 0.25F;
    parameters.SecondaryWeight = 0.5F;
    const format::MaterialScattering scattering = format::MakeHairCardScattering(parameters);
    CHECK(scattering.Model == format::MaterialModel::HairCard);
    CHECK(format::IsHairCardValid(scattering) == true);
    CHECK(format::IsMaterialScatteringValid(scattering) == true);
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == true);
    const format::HairCardParameters read = format::GetHairCardParameters(scattering);
    CHECK(read.StrandDirectionDegrees == 90);
    CHECK(read.PrimaryShiftDegrees == -8);
    CHECK(read.SecondaryShiftDegrees == 12);
    CHECK(read.PrimaryWeight == 0.25F);
    CHECK(read.SecondaryWeight == 0.5F);
    CHECK(format::GetMaterialModelBundleVersion(format::MaterialModel::HairCard) == format::VERSION_13);
    CHECK(format::IsSupportedVersion(format::VERSION_13) == true);
    CHECK(format::SupportsFixedDimensionReflection(scattering) == true);
    // Ray and reference evaluators receive the metallic/roughness lobes; other models pass through.
    CHECK(format::GetSurfaceReflectionScattering(scattering).Model == format::MaterialModel::MetallicRoughness);
    format::MaterialScattering conductor;
    conductor.Model = format::MaterialModel::Conductor;
    conductor.AlphaU = 0.25F;
    conductor.AlphaV = 0.25F;
    CHECK(format::GetSurfaceReflectionScattering(conductor).AlphaU == 0.25F);
    CHECK(format::GetHairCardParameters(conductor).StrandDirectionDegrees == 0);
    // Every control is range checked, including non-finite values.
    const auto isValidWith = [&](auto edit) {
        format::HairCardParameters changed = parameters;
        edit(changed);
        return format::IsHairCardValid(format::MakeHairCardScattering(changed));
    };
    CHECK(isValidWith([](format::HairCardParameters& inoutValue) { inoutValue.StrandDirectionDegrees = 180; }) == true);
    CHECK(isValidWith([](format::HairCardParameters& inoutValue) { inoutValue.StrandDirectionDegrees = 181; }) ==
          false);
    CHECK(isValidWith([](format::HairCardParameters& inoutValue) {
              inoutValue.StrandDirectionDegrees = std::numeric_limits<format::float32>::quiet_NaN();
          }) == false);
    CHECK(isValidWith([](format::HairCardParameters& inoutValue) {
              inoutValue.PrimaryShiftDegrees = format::MAX_HAIR_CARD_SHIFT_DEGREES + 1;
          }) == false);
    CHECK(isValidWith([](format::HairCardParameters& inoutValue) {
              inoutValue.SecondaryShiftDegrees = -format::MAX_HAIR_CARD_SHIFT_DEGREES - 1;
          }) == false);
    CHECK(isValidWith([](format::HairCardParameters& inoutValue) { inoutValue.PrimaryWeight = -0.5F; }) == false);
    CHECK(isValidWith([](format::HairCardParameters& inoutValue) {
              inoutValue.SecondaryWeight = std::numeric_limits<format::float32>::infinity();
          }) == false);
    // Unused slots keep their defaults so equal controls cook to equal bytes.
    format::MaterialScattering changed = scattering;
    changed.K[2] = 1;
    CHECK(format::IsHairCardValid(changed) == false);
    changed = scattering;
    changed.AlphaU = 0.5F;
    CHECK(format::IsMaterialScatteringValid(changed) == false);
}

TEST_CASE("Medium interfaces store absorption in LUSCN v12 and are never surfaces", "[scene_format][materials][medium]")
{
    format::MaterialScattering scattering;
    scattering.Model = format::MaterialModel::MediumInterface;
    scattering.K = {{0.5F, 1, 2}};
    CHECK(format::IsMediumInterfaceValid(scattering) == true);
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == true);
    // Surface evaluators reject it: it neither scatters nor occludes.
    CHECK(format::IsMaterialScatteringValid(scattering) == false);
    CHECK(format::GetMaterialCapabilities(scattering) == 0);
    CHECK(format::GetMaterialModelBundleVersion(format::MaterialModel::MediumInterface) == format::VERSION_12);
    CHECK(format::IsSupportedVersion(format::VERSION_12) == true);
    // Vacuum is admitted; negative, nonfinite and oversized absorption is not.
    scattering.K = {};
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == true);
    for (const format::float32 invalid : {-1.0F,
                                          std::numeric_limits<format::float32>::quiet_NaN(),
                                          format::MAX_MEDIUM_ABSORPTION_PER_WORLD * 2,
                                          std::numeric_limits<format::float32>::infinity()})
    {
        scattering.K = {{0, invalid, 0}};
        CHECK(format::IsStoredMaterialScatteringValid(scattering) == false);
    }
    // Every other field is the index-matched default.
    scattering.K = {};
    scattering.Eta = {{1.5F, 1.5F, 1.5F}};
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == false);
    scattering = {};
    scattering.Model = format::MaterialModel::MediumInterface;
    scattering.AlphaU = .5F;
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == false);
}

TEST_CASE("Skin stores scattering distances in LUSCN v14 and reflects like metallic-roughness",
          "[scene_format][materials][skin]")
{
    format::MaterialScattering scattering;
    scattering.Model = format::MaterialModel::Skin;
    scattering.K = {{0.012F, 0.006F, 0.003F}};
    CHECK(format::IsMaterialScatteringValid(scattering) == true);
    CHECK(format::IsStoredMaterialScatteringValid(scattering) == true);
    // The surface reflection is the metallic-roughness dielectric, so fixed-dimension estimators accept it.
    CHECK(format::GetMaterialCapabilities(scattering) ==
          format::GetMaterialCapabilities(format::MaterialModel::MetallicRoughness));
    CHECK(format::SupportsFixedDimensionReflection(scattering) == true);
    CHECK(format::GetMaterialModelBundleVersion(format::MaterialModel::Skin) == format::VERSION_14);
    CHECK(format::IsSupportedVersion(format::VERSION_14) == true);
    CHECK(format::IsSupportedVersion(format::VERSION_14 + 1) == false);
    // Ray and reference evaluators receive the metallic/roughness dielectric lobes.
    CHECK(format::GetSurfaceReflectionScattering(scattering).Model == format::MaterialModel::MetallicRoughness);
    CHECK(format::LAST_MATERIAL_MODEL == format::MaterialModel::Skin);
    // Zero distance is admitted: it reproduces the unfiltered metallic-roughness lighting.
    scattering.K = {};
    CHECK(format::IsMaterialScatteringValid(scattering) == true);
    scattering.K = {{format::MAX_SKIN_SCATTERING_DISTANCE, 0, 0}};
    CHECK(format::IsMaterialScatteringValid(scattering) == true);
    for (const format::float32 invalid : {-0.001F,
                                          std::numeric_limits<format::float32>::quiet_NaN(),
                                          format::MAX_SKIN_SCATTERING_DISTANCE * 2,
                                          std::numeric_limits<format::float32>::infinity()})
    {
        scattering.K = {{0, 0, invalid}};
        CHECK(format::IsMaterialScatteringValid(scattering) == false);
    }
    // Every other field keeps its default, so equal skin materials cook to equal bytes.
    scattering.K = {};
    scattering.Eta = {{1.4F, 1.4F, 1.4F}};
    CHECK(format::IsMaterialScatteringValid(scattering) == false);
    scattering = {};
    scattering.Model = format::MaterialModel::Skin;
    scattering.AlphaU = .5F;
    scattering.AlphaV = .5F;
    CHECK(format::IsMaterialScatteringValid(scattering) == false);
    scattering = {};
    scattering.Model = format::MaterialModel::Skin;
    scattering.CoatingThickness = 0;
    CHECK(format::IsMaterialScatteringValid(scattering) == false);
}

TEST_CASE("Dielectrics declare dispersion as an index n_d and an Abbe number", "[scene_format][materials][dielectric]")
{
    using ludus::foundation::float64;
    const auto magnitude = [](float64 value) { return value < 0 ? -value : value; };
    // pbrt-v4's glass-F11 as the pbrt importers declare it.
    constexpr format::DielectricDispersion FLINT{ .IndexD = 1.784743F, .AbbeNumber = 25.7F };
    for (const format::MaterialModel model : {format::MaterialModel::Dielectric, format::MaterialModel::ThinDielectric})
    {
        CAPTURE(static_cast<format::uint32>(model));
        const format::MaterialScattering scattering = format::MakeDielectricScattering(model, FLINT);
        CHECK(format::IsMaterialScatteringValid(scattering) == true);
        CHECK(format::IsStoredMaterialScatteringValid(scattering) == true);
        // n_d fills every Eta channel and V_d is K[0]; the declaration reads back unchanged.
        CHECK(scattering.Eta[0] == FLINT.IndexD);
        CHECK(scattering.Eta[2] == FLINT.IndexD);
        CHECK(scattering.K[0] == FLINT.AbbeNumber);
        CHECK(format::GetDielectricDispersion(scattering).IndexD == FLINT.IndexD);
        CHECK(format::GetDielectricDispersion(scattering).AbbeNumber == FLINT.AbbeNumber);
        CHECK(format::GetMaterialCapabilities(scattering) == format::GetMaterialCapabilities(model));
    }
    // Other models carry no dispersion.
    format::MaterialScattering conductor;
    conductor.Model = format::MaterialModel::Conductor;
    conductor.K = {{3, 3, 3}};
    CHECK(format::GetDielectricDispersion(conductor).AbbeNumber == 0);
    // The Cauchy equation n = A + B / lambda^2 through n_d with n_F - n_C = (n_d - 1) / V_d, checked in float64
    // against its defining properties rather than its expression.
    const auto index = [&](format::float32 nanometers) {
        return static_cast<float64>(format::ComputeDielectricIndex(FLINT, nanometers));
    };
    const float64 indexD = static_cast<float64>(FLINT.IndexD);
    constexpr float64 INDEX_TOLERANCE = 1e-6;
    CHECK(magnitude(index(format::LUDUS_DIELECTRIC_D_LINE_NANOMETERS) - indexD) <= INDEX_TOLERANCE);
    CHECK(magnitude(index(format::LUDUS_DIELECTRIC_F_LINE_NANOMETERS) -
                    index(format::LUDUS_DIELECTRIC_C_LINE_NANOMETERS) -
                    (indexD - 1) / static_cast<float64>(FLINT.AbbeNumber)) <= INDEX_TOLERANCE);
    // (n - A) lambda^2 is the same constant B at every wavelength; normal dispersion makes blue refract more.
    const float64 lineD = static_cast<float64>(format::LUDUS_DIELECTRIC_D_LINE_NANOMETERS);
    const float64 cauchyA = indexD - (index(400) - indexD) / (lineD * lineD / (400.0 * 400.0) - 1);
    const float64 cauchyB = (index(400) - cauchyA) * 400.0 * 400.0;
    for (const format::float32 nanometers : {380.0F, 486.1327F, 550.0F, 656.2725F, 780.0F})
    {
        CAPTURE(nanometers);
        const float64 wavelength = static_cast<float64>(nanometers);
        CHECK(magnitude((index(nanometers) - cauchyA) * wavelength * wavelength - cauchyB) <= cauchyB * 1e-4);
    }
    CHECK(index(380) > index(550));
    CHECK(index(550) > index(780));
    // RGB transport (zero wavelength) and a nondispersive index both use n_d.
    CHECK(format::ComputeDielectricIndex(FLINT, 0) == FLINT.IndexD);
    CHECK(format::ComputeDielectricIndex({ .IndexD = 1.5F, .AbbeNumber = 0 }, 400) == 1.5F);
    // Admission: an Abbe number in [MIN, MAX] above an index of one, or zero; K[1..2] stay zero.
    CHECK(format::IsDielectricDispersionValid({ .IndexD = 1.5F, .AbbeNumber = format::MIN_DIELECTRIC_ABBE_NUMBER }) ==
          true);
    CHECK(format::IsDielectricDispersionValid({ .IndexD = 1.5F, .AbbeNumber = format::MAX_DIELECTRIC_ABBE_NUMBER }) ==
          true);
    CHECK(format::IsDielectricDispersionValid({ .IndexD = 0.8F, .AbbeNumber = 0 }) == true);
    for (const format::DielectricDispersion invalid : {
             format::DielectricDispersion{ .IndexD = 1.5F, .AbbeNumber = format::MIN_DIELECTRIC_ABBE_NUMBER * 0.5F },
             format::DielectricDispersion{ .IndexD = 1.5F, .AbbeNumber = format::MAX_DIELECTRIC_ABBE_NUMBER * 2 },
             format::DielectricDispersion{ .IndexD = 1.5F, .AbbeNumber = -30 },
             format::DielectricDispersion
             {
                 .IndexD = 1.5F,
                 .AbbeNumber = std::numeric_limits<format::float32>::quiet_NaN(),
             },
             format::DielectricDispersion{ .IndexD = 0.8F, .AbbeNumber = 30 },
             format::DielectricDispersion{ .IndexD = 1, .AbbeNumber = 30 },
         })
    {
        CAPTURE(invalid.IndexD, invalid.AbbeNumber);
        CHECK(format::IsDielectricDispersionValid(invalid) == false);
        CHECK(format::IsMaterialScatteringValid(
                  format::MakeDielectricScattering(format::MaterialModel::Dielectric, invalid)) == false);
    }
    format::MaterialScattering stray = format::MakeDielectricScattering(format::MaterialModel::Dielectric, FLINT);
    stray.K[1] = 1;
    CHECK(format::IsMaterialScatteringValid(stray) == false);
}

TEST_CASE("Material program decode rejects invalid extents without replacing the output", "[scene_format][materials]")
{
    const format::MaterialProgram original = MakeMixProgram();
    format::MaterialProgramBytes bytes{};
    format::EncodeMaterialProgram(original, bytes);

    format::MaterialProgram decoded{};
    REQUIRE(format::DecodeMaterialProgram(bytes, decoded) == true);
    REQUIRE(decoded.LobeCount == original.LobeCount);
    REQUIRE(decoded.InstructionCount == original.InstructionCount);

    format::MaterialProgram preserved = original;
    preserved.LobeCount = 1;
    REQUIRE_FALSE(format::DecodeMaterialProgram({nullptr, bytes.GetSize()}, preserved));
    REQUIRE_FALSE(format::DecodeMaterialProgram({bytes.GetData(), bytes.GetSize() - 1}, preserved));
    REQUIRE_FALSE(format::DecodeMaterialProgram({}, preserved));
    format::MaterialProgramBytes preservedBytes{};
    format::EncodeMaterialProgram(preserved, preservedBytes);
    format::MaterialProgram expected = original;
    expected.LobeCount = 1;
    format::MaterialProgramBytes expectedBytes{};
    format::EncodeMaterialProgram(expected, expectedBytes);
    CHECK(preservedBytes == expectedBytes);
}
