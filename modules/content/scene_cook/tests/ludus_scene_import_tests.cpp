// Tests for the Ludus scene source importer (ludus-scene-v1) over the engine's
// checked-in test scenes (tests/scenes) and in-memory documents.

#include "import_test_support.h"

#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/runtime/resources/scene.h>

#include "importers/import_diagnostics.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace
{
using namespace ludus::scene_import_tests;
using namespace ludus::content::scene_cook;
using ludus::foundation::core::float32;
using ludus::foundation::core::float64;
using ludus::foundation::core::uint32;
using ludus::foundation::core::usize;
namespace codes = ludus::content::scene_cook::importers::codes;
namespace math = ludus::foundation::math;

/// A checked-in engine test scene below tests/scenes.
std::string GetTestScene(std::string_view relative)
{
    return JoinPath(LUDUS_TEST_SCENES_DIR, relative);
}

std::string_view GetReport(const BestEffortResult& result)
{
    return {reinterpret_cast<const char*>(result.Report.GetData()), result.Report.GetSize()};
}

bool ReportHasCode(const BestEffortResult& result, std::string_view code)
{
    return GetReport(result).find("\"code\": \"" + std::string(code) + "\"") != std::string_view::npos;
}

struct ExpectedScene final
{
    std::string_view File;
    usize Triangles = 0;
};

/// The Cornell room camera the GPU tests use: eye 3.4 m in front of the origin, focal length 2.2.
constexpr float32 CORNELL_EYE_Z_WORLD = 3.4F;
constexpr float32 CORNELL_FOCAL_LENGTH = 2.2F;
} // namespace

TEST_CASE("Ludus scene import cooks the engine's occlusion test scenes", "[scene_cook][ludus-scene]")
{
    // Fans of the files' polygons: wall and light quads plus the slats (quads, clipped pentagons and triangles).
    constexpr usize SCENE_COUNT = 3;
    constexpr ludus::foundation::StaticArray<ExpectedScene, SCENE_COUNT> SCENES{{
        { .File = "light_occlusion/louvre-near-emitter.luscene.json", .Triangles = 34 },
        { .File = "light_occlusion/louvre-at-wall.luscene.json", .Triangles = 19 },
        { .File = "light_occlusion/occluded-penumbra.luscene.json", .Triangles = 8 },
    }};
    for (const ExpectedScene& expected : SCENES)
    {
        INFO(expected.File);
        const std::string entry = GetTestScene(expected.File);
        BestEffortResult first;
        BestEffortResult second;
        CookError error;
        REQUIRE(ImportSceneBestEffort(entry, {}, first, error) == true);
        REQUIRE(ImportLudusScene(entry, {}, second, error) == true);
        CHECK(first.Format == SceneSourceFormat::LudusScene);
        CHECK(first.Profile == LUDUS_SCENE_PROFILE);
        CHECK(first.Summary.Errors == 0);
        CHECK(first.Summary.SourceAreaLights == 1);
        REQUIRE(first.Bundle.GetSize() == second.Bundle.GetSize());
        CHECK(std::memcmp(first.Bundle.GetData(), second.Bundle.GetData(), first.Bundle.GetSize()) == 0);
        CHECK(GetReport(first).find("\"importer\": \"ludus-scene-v1\"") != std::string_view::npos);
        // Each light's authored surface (black or grey) becomes LUSCN's white rough emitter surface.
        CHECK(ReportHasCode(first, codes::MATERIAL_APPROXIMATED) == true);
        bool hasLicense = false;
        for (const ludus::foundation::String& input : first.Inputs)
        {
            hasLicense = hasLicense == true || input.GetView() == "LICENSE.txt";
        }
        CHECK(hasLicense == true);

        ludus::resources::SceneDefinition decoded;
        REQUIRE(ludus::resources::SceneDefinition::Decode(first.Bundle.GetData(), first.Bundle.GetSize(), decoded) ==
                ludus::resources::Status::Ok);
        CHECK(decoded.Eye == math::Vector3{0, 0, CORNELL_EYE_Z_WORLD});
        CHECK(decoded.Target == math::Vector3{0, 0, -1});
        CHECK(decoded.Up == math::Vector3{0, 1, 0});
        const float32 halfFov = decoded.VerticalFov / 2;
        CHECK(math::Abs(math::Sin(halfFov) / math::Cos(halfFov) * CORNELL_FOCAL_LENGTH - 1) < 1e-6F);

        usize emitters = 0;
        for (const ludus::resources::SceneObject& object : decoded.Objects)
        {
            const ludus::resources::SceneMaterial& material = decoded.Materials[object.Material];
            if (material.Emission.X == 0.0F)
            {
                continue;
            }
            ++emitters;
            CHECK(material.Emission == math::Vector3{6, 6, 6});
            CHECK(material.IsEmissionTwoSided == false);
            // The one-sided ceiling light faces down: its normals and its winding both point to -y.
            const ludus::resources::SceneMesh& mesh = decoded.Meshes[object.Mesh];
            REQUIRE(mesh.Indices.GetSize() >= 3);
            const math::Vector3 a = mesh.Positions[mesh.Indices[0]];
            const math::Vector3 b = mesh.Positions[mesh.Indices[1]];
            const math::Vector3 c = mesh.Positions[mesh.Indices[2]];
            CHECK(math::Cross(b - a, c - a).Y < 0.0F);
            for (const math::Vector3& normal : mesh.Normals)
            {
                CHECK(normal == math::Vector3{0, -1, 0});
            }
        }
        CHECK(emitters == 1);
        usize triangles = 0;
        for (const ludus::resources::SceneObject& object : decoded.Objects)
        {
            triangles += decoded.Meshes[object.Mesh].Indices.GetSize() / 3;
        }
        CHECK(triangles == expected.Triangles);
    }
}

TEST_CASE("Ludus scene import cooks the engine's single-slit diffraction scene", "[scene_cook][ludus-scene]")
{
    const std::string entry = GetTestScene("diffraction/single-slit.luscene.json");
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportSceneBestEffort(entry, {}, result, error) == true);
    CHECK(result.Format == SceneSourceFormat::LudusScene);
    CHECK(result.Summary.Errors == 0);
    CHECK(result.Summary.SourceAreaLights == 1);
    // The light's black surface becomes LUSCN's white rough emitter surface.
    CHECK(ReportHasCode(result, codes::MATERIAL_APPROXIMATED) == true);

    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    // The camera stands between the screen (z = 0) and the wall (z = -1), facing the wall.
    CHECK(decoded.Eye == math::Vector3{0, 0, -0.1F});
    CHECK(decoded.Target == math::Vector3{0, 0, -1});

    usize emitters = 0;
    usize triangles = 0;
    for (const ludus::resources::SceneObject& object : decoded.Objects)
    {
        const ludus::resources::SceneMesh& mesh = decoded.Meshes[object.Mesh];
        triangles += mesh.Indices.GetSize() / 3;
        const ludus::resources::SceneMaterial& material = decoded.Materials[object.Material];
        if (material.Emission.X == 0.0F)
        {
            continue;
        }
        ++emitters;
        // The cooked format's largest radiance, so the small coherent-enough light still lights the wall.
        CHECK(material.Emission == math::Vector3{10000, 10000, 10000});
        CHECK(material.IsEmissionTwoSided == false);
        // The one-sided light faces the screen and the wall behind it, towards -z.
        for (const math::Vector3& normal : mesh.Normals)
        {
            CHECK(normal == math::Vector3{0, 0, -1});
        }
    }
    CHECK(emitters == 1);
    // Wall and light quads plus the four screen quads around the slit.
    CHECK(triangles == 12);
}

TEST_CASE("Ludus scene import cooks the edge diffraction reference scenes", "[scene_cook][ludus-scene][diffraction]")
{
    // See the scene source guide for the Sommerfeld, conducting wedge and double-slit references.
    constexpr usize SCENE_COUNT = 3;
    constexpr usize QUAD_TRIANGLES = 2;
    constexpr ludus::foundation::StaticArray<ExpectedScene, SCENE_COUNT> SCENES{{
        { .File = "diffraction/knife-edge.luscene.json", .Triangles = 6 },
        { .File = "diffraction/right-angle-wedge.luscene.json", .Triangles = 8 },
        { .File = "diffraction/double-slit.luscene.json", .Triangles = 30 },
    }};
    for (const ExpectedScene& expected : SCENES)
    {
        INFO(expected.File);
        const std::string entry = GetTestScene(expected.File);
        BestEffortResult result;
        CookError error;
        REQUIRE(ImportSceneBestEffort(entry, {}, result, error) == true);
        CHECK(result.Summary.Errors == 0);
        CHECK(result.Summary.SourceAreaLights == 1);

        ludus::resources::SceneDefinition decoded;
        REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
                ludus::resources::Status::Ok);
        REQUIRE(decoded.Objects.GetSize() == 3);
        usize triangles = 0;
        usize emitters = 0;
        usize receivers = 0;
        for (const ludus::resources::SceneObject& object : decoded.Objects)
        {
            const ludus::resources::SceneMesh& mesh = decoded.Meshes[object.Mesh];
            REQUIRE(mesh.Positions.IsEmpty() == false);
            const usize meshTriangles = mesh.Indices.GetSize() / 3;
            triangles += meshTriangles;
            const ludus::resources::SceneMaterial& material = decoded.Materials[object.Material];
            if (material.Emission.X > 0)
            {
                ++emitters;
                CHECK(meshTriangles == QUAD_TRIANGLES);
                CHECK(material.IsEmissionTwoSided == false);
                for (const math::Vector3& normal : mesh.Normals)
                {
                    CHECK(normal == math::Vector3{0, 0, -1});
                }
                for (const math::Vector3& position : mesh.Positions)
                {
                    CHECK(position.Z == 0.5F);
                }
            }
            else if (mesh.Positions[0].Z == -1)
            {
                ++receivers;
                CHECK(meshTriangles == QUAD_TRIANGLES);
                for (const math::Vector3& normal : mesh.Normals)
                {
                    CHECK(normal == math::Vector3{0, 0, 1});
                }
                for (const math::Vector3& position : mesh.Positions)
                {
                    CHECK(position.Z == -1);
                }
            }
        }
        CHECK(emitters == 1);
        CHECK(receivers == 1);
        CHECK(triangles == expected.Triangles);
    }
}

TEST_CASE("Ludus scene import can leave the slit's aperture to the importing World", "[scene_cook][ludus-scene]")
{
    const std::string entry = GetTestScene("diffraction/single-slit.luscene.json");
    BestEffortOptions options;
    options.ShouldDeferApertures = true;
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportSceneBestEffort(entry, options, result, error) == true);
    CHECK(result.Summary.Errors == 0);
    CHECK(ReportHasCode(result, codes::APERTURE_DEFERRED) == true);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    usize triangles = 0;
    for (const ludus::resources::SceneObject& object : decoded.Objects)
    {
        triangles += decoded.Meshes[object.Mesh].Indices.GetSize() / 3;
    }
    // Only the wall and light quads remain; the editor generates the screen from its parameters.
    CHECK(triangles == 4);

    // Without the option the same file cooks its screen, and reports no deferral.
    BestEffortResult cooked;
    REQUIRE(ImportSceneBestEffort(entry, {}, cooked, error) == true);
    CHECK(ReportHasCode(cooked, codes::APERTURE_DEFERRED) == false);
}

TEST_CASE("Ludus scene import keeps materials and reports invalid documents", "[scene_cook][ludus-scene]")
{
    ScratchDirectory scratch;
    const std::string entry = scratch.GetPath("room.luscene.json");
    WriteFile(entry, R"({"format": "ludus.scene", "version": 1, "name": "Room",
        "materials": [
          {"name": "Floor", "model": "diffuse", "base_color": [0.25, 0.5, 0.75]},
          {"name": "Metal", "model": "metallic_roughness", "base_color": [0.9, 0.8, 0.7], "metallic": 1,
           "roughness": 0.3},
          {"name": "Lamp", "model": "metallic_roughness", "base_color": [1, 1, 1], "roughness": 1,
           "emission": {"radiance": [2, 4, 8], "two_sided": true}}],
        "objects": [
          {"name": "Floor", "material": "Floor", "positions": [[-1, 0, 1], [1, 0, 1], [1, 0, -1], [-1, 0, -1]],
           "polygons": [[0, 1, 2, 3]]},
          {"name": "Block", "material": "Metal",
           "positions": [[0, 0, 0], [0.5, 0, 0], [0.75, 0.25, 0], [0.5, 0.5, 0], [0, 0.5, 0]],
           "polygons": [[0, 1, 2, 3, 4]]},
          {"name": "Lamp", "material": "Lamp", "positions": [[0, 2, 0], [0, 2, 1], [1, 2, 0]],
           "polygons": [[0, 1, 2]]}]})");
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportSceneBestEffort(entry, {}, result, error) == true);
    // A white rough emitter is exactly what LUSCN keeps; no camera means a framing view.
    CHECK(ReportHasCode(result, codes::MATERIAL_APPROXIMATED) == false);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    bool hasFloor = false;
    bool hasMetal = false;
    bool hasLamp = false;
    for (const ludus::resources::SceneObject& object : decoded.Objects)
    {
        const ludus::resources::SceneMaterial& material = decoded.Materials[object.Material];
        const usize triangles = decoded.Meshes[object.Mesh].Indices.GetSize() / 3;
        if (material.Emission.X > 0.0F)
        {
            hasLamp = true;
            CHECK(material.Emission == math::Vector3{2, 4, 8});
            CHECK(material.IsEmissionTwoSided == true);
            CHECK(triangles == 1);
        }
        else if (material.Scattering.Model == ludus::content::scene_format::MaterialModel::Diffuse)
        {
            hasFloor = true;
            CHECK(material.Reflectance == math::Vector3{.25F, .5F, .75F});
            CHECK(triangles == 2);
        }
        else
        {
            hasMetal = true;
            CHECK(material.Reflectance == math::Vector3{.9F, .8F, .7F});
            CHECK(material.Metallic == 1.0F);
            CHECK(material.Roughness == .3F);
            // The pentagon becomes a fan of three triangles.
            CHECK(triangles == 3);
        }
    }
    CHECK(hasFloor == true);
    CHECK(hasMetal == true);
    CHECK(hasLamp == true);

    // A broken document fails with the reader's message in the report.
    WriteFile(entry, R"({"format": "ludus.scene", "version": 1, "name": "Room",
        "materials": [{"name": "Floor", "model": "diffuse", "base_color": [0.25, 0.5, 0.75]}],
        "objects": [{"name": "Floor", "material": "Ceiling", "positions": [[0, 0, 0], [1, 0, 0], [0, 0, 1]],
                     "polygons": [[0, 1, 2]]}]})");
    BestEffortResult failed;
    CHECK(ImportSceneBestEffort(entry, {}, failed, error) == false);
    CHECK(error.Result == ludus::content::Status::Invalid);
    CHECK(ReportHasCode(failed, codes::MALFORMED_INPUT) == true);
    CHECK(GetReport(failed).find("objects[0].material: names no declared material") != std::string_view::npos);
}

TEST_CASE("Ludus scene import cooks closure programs into LUSCN version 11", "[scene_cook][ludus-scene][program]")
{
    namespace format = ludus::content::scene_format;
    ScratchDirectory scratch;
    const std::string entry = scratch.GetPath("program.luscene.json");
    WriteFile(entry, R"({"format": "ludus.scene", "version": 1, "name": "Program room",
        "materials": [
          {"name": "Floor", "model": "diffuse", "base_color": [0.25, 0.5, 0.75]},
          {"name": "Rust", "model": "program", "base_color": [0.6, 0.3, 0.1], "metallic": 0.7, "roughness": 0.4,
           "program": {"version": 1,
             "parameters": [{"name": "metal", "input": "metallic"}, {"name": "rust", "one_minus": "metal"},
                            {"name": "rough", "input": "roughness"}],
             "lobes": [{"operation": "diffuse", "weight": "rust"},
                       {"operation": "conductor", "weight": "metal", "roughness": "rough",
                        "eta": [0.2, 0.9, 1.1], "k": [3.9, 2.4, 2.2]}]}}],
        "objects": [
          {"name": "Floor", "material": "Floor", "positions": [[-1, 0, 1], [1, 0, 1], [1, 0, -1], [-1, 0, -1]],
           "polygons": [[0, 1, 2, 3]]},
          {"name": "Block", "material": "Rust", "positions": [[0, 0, 0], [0.5, 0, 0], [0, 0.5, 0]],
           "polygons": [[0, 1, 2]]}]})");
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportSceneBestEffort(entry, {}, result, error) == true);
    uint32 version = 0;
    REQUIRE(ludus::foundation::core::TryReadLittleEndian<uint32>(
                {result.Bundle.GetData() + format::MAGIC.size(), sizeof(uint32)},
                version) == true);
    CHECK(version == format::VERSION_11);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    const ludus::resources::SceneMaterial* programOrNull = nullptr;
    for (const ludus::resources::SceneMaterial& material : decoded.Materials)
    {
        if (material.Scattering.Model == format::MaterialModel::Program)
        {
            programOrNull = &material;
        }
        else
        {
            CHECK(material.Program.LobeCount == 0);
        }
    }
    REQUIRE(programOrNull != nullptr);
    // The surface inputs are also the raster approximation.
    CHECK(programOrNull->Metallic == .7F);
    CHECK(programOrNull->Roughness == .4F);
    CHECK(programOrNull->Reflectance == math::Vector3{.6F, .3F, .1F});
    const format::MaterialProgram& program = programOrNull->Program;
    REQUIRE(format::IsMaterialProgramValid(program) == true);
    CHECK(program.InstructionCount == 3);
    REQUIRE(program.LobeCount == 2);
    CHECK(program.Lobes[0].Operation == format::MaterialLobeOperation::Diffuse);
    CHECK(program.Lobes[1].Operation == format::MaterialLobeOperation::Conductor);
    CHECK(program.Lobes[1].Eta[2] == 1.1F);
    CHECK(program.Lobes[1].K[0] == 3.9F);

    // Locate the record by its leading version and counts, then corrupt one opcode and one
    // padding byte; the decoder rejects both and preserves its output.
    const ludus::foundation::StaticArray<uint32, format::MATERIAL_PROGRAM_HEADER_FIELDS> header{
        {program.Version, program.ConstantCount, program.InstructionCount, program.LobeCount}};
    std::string bytes(reinterpret_cast<const char*>(result.Bundle.GetData()), result.Bundle.GetSize());
    std::string pattern;
    for (const uint32 field : header)
    {
        for (usize byte = 0; byte < sizeof(uint32); ++byte)
        {
            pattern += static_cast<char>((field >> (8 * byte)) & 0xFFU);
        }
    }
    const usize record = bytes.find(pattern);
    REQUIRE(record != std::string::npos);
    REQUIRE(bytes.find(pattern, record + 1) == std::string::npos);
    const usize firstInstruction = record + offsetof(format::MaterialProgram, Instructions);
    for (const usize offset : {firstInstruction + offsetof(format::MaterialProgramInstruction, Opcode),
                               firstInstruction + offsetof(format::MaterialProgramInstruction, Reserved)})
    {
        std::string corrupted = bytes;
        corrupted[offset] = static_cast<char>(200);
        ludus::resources::SceneDefinition rejected;
        CHECK(ludus::resources::SceneDefinition::Decode(
                  reinterpret_cast<const ludus::foundation::uint8*>(corrupted.data()),
                  corrupted.size(),
                  rejected) == ludus::resources::Status::Failed);
        CHECK(rejected.Materials.IsEmpty() == true);
    }
    // A truncated record fails too.
    ludus::resources::SceneDefinition truncated;
    CHECK(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(),
                                                    record + format::MATERIAL_PROGRAM_BYTES / 2,
                                                    truncated) == ludus::resources::Status::Failed);
}

TEST_CASE("Ludus scene import cooks the engine's closure-program test scene", "[scene_cook][ludus-scene][program]")
{
    namespace format = ludus::content::scene_format;
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportSceneBestEffort(GetTestScene("materials/closure-program-block.luscene.json"), {}, result, error) ==
            true);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    usize programs = 0;
    usize triangles = 0;
    for (const ludus::resources::SceneObject& object : decoded.Objects)
    {
        const ludus::resources::SceneMaterial& material = decoded.Materials[object.Material];
        triangles += decoded.Meshes[object.Mesh].Indices.GetSize() / 3;
        if (material.Scattering.Model != format::MaterialModel::Program)
        {
            continue;
        }
        ++programs;
        // The block's six quads; its program's three lobes, and the authored surface inputs.
        CHECK(decoded.Meshes[object.Mesh].Indices.GetSize() / 3 == 12);
        CHECK(material.Program.LobeCount == 3);
        CHECK(material.Program.Lobes[2].Operation == format::MaterialLobeOperation::CosinePower);
        CHECK(material.Metallic == 0.7F);
        CHECK(format::SupportsFixedDimensionReflection(material.Scattering, material.Program) == true);
    }
    CHECK(programs == 1);
    CHECK(triangles == 16);
}

TEST_CASE("Ludus scene import cooks the engine's grain-mix test scene", "[scene_cook][ludus-scene][program]")
{
    namespace format = ludus::content::scene_format;
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportSceneBestEffort(GetTestScene("materials/grain-mix-board.luscene.json"), {}, result, error) == true);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    usize programs = 0;
    for (const ludus::resources::SceneObject& object : decoded.Objects)
    {
        const ludus::resources::SceneMaterial& material = decoded.Materials[object.Material];
        if (material.Scattering.Model != format::MaterialModel::Program)
        {
            continue;
        }
        ++programs;
        // One record mixes a diffuse lobe weighted by the complement of the base colour mask with a conductor
        // weighted by the mask; the renderer tests load the same record through both transport graphs.
        CHECK(material.Program.InstructionCount == 3);
        CHECK(material.Program.ConstantCount == 0);
        REQUIRE(material.Program.LobeCount == 2);
        CHECK(material.Program.Lobes[0].Operation == format::MaterialLobeOperation::Diffuse);
        CHECK(material.Program.Lobes[1].Operation == format::MaterialLobeOperation::Conductor);
        CHECK(material.Program.Instructions[1].Opcode == format::MaterialProgramOpcode::OneMinus);
        CHECK(material.Program.Lobes[0].WeightRegister == material.Program.Instructions[1].Destination);
        CHECK(material.Program.Lobes[1].WeightRegister == material.Program.Instructions[0].Destination);
        CHECK(format::SupportsFixedDimensionReflection(material.Scattering, material.Program) == true);
    }
    CHECK(programs == 1);
}

TEST_CASE("Ludus scene import cooks the engine's hair-card test scene into LUSCN version 13",
          "[scene_cook][ludus-scene][hair]")
{
    namespace format = ludus::content::scene_format;
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportSceneBestEffort(GetTestScene("materials/hair-card-patch.luscene.json"), {}, result, error) == true);
    uint32 version = 0;
    REQUIRE(ludus::foundation::core::TryReadLittleEndian<uint32>(
                {result.Bundle.GetData() + format::MAGIC.size(), sizeof(uint32)},
                version) == true);
    CHECK(version == format::VERSION_13);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    usize hairCards = 0;
    usize controls = 0;
    for (const ludus::resources::SceneObject& object : decoded.Objects)
    {
        const ludus::resources::SceneMaterial& material = decoded.Materials[object.Material];
        if (material.Scattering.Model == format::MaterialModel::MetallicRoughness && material.Name.View() == "Control")
        {
            ++controls;
        }
        if (material.Scattering.Model != format::MaterialModel::HairCard)
        {
            continue;
        }
        ++hairCards;
        const format::HairCardParameters hair = format::GetHairCardParameters(material.Scattering);
        CHECK(hair.StrandDirectionDegrees == 90);
        CHECK(hair.PrimaryShiftDegrees == -5);
        CHECK(hair.SecondaryShiftDegrees == 7.5F);
        CHECK(hair.PrimaryWeight == 0.1F);
        CHECK(hair.SecondaryWeight == 0.05F);
        CHECK(material.Metallic == 0);
        CHECK(material.Roughness == 0.35F);
    }
    CHECK(hairCards == 1);
    CHECK(controls == 1);
}

TEST_CASE("Ludus scene import preserves a tilted polygon's flat normal far from the origin",
          "[scene_cook][ludus-scene]")
{
    // Width and height are binary fractions. Both sets lie on a plane with normal (-1, -1, 1).
    constexpr std::string_view LOCAL_POSITIONS = R"([[0, 0, 0], [0.4453125, 0, 0.4453125],
        [0.4453125, 0.47265625, 0.91796875], [0, 0.47265625, 0.47265625]])";
    constexpr std::string_view OFFSET_POSITIONS = R"([[900000.0146484375, 900000.0205078125, 900000.0048828125],
        [900000.4599609375, 900000.0205078125, 900000.4501953125],
        [900000.4599609375, 900000.4931640625, 900000.9228515625],
        [900000.0146484375, 900000.4931640625, 900000.4775390625]])";
    constexpr float32 NORMAL_TOLERANCE = 1e-6F;
    const math::Vector3 forwardNormal = math::Vector3{-1, -1, 1} * (1.0F / math::Sqrt(3.0F));
    const ScratchDirectory scratch;
    const std::string entry = scratch.GetPath("tilted.luscene.json");

    for (const std::string_view positions : {LOCAL_POSITIONS, OFFSET_POSITIONS})
    {
        for (const bool isReversed : {false, true})
        {
            INFO(positions);
            INFO(isReversed);
            std::string document = R"({"format":"ludus.scene","version":1,"name":"Tilted quad",
              "materials":[{"name":"Wall","model":"diffuse","base_color":[1,1,1]}],
              "objects":[{"name":"Quad","material":"Wall","positions":)";
            document += positions;
            document += isReversed == true ? R"(,"polygons":[[0,3,2,1]]}]})" : R"(,"polygons":[[0,1,2,3]]}]})";
            WriteFile(entry, document);
            BestEffortResult result;
            CookError error;
            const bool hasImported = ImportSceneBestEffort(entry, {}, result, error);
            INFO(error.Message.View());
            REQUIRE(hasImported == true);
            ludus::resources::SceneDefinition decoded;
            REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(),
                                                              result.Bundle.GetSize(),
                                                              decoded) == ludus::resources::Status::Ok);
            REQUIRE(decoded.Meshes.GetSize() == 1);
            const ludus::resources::SceneMesh& mesh = decoded.Meshes[0];
            CHECK(mesh.Indices.GetSize() == 6);
            REQUIRE(mesh.Normals.GetSize() == 4);
            const math::Vector3 expectedNormal = isReversed == true ? -forwardNormal : forwardNormal;
            for (const math::Vector3& normal : mesh.Normals)
            {
                CHECK(math::Length(normal - expectedNormal) < NORMAL_TOLERANCE);
            }
        }
    }
}

TEST_CASE("Scene detection tells Ludus scene sources from glTF JSON", "[scene_cook][ludus-scene]")
{
    CHECK(DetectSceneSourceFormat(AsBytes("{\n  \"format\": \"ludus.scene\",\n  \"version\": 1"), "a.json") ==
          SceneSourceFormat::LudusScene);
    CHECK(DetectSceneSourceFormat(AsBytes("\xEF\xBB\xBF{\"format\":\"ludus.scene\"}"), "a") ==
          SceneSourceFormat::LudusScene);
    CHECK(DetectSceneSourceFormat(AsBytes("{\"asset\": {\"version\": \"2.0\"}}"), "a.luscene.json") ==
          SceneSourceFormat::Gltf);
    CHECK(DetectSceneSourceFormat(AsBytes("{\"version\": 1, \"format\": \"ludus.scene\"}"), "a.gltf") ==
          SceneSourceFormat::Gltf);
    CHECK(DetectSceneSourceFormat({}, "room.LUSCENE.JSON") == SceneSourceFormat::LudusScene);
}

TEST_CASE("Ludus scene import cooks an absorbing medium boundary into LUSCN version 12",
          "[scene_cook][ludus-scene][medium]")
{
    namespace format = ludus::content::scene_format;
    ScratchDirectory scratch;
    const std::string entry = scratch.GetPath("medium.luscene.json");
    // A unit cube of absorber in front of a wall; each face is wound counterclockwise seen from outside.
    WriteFile(entry, R"({"format": "ludus.scene", "version": 1, "name": "Ink cube",
        "materials": [
          {"name": "Wall", "model": "diffuse", "base_color": [0.25, 0.5, 0.75]},
          {"name": "Ink", "model": "medium_interface", "medium": {"type": "homogeneous", "absorption": [0.5, 1, 2]}}],
        "objects": [
          {"name": "Wall", "material": "Wall", "positions": [[-2, -2, 2], [2, -2, 2], [2, 2, 2], [-2, 2, 2]],
           "polygons": [[0, 3, 2, 1]]},
          {"name": "Cube", "material": "Ink",
           "positions": [[-0.5, -0.5, -0.5], [0.5, -0.5, -0.5], [0.5, 0.5, -0.5], [-0.5, 0.5, -0.5],
                         [-0.5, -0.5, 0.5], [0.5, -0.5, 0.5], [0.5, 0.5, 0.5], [-0.5, 0.5, 0.5]],
           "polygons": [[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4], [3, 7, 6, 2], [0, 4, 7, 3], [1, 2, 6, 5]]}]})");
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportSceneBestEffort(entry, {}, result, error) == true);
    uint32 version = 0;
    REQUIRE(ludus::foundation::core::TryReadLittleEndian<uint32>(
                {result.Bundle.GetData() + format::MAGIC.size(), sizeof(uint32)},
                version) == true);
    CHECK(version == format::VERSION_12);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    const ludus::resources::SceneMaterial* mediumOrNull = nullptr;
    for (const ludus::resources::SceneMaterial& material : decoded.Materials)
    {
        if (material.Scattering.Model == format::MaterialModel::MediumInterface)
        {
            mediumOrNull = &material;
        }
    }
    REQUIRE(mediumOrNull != nullptr);
    CHECK(mediumOrNull->Scattering.K[0] == .5F);
    CHECK(mediumOrNull->Scattering.K[1] == 1);
    CHECK(mediumOrNull->Scattering.K[2] == 2);
    CHECK(mediumOrNull->Emission == math::Vector3{});
    // The boundary keeps its twelve outward triangles with bit-identical shared corners.
    usize boundaryTriangles = 0;
    for (const ludus::resources::SceneObject& object : decoded.Objects)
    {
        if (decoded.Materials[object.Material].Scattering.Model == format::MaterialModel::MediumInterface)
        {
            const ludus::resources::SceneMesh& mesh = decoded.Meshes[object.Mesh];
            boundaryTriangles += mesh.Indices.GetSize() / 3;
            for (usize first = 0; first < mesh.Indices.GetSize(); first += 3)
            {
                const math::Vector3 a = mesh.Positions[mesh.Indices[first]];
                const math::Vector3 b = mesh.Positions[mesh.Indices[first + 1]];
                const math::Vector3 c = mesh.Positions[mesh.Indices[first + 2]];
                const math::Vector3 centroid = (a + b + c) * (1.F / 3);
                CHECK(math::Dot(math::Cross(b - a, c - a), centroid) > 0); // Outward.
            }
        }
    }
    CHECK(boundaryTriangles == 12);

    // An unsupported medium type fails the import with the reader's explicit message.
    WriteFile(entry, R"({"format": "ludus.scene", "version": 1, "name": "Fog",
        "materials": [{"name": "Fog", "model": "medium_interface", "medium": {"type": "nanovdb", "file": "fog.nvdb"}}],
        "objects": [{"name": "Fog", "material": "Fog", "positions": [[0, 0, 0], [1, 0, 0], [0, 0, 1]],
                     "polygons": [[0, 1, 2]]}]})");
    BestEffortResult failed;
    CHECK(ImportSceneBestEffort(entry, {}, failed, error) == false);
    CHECK(ReportHasCode(failed, codes::MALFORMED_INPUT) == true);
    CHECK(GetReport(failed).find("is an unsupported medium type") != std::string_view::npos);
}

TEST_CASE("Ludus scene import cooks the engine's skin bench into LUSCN version 14", "[scene_cook][ludus-scene][skin]")
{
    namespace format = ludus::content::scene_format;
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportSceneBestEffort(GetTestScene("materials/skin-diffusion-bench.luscene.json"), {}, result, error) ==
            true);
    CHECK(result.Summary.Errors == 0);
    uint32 version = 0;
    REQUIRE(ludus::foundation::core::TryReadLittleEndian<uint32>(
                {result.Bundle.GetData() + format::MAGIC.size(), sizeof(uint32)},
                version) == true);
    CHECK(version == format::VERSION_14);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    usize skinObjects = 0;
    usize otherObjects = 0;
    for (const ludus::resources::SceneObject& object : decoded.Objects)
    {
        const ludus::resources::SceneMaterial& material = decoded.Materials[object.Material];
        if (material.Scattering.Model != format::MaterialModel::Skin)
        {
            ++otherObjects;
            continue;
        }
        ++skinObjects;
        CHECK(material.Name.View() == "Skin");
        CHECK(material.Scattering.K[0] == 0.012F);
        CHECK(material.Scattering.K[1] == 0.006F);
        CHECK(material.Scattering.K[2] == 0.003F);
        CHECK(material.Metallic == 0);
        CHECK(math::Abs(material.Roughness - 0.45F) < 1e-6F);
        CHECK(material.Reflectance == math::Vector3{0.8F, 0.5F, 0.4F});
        CHECK(format::IsMaterialScatteringValid(material.Scattering) == true);
    }
    // The skin block sits beside a non-skin block, under a non-skin bar, on a floor, under a light.
    CHECK(skinObjects == 1);
    CHECK(otherObjects == 4);

    // Skin is a dielectric: the decoder rejects a stored metallic fraction. The record's metallic and
    // roughness fields are adjacent, and only the skin material has roughness 0.45.
    constexpr float32 SKIN_ROUGHNESS = 0.45F;
    constexpr float32 EDITED_VALUE = 0.5F;
    const ludus::foundation::StaticArray<float32, 2> metallicAndRoughness{{0, SKIN_ROUGHNESS}};
    ludus::foundation::StaticArray<ludus::foundation::core::uint8, 2 * sizeof(float32)> pattern{};
    std::memcpy(pattern.GetData(), metallicAndRoughness.GetData(), pattern.GetSize());
    usize metallicAt = result.Bundle.GetSize();
    for (usize at = 0; at + pattern.GetSize() <= result.Bundle.GetSize(); ++at)
    {
        if (std::memcmp(result.Bundle.GetData() + at, pattern.GetData(), pattern.GetSize()) == 0)
        {
            REQUIRE(metallicAt == result.Bundle.GetSize());
            metallicAt = at;
        }
    }
    REQUIRE(metallicAt < result.Bundle.GetSize());
    // Control: editing the adjacent roughness still decodes, so the rejection below is the metallic check.
    ludus::foundation::Array<ludus::foundation::core::uint8> edited;
    REQUIRE(edited.TryResize(result.Bundle.GetSize()) == true);
    std::memcpy(edited.GetData(), result.Bundle.GetData(), edited.GetSize());
    std::memcpy(edited.GetData() + metallicAt + sizeof(float32), &EDITED_VALUE, sizeof(float32));
    CHECK(ludus::resources::SceneDefinition::Decode(edited.GetData(), edited.GetSize(), decoded) ==
          ludus::resources::Status::Ok);
    std::memcpy(edited.GetData(), result.Bundle.GetData(), edited.GetSize());
    std::memcpy(edited.GetData() + metallicAt, &EDITED_VALUE, sizeof(float32));
    CHECK(ludus::resources::SceneDefinition::Decode(edited.GetData(), edited.GetSize(), decoded) !=
          ludus::resources::Status::Ok);
}

TEST_CASE("The engine's character bust imports with its maps, licence and metre scale", "[scene_cook][gltf][skin]")
{
    namespace format = ludus::content::scene_format;
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportSceneBestEffort(GetTestScene("characters/infinite-head/infinite-head.gltf"), {}, result, error) ==
            true);
    CHECK(result.Format == SceneSourceFormat::Gltf);
    CHECK(result.Summary.Errors == 0);
    // One mesh in one material slot, the slot an artist assigns skin to.
    constexpr uint32 HEAD_TRIANGLES = 17684;
    CHECK(result.Summary.SourceIndices == HEAD_TRIANGLES * 3);
    CHECK(result.Summary.CookedMaterials == 1);
    CHECK(ReportHasCode(result, codes::TANGENTS_GENERATED) == true);
    bool hasLicense = false;
    for (const ludus::foundation::String& input : result.Inputs)
    {
        hasLicense = hasLicense == true || input.GetView() == "LICENSE.txt";
    }
    CHECK(hasLicense == true);

    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    REQUIRE(decoded.Materials.GetSize() == 1);
    const ludus::resources::SceneMaterial& slot = decoded.Materials[0];
    CHECK(slot.Name.View() == "Skin");
    CHECK(slot.Scattering.Model == format::MaterialModel::MetallicRoughness);
    CHECK(slot.Metallic == 0);
    CHECK(slot.Texture != format::NO_TEXTURE);
    CHECK(slot.SurfaceTexture != format::NO_TEXTURE);
    CHECK(slot.NormalScale > 0);
    // About 17 cm wide in metres, so authored scattering distances are in skin's millimetre range.
    constexpr float32 MIN_WIDTH_WORLD = 0.15F;
    constexpr float32 MAX_WIDTH_WORLD = 0.19F;
    float32 minimumX = 0;
    float32 maximumX = 0;
    bool hasPosition = false;
    for (const ludus::resources::SceneObject& object : decoded.Objects)
    {
        for (const math::Vector3& positionMesh : decoded.Meshes[object.Mesh].Positions)
        {
            const math::Vector3 positionWorld = math::TransformPoint(object.Transform, positionMesh);
            minimumX = hasPosition == true ? math::Min(minimumX, positionWorld.X) : positionWorld.X;
            maximumX = hasPosition == true ? math::Max(maximumX, positionWorld.X) : positionWorld.X;
            hasPosition = true;
        }
    }
    REQUIRE(hasPosition == true);
    CHECK(maximumX - minimumX > MIN_WIDTH_WORLD);
    CHECK(maximumX - minimumX < MAX_WIDTH_WORLD);

    // glTF texture coordinates put v = 0 at the top of the image, and the colour map
    // has the crown at the top and the shoulders at the bottom. Height must therefore
    // fall as v grows; the scan's original bottom-up UVs gave the opposite sign and
    // drew the lips below the chin.
    constexpr float64 MAX_HEIGHT_TO_V_CORRELATION = -0.5;
    REQUIRE(decoded.Meshes.GetSize() == 1);
    const ludus::resources::SceneMesh& head = decoded.Meshes[0];
    REQUIRE(head.Texcoords.GetSize() == head.Positions.GetSize());
    REQUIRE(head.Positions.GetSize() > 0);
    const float64 count = static_cast<float64>(head.Positions.GetSize());
    float64 meanHeight = 0;
    float64 meanV = 0;
    for (usize i = 0; i < head.Positions.GetSize(); ++i)
    {
        meanHeight += static_cast<float64>(head.Positions[i].Y) / count;
        meanV += static_cast<float64>(head.Texcoords[i].Y) / count;
    }
    float64 covariance = 0;
    float64 heightVariance = 0;
    float64 vVariance = 0;
    for (usize i = 0; i < head.Positions.GetSize(); ++i)
    {
        const float64 height = static_cast<float64>(head.Positions[i].Y) - meanHeight;
        const float64 v = static_cast<float64>(head.Texcoords[i].Y) - meanV;
        covariance += height * v;
        heightVariance += height * height;
        vVariance += v * v;
    }
    REQUIRE(heightVariance > 0);
    REQUIRE(vVariance > 0);
    CHECK(covariance / math::Sqrt(heightVariance * vVariance) < MAX_HEIGHT_TO_V_CORRELATION);
}
