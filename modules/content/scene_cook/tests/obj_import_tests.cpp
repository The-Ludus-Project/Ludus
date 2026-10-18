// Tests for the best-effort Wavefront OBJ/MTL importer (obj-best-effort-v1).
// Checked-in fixtures live in fixtures/obj (see fixtures/generate_scene_fixtures.py);
// malformed and limit cases are generated in memory.

#include "import_test_support.h"

#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/runtime/resources/scene.h>

#include "importers/obj/obj_reader.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace
{
using namespace ludus::scene_import_tests;
using namespace ludus::content::scene_cook;
using namespace ludus::content::scene_cook::importers;
using ludus::content::Status;
using ludus::foundation::core::float32;
using ludus::foundation::core::float64;
using ludus::foundation::core::uint32;
using ludus::foundation::core::usize;

/// Read OBJ text with optional in-memory companion files.
Status ReadObj(std::string_view text,
               SceneIr& outScene,
               DiagnosticLog& outLog,
               std::span<const MemorySourceFile> files = {},
               const Cancellation* cancelOrNull = nullptr)
{
    MemoryByteSource source(AsBytes(text));
    MemorySourceResolver resolver(files);
    return obj::ReadScene(source, "memory.obj", resolver, outScene, outLog, cancelOrNull);
}

bool HasCode(const DiagnosticLog& log, std::string_view code)
{
    return log.CountOccurrences(code) != 0;
}

bool ReportHasCode(const BestEffortResult& result, std::string_view code)
{
    const std::string_view report(reinterpret_cast<const char*>(result.Report.GetData()), result.Report.GetSize());
    return report.find("\"code\": \"" + std::string(code) + "\"") != std::string_view::npos;
}

std::string_view GetReport(const BestEffortResult& result)
{
    return {reinterpret_cast<const char*>(result.Report.GetData()), result.Report.GetSize()};
}

const IrMaterial* FindMaterial(const SceneIr& scene, std::string_view name)
{
    for (const IrMaterial& material : scene.Materials)
    {
        if (material.Name.GetView() == name)
        {
            return &material;
        }
    }
    return nullptr;
}

bool CancelImmediately(void*) noexcept
{
    return true;
}
} // namespace

TEST_CASE("OBJ fixture reads groups, relative indices and MTL terms into SceneIR", "[scene_cook][obj]")
{
    const fs::path root = fs::canonical(GetFixture("obj/room"));
    const std::string rootText = root.string(); // The resolver borrows its root.
    NativeSourceResolver resolver(rootText);
    const std::string text = ReadFileText(root / "room.obj");
    MemoryByteSource source(AsBytes(text));
    SceneIr scene;
    DiagnosticLog log;
    REQUIRE(obj::ReadScene(source, "room.obj", resolver, scene, log) == Status::Ok);
    REQUIRE(scene.SourceFiles.GetSize() == 2);
    CHECK(scene.SourceFiles[1].GetView() == "room.mtl");
    REQUIRE(scene.Meshes.GetSize() == 4);
    REQUIRE(scene.WorldShapes.GetSize() == 4);

    // Floor: one quad fan-triangulated over four welded corners, vt flipped to top-down.
    const IrMesh& floor = scene.Meshes[0];
    CHECK(floor.GetVertexCount() == 4);
    CHECK(floor.Indices.GetSize() == 6);
    CHECK(floor.Normals.GetSize() == 12);
    REQUIRE(floor.Uvs.GetSize() == 8);
    bool hasFlippedOrigin = false;
    for (usize v = 0; v < 4; ++v)
    {
        hasFlippedOrigin = hasFlippedOrigin || (floor.Uvs[v * 2] == 0.0F && floor.Uvs[v * 2 + 1] == 1.0F);
    }
    CHECK(hasFlippedOrigin);

    // Mirror wall: negative indices, no vt/vn, so normals are left to the cook.
    const IrMesh& wall = scene.Meshes[1];
    CHECK(wall.GetVertexCount() == 4);
    CHECK(wall.Normals.IsEmpty());
    CHECK(wall.Uvs.IsEmpty());
    for (usize v = 0; v < 4; ++v)
    {
        CHECK(wall.Positions[v * 3 + 2] == -2.0F); // All corners of the back wall.
    }

    // Phong mapping: a black-diffuse, strong-specular material becomes a conductor.
    const IrMaterial* mirror = FindMaterial(scene, "Mirror");
    REQUIRE(mirror != nullptr);
    CHECK(mirror->Metallic == 1.0);
    CHECK(mirror->BaseColour[0] == static_cast<float64>(0.9F)); // Decimals round to binary32.
    CHECK(std::fabs(mirror->Roughness - std::pow(2.0 / 1002.0, 0.25)) < 1e-12);
    CHECK(mirror->IsApproximated);

    const IrMaterial* checker = FindMaterial(scene, "Checker");
    REQUIRE(checker != nullptr);
    REQUIRE(checker->BaseColourTexture != IR_NONE);
    const IrTexture& texture = scene.Textures[checker->BaseColourTexture];
    CHECK(texture.File.GetView() == "textures/checker.png"); // '\' normalised.
    CHECK(texture.Encoding == TextureEncoding::Srgb);
    CHECK(texture.IsRepeated);
    CHECK(HasCode(log, codes::PATH_NORMALIZED));

    const IrMaterial* light = FindMaterial(scene, "Light");
    REQUIRE(light != nullptr);
    CHECK(light->Emission[0] == 17.0);
    const IrShape& lightShape = scene.Shapes[2];
    CHECK(lightShape.IsEmissionOptional);
    CHECK(lightShape.Emission[1] == 12.0);
    CHECK(scene.Lights.Area == 1);

    // PBR extension keys win over the Phong mapping.
    const IrMaterial* box = FindMaterial(scene, "BoxPbr");
    REQUIRE(box != nullptr);
    CHECK(box->Roughness == 0.5);
    CHECK(box->Metallic == 0.0);
    CHECK(box->IsApproximated == false);
    CHECK(box->RoughnessTexture != IR_NONE);
    CHECK(box->RoughnessChannel == 1); // -imfchan g
    CHECK(scene.Textures[box->RoughnessTexture].Encoding == TextureEncoding::Linear);
    CHECK(box->NormalTexture != IR_NONE);
    CHECK(box->NormalTextureKind == IrNormalTextureKind::TangentSpace);
    CHECK(box->Opacity == 0.5);
    CHECK(box->AlphaMode == IrAlphaMode::Blend);
    CHECK(HasCode(log, codes::PRIMITIVE_SKIPPED)); // The `l` record.
}

TEST_CASE("OBJ import cooks a deterministic, runtime-decodable bundle", "[scene_cook][obj]")
{
    const std::string entry = GetFixture("obj/room/room.obj").string();
    BestEffortResult first;
    BestEffortResult second;
    CookError error;
    REQUIRE(ImportObjBestEffort(entry, {}, first, error) == true);
    REQUIRE(ImportSceneBestEffort(entry, {}, second, error) == true);
    CHECK(first.Format == SceneSourceFormat::Obj);
    CHECK(second.Profile == OBJ_BEST_EFFORT_PROFILE);
    REQUIRE(first.Bundle.GetSize() == second.Bundle.GetSize());
    CHECK(std::memcmp(first.Bundle.GetData(), second.Bundle.GetData(), first.Bundle.GetSize()) == 0);
    CHECK(GetReport(first) == GetReport(second));
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(first.Bundle.GetData(), first.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    CHECK(first.Summary.Errors == 0);
    CHECK(first.Summary.CookedMaterials == decoded.Materials.GetSize());
    CHECK(first.Summary.SourceAreaLights == 1);
    CHECK(GetReport(first).find("\"importer\": \"obj-best-effort-v1\"") != std::string_view::npos);
    CHECK(ReportHasCode(first, codes::TANGENTS_GENERATED));
    CHECK(ReportHasCode(first, codes::TEXTURE_APPROXIMATED)); // map_Pr averaged.
    usize emissive = 0;
    for (const ludus::resources::SceneMaterial& material : decoded.Materials)
    {
        emissive += material.Emission.X > 0.0F ? 1 : 0;
    }
    CHECK(emissive == 1);
    // Inputs list the entry, the library, the textures and the licence.
    const auto contains = [&](std::string_view name) {
        for (const ludus::foundation::String& input : first.Inputs)
        {
            if (input.GetView() == name)
            {
                return true;
            }
        }
        return false;
    };
    REQUIRE(first.Inputs.IsEmpty() == false);
    CHECK(first.Inputs[0].GetView() == "room.obj");
    CHECK(contains("room.mtl"));
    CHECK(contains("textures/checker.png"));
    CHECK(contains("LICENSE.txt"));

    ScratchDirectory scratch;
    REQUIRE(WriteBestEffortImport((scratch.Path / "out").string(), first, error) == true);
    CHECK(fs::file_size(scratch.Path / "out/scene.luscn") == first.Bundle.GetSize());
    CHECK(fs::exists(scratch.Path / "out/import-report.json"));
    CHECK(ReadFileText(scratch.Path / "out/LICENSE.txt") == ReadFileText(GetFixture("obj/room/LICENSE.txt")));
}

TEST_CASE("OBJ faces: fan triangulation, smoothing groups and degenerate corners", "[scene_cook][obj]")
{
    SECTION("a pentagon becomes three triangles")
    {
        SceneIr scene;
        DiagnosticLog log;
        REQUIRE(ReadObj("v 0 0 0\nv 1 0 0\nv 2 1 0\nv 1 2 0\nv 0 1 0\nf 1 2 3 4 5\n", scene, log) == Status::Ok);
        REQUIRE(scene.Meshes.GetSize() == 1);
        CHECK(scene.Meshes[0].Indices.GetSize() == 9);
        CHECK(scene.Meshes[0].GetVertexCount() == 5);
    }
    SECTION("unsmoothed faces keep their own corners; a smoothing group shares them")
    {
        const std::string_view quad = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 1\nf 1 2 3\nf 1 3 4\n";
        SceneIr flat;
        DiagnosticLog flatLog;
        REQUIRE(ReadObj("s off\n" + std::string(quad), flat, flatLog) == Status::Ok);
        CHECK(flat.Meshes[0].GetVertexCount() == 6);
        SceneIr smooth;
        DiagnosticLog smoothLog;
        REQUIRE(ReadObj("s 1\n" + std::string(quad), smooth, smoothLog) == Status::Ok);
        CHECK(smooth.Meshes[0].GetVertexCount() == 4);
    }
    SECTION("repeated positions are removed and reported")
    {
        SceneIr scene;
        DiagnosticLog log;
        REQUIRE(ReadObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 1 1 2\n", scene, log) == Status::Ok);
        CHECK(scene.Meshes[0].Indices.GetSize() == 3);
        CHECK(HasCode(log, codes::GEOMETRY_REPAIRED));
    }
    SECTION("polygons beyond 64 corners are skipped, not truncated")
    {
        std::string text;
        std::string face = "f";
        for (int i = 1; i <= 70; ++i)
        {
            text += "v " + std::to_string(i) + " " + std::to_string(i % 7) + " 0\n";
            face += " " + std::to_string(i);
        }
        text += face + "\nf 1 2 3\n";
        SceneIr scene;
        DiagnosticLog log;
        REQUIRE(ReadObj(text, scene, log) == Status::Ok);
        CHECK(HasCode(log, codes::PRIMITIVE_SKIPPED));
        CHECK(scene.Meshes[0].Indices.GetSize() == 3);
    }
    SECTION("mixed attributes are made uniform per group with a diagnostic")
    {
        SceneIr scene;
        DiagnosticLog log;
        REQUIRE(ReadObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvt 0 0\nvn 0 0 1\nf 1/1/1 2/1/1 3/1/1\nf 2 4 3\n",
                        scene,
                        log) == Status::Ok);
        CHECK(log.CountOccurrences(codes::ATTRIBUTE_IGNORED) == 2);
        CHECK(scene.Meshes[0].Normals.IsEmpty());
        CHECK(scene.Meshes[0].Uvs.GetSize() == scene.Meshes[0].GetVertexCount() * 2);
    }
}

TEST_CASE("OBJ malformed input fails with located errors", "[scene_cook][obj]")
{
    struct Case
    {
        std::string_view Text;
        std::string_view Code;
    };
    const Case cases[] = {
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 0\n", codes::MALFORMED_INPUT},       // Zero index.
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 4\n", codes::MALFORMED_INPUT},       // Past the end.
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\nf -4 -2 -1\n", codes::MALFORMED_INPUT},    // Before the start.
        {"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/5 2 3\n", codes::MALFORMED_INPUT},     // Missing vt.
        {"v 0 0 x\n", codes::MALFORMED_INPUT},                                  // Bad number.
        {"v 0 0\n", codes::MALFORMED_INPUT},                                    // Too few components.
        {"v 0 0 1e999\n", codes::MALFORMED_INPUT},                              // Overflow.
        {"# comment only\n", codes::LIMIT_EXCEEDED},                            // Nothing drawable.
        {std::string_view("v 0 0 0\n\0f 1 1 1\n", 17), codes::MALFORMED_INPUT}, // NUL byte.
    };
    for (const Case& item : cases)
    {
        INFO(item.Text);
        SceneIr scene;
        DiagnosticLog log;
        CHECK(ReadObj(item.Text, scene, log) == Status::Invalid);
        CHECK(HasCode(log, item.Code));
        REQUIRE(log.GetEntries().IsEmpty() == false);
        CHECK(log.GetEntries()[0].File.GetView() == "memory.obj");
    }
    // An overlong line is a located error, not an allocation.
    SceneIr scene;
    DiagnosticLog log;
    const std::string overlong = "v 0 0 0" + std::string(obj::MAX_LINE_BYTES, ' ') + "\n";
    CHECK(ReadObj(overlong, scene, log) == Status::Invalid);
    CHECK(HasCode(log, codes::MALFORMED_INPUT));
    // Line continuations join physical lines and keep the first line number.
    SceneIr joined;
    DiagnosticLog joinedLog;
    CHECK(ReadObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 \\\n 2 3\n", joined, joinedLog) == Status::Ok);
    CHECK(joined.Meshes[0].Indices.GetSize() == 3);
}

TEST_CASE("OBJ dependencies stay inside the import root and report problems", "[scene_cook][obj]")
{
    const std::string_view mtl = "newmtl Red\nKd 1 0 0\nmap_Kd ../outside.png\nmap_Ks spec.png\nKa 1 1 1\nfoo 1\n";
    const MemorySourceFile files[] = {{"lib.mtl", AsBytes(mtl)}};
    SceneIr scene;
    DiagnosticLog log;
    REQUIRE(ReadObj("mtllib lib.mtl missing.mtl ../escape.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                    "usemtl Red\nf 1 2 3\nusemtl Blue\nf 1 2 3\n",
                    scene,
                    log,
                    files) == Status::Ok);
    CHECK(log.CountOccurrences(codes::DEPENDENCY_UNREADABLE) == 2);
    CHECK(HasCode(log, codes::TEXTURE_UNREADABLE)); // ../outside.png
    CHECK(HasCode(log, codes::NAME_UNDEFINED));     // usemtl Blue
    CHECK(HasCode(log, codes::UNKNOWN_DIRECTIVE));  // foo
    REQUIRE(scene.Materials.GetSize() == 1);
    CHECK(scene.Materials[0].BaseColourTexture == IR_NONE);
    CHECK(scene.Materials[0].SpecularTexture != IR_NONE);
    REQUIRE(scene.Shapes.GetSize() == 2);
    CHECK(scene.Shapes[0].Material == 0);
    CHECK(scene.Shapes[1].Material == IR_NONE);
}

TEST_CASE("OBJ reading honours cancellation", "[scene_cook][obj]")
{
    std::string text;
    for (int i = 0; i < 10000; ++i)
    {
        text += "v 0 0 0\n";
    }
    const Cancellation cancel{CancelImmediately, nullptr};
    SceneIr scene;
    DiagnosticLog log;
    CHECK(ReadObj(text, scene, log, {}, &cancel) == Status::Cancelled);
}

TEST_CASE("OBJ budget: excess materials merge and decorative emitters drop instead of failing", "[scene_cook][obj]")
{
    ScratchDirectory scratch;
    std::string obj = "mtllib m.mtl\n";
    std::string mtl;
    for (int i = 0; i < 12; ++i)
    {
        const std::string x = std::to_string(i * 2);
        mtl += "newmtl M" + std::to_string(i) + "\nKd 0." + std::to_string(i % 10) + " 0.5 0.5\n";
        // Eight distinct emitters exceed the emitter slots; they are decorative in OBJ.
        if (i < 8)
        {
            mtl += "Ke " + std::to_string(i + 1) + " 1 1\n";
        }
        obj += "v ";
        obj += x;
        obj += " 0 0\nv ";
        obj += x;
        obj += ".5 0 0\nv ";
        obj += x;
        obj += " 1 0\nusemtl M";
        obj += std::to_string(i);
        obj += "\nf -3 -2 -1\n";
    }
    WriteFile(scratch.Path / "scene.obj", obj);
    WriteFile(scratch.Path / "m.mtl", mtl);
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportObjBestEffort((scratch.Path / "scene.obj").string(), {}, result, error) == true);
    CHECK(result.Summary.CookedMaterials <= ludus::content::scene_format::MAX_MESHES);
    CHECK(ReportHasCode(result, codes::EMISSION_DROPPED));
    CHECK(ReportHasCode(result, codes::MATERIALS_MERGED));
    CHECK(result.Summary.Errors == 0);
    // A dry run plans without decoding or packing.
    BestEffortResult dry;
    BestEffortOptions options;
    options.IsDryRun = true;
    REQUIRE(ImportObjBestEffort((scratch.Path / "scene.obj").string(), options, dry, error) == true);
    CHECK(dry.Bundle.IsEmpty());
    CHECK(GetReport(dry).find("\"dry_run\": true") != std::string_view::npos);
}

TEST_CASE("scene format detection uses content before extensions", "[scene_cook][obj][gltf]")
{
    CHECK(DetectSceneSourceFormat(AsBytes("# exported\nv 0 0 0\n"), "a.txt") == SceneSourceFormat::Obj);
    CHECK(DetectSceneSourceFormat(AsBytes("mtllib a.mtl\n"), "scene.pbrt") == SceneSourceFormat::Obj);
    CHECK(DetectSceneSourceFormat(AsBytes("LookAt 0 0 0  0 0 1  0 1 0\n"), "scene.obj") == SceneSourceFormat::Pbrt);
    CHECK(DetectSceneSourceFormat(AsBytes("\xEF\xBB\xBF  {\"asset\":{}}"), "scene") == SceneSourceFormat::Gltf);
    CHECK(DetectSceneSourceFormat(AsBytes("glTF\x02\x00\x00\x00"), "scene.bin") == SceneSourceFormat::Gltf);
    CHECK(DetectSceneSourceFormat(AsBytes("# only a comment\n"), "x.OBJ") == SceneSourceFormat::Obj);
    CHECK(DetectSceneSourceFormat(AsBytes("# only a comment\n"), "x.glb") == SceneSourceFormat::Gltf);
    CHECK(DetectSceneSourceFormat(AsBytes("\x89PNG"), "x.png") == SceneSourceFormat::Unknown);
    BestEffortResult result;
    CookError error;
    ScratchDirectory scratch;
    WriteFile(scratch.Path / "image.png", "\x89PNG\r\n");
    CHECK(ImportSceneBestEffort((scratch.Path / "image.png").string(), {}, result, error) == false);
    CHECK(error.Result == Status::Unsupported);
    CHECK(ImportSceneBestEffort((scratch.Path / "missing.obj").string(), {}, result, error) == false);
    CHECK(error.Result == Status::NotFound);
}
