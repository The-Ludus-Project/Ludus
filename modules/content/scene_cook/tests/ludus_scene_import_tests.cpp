// Tests for the Ludus scene source importer (ludus-scene-v1) over the engine's
// checked-in test scenes (tests/scenes) and in-memory documents.

#include "import_test_support.h"

#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
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
using ludus::foundation::core::uint32;
using ludus::foundation::core::usize;
namespace codes = ludus::content::scene_cook::importers::codes;
namespace math = ludus::foundation::math;

/// A checked-in engine test scene below tests/scenes.
fs::path GetTestScene(std::string_view relative)
{
    return fs::path(LUDUS_TEST_SCENES_DIR) / relative;
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
        const std::string entry = GetTestScene(expected.File).string();
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
    const std::string entry = GetTestScene("diffraction/single-slit.luscene.json").string();
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

TEST_CASE("Ludus scene import can leave the slit's aperture to the importing World", "[scene_cook][ludus-scene]")
{
    const std::string entry = GetTestScene("diffraction/single-slit.luscene.json").string();
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
    const fs::path entry = scratch.Path / "room.luscene.json";
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
    REQUIRE(ImportSceneBestEffort(entry.string(), {}, result, error) == true);
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
    CHECK(ImportSceneBestEffort(entry.string(), {}, failed, error) == false);
    CHECK(error.Result == ludus::content::Status::Invalid);
    CHECK(ReportHasCode(failed, codes::MALFORMED_INPUT) == true);
    CHECK(GetReport(failed).find("objects[0].material: names no declared material") != std::string_view::npos);
}

TEST_CASE("Ludus scene import cooks closure programs into LUSCN version 11", "[scene_cook][ludus-scene][program]")
{
    namespace format = ludus::content::scene_format;
    ScratchDirectory scratch;
    const fs::path entry = scratch.Path / "program.luscene.json";
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
    REQUIRE(ImportSceneBestEffort(entry.string(), {}, result, error) == true);
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
