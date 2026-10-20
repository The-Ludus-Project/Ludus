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
using ludus::foundation::core::int32;
using ludus::foundation::core::uint32;
using ludus::foundation::core::usize;

/// Read OBJ text with optional in-memory companion files.
Status ReadWavefront(std::string_view text,
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
    // map_Pr and the norm map repack into a LUSCN v6 surface map instead of averaging.
    CHECK(ReportHasCode(first, codes::TEXTURE_APPROXIMATED) == false);
    usize emissive = 0;
    usize surfaceMapped = 0;
    for (const ludus::resources::SceneMaterial& material : decoded.Materials)
    {
        emissive += material.Emission.X > 0.0F ? 1 : 0;
        if (material.SurfaceTexture != ludus::content::scene_format::NO_TEXTURE)
        {
            ++surfaceMapped;
            // BoxPbr: Pr 0.5 stays the multiplier of map_Pr's green 128 channel (-imfchan g).
            CHECK(material.Roughness == .5F);
            CHECK(material.NormalScale == 1.0F);
            const ludus::resources::SceneTexture& surface = decoded.Textures[material.SurfaceTexture];
            CHECK(surface.IsLinear == true);
            CHECK(surface.Pixels[0] == 128); // Flat normal X from normal.png (128, 128, 255).
            CHECK(surface.Pixels[2] == 128); // Roughness multiplier.
            CHECK(surface.Pixels[3] == 255); // No metallic texture: multiplier one.
        }
    }
    CHECK(emissive == 1);
    CHECK(surfaceMapped == 1);
    // The normal-mapped box carries MikkTSpace tangents in its vertices.
    bool hasTangent = false;
    for (const ludus::resources::SceneMesh& mesh : decoded.Meshes)
    {
        for (const ludus::foundation::math::Vector4& tangent : mesh.Tangents)
        {
            hasTangent = hasTangent == true || tangent.W != 0.0F;
        }
    }
    CHECK(hasTangent == true);
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
        REQUIRE(ReadWavefront("v 0 0 0\nv 1 0 0\nv 2 1 0\nv 1 2 0\nv 0 1 0\nf 1 2 3 4 5\n", scene, log) == Status::Ok);
        REQUIRE(scene.Meshes.GetSize() == 1);
        CHECK(scene.Meshes[0].Indices.GetSize() == 9);
        CHECK(scene.Meshes[0].GetVertexCount() == 5);
    }
    SECTION("unsmoothed faces keep their own corners; a smoothing group shares them")
    {
        const std::string_view quad = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 1\nf 1 2 3\nf 1 3 4\n";
        SceneIr flat;
        DiagnosticLog flatLog;
        REQUIRE(ReadWavefront("s off\n" + std::string(quad), flat, flatLog) == Status::Ok);
        CHECK(flat.Meshes[0].GetVertexCount() == 6);
        SceneIr smooth;
        DiagnosticLog smoothLog;
        REQUIRE(ReadWavefront("s 1\n" + std::string(quad), smooth, smoothLog) == Status::Ok);
        CHECK(smooth.Meshes[0].GetVertexCount() == 4);
    }
    SECTION("explicit normals weld equal v/vt/vn corners whatever the smoothing group")
    {
        // Exporters often write vn without any s statement; the corners are still shared.
        const std::string_view quad = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\n"
                                      "f 1/1/1 2/1/1 3/1/1\nf 1/1/1 3/1/1 4/1/1\n";
        for (const std::string_view prefix : {std::string_view(""), std::string_view("s off\n")})
        {
            SceneIr scene;
            DiagnosticLog log;
            REQUIRE(ReadWavefront(std::string(prefix) + std::string(quad), scene, log) == Status::Ok);
            CHECK(scene.Meshes[0].GetVertexCount() == 4);
            CHECK(scene.Meshes[0].Indices.GetSize() == 6);
            CHECK(scene.Meshes[0].Normals.GetSize() == usize{4} * 3);
        }
    }
    SECTION("repeated positions are removed and reported")
    {
        SceneIr scene;
        DiagnosticLog log;
        REQUIRE(ReadWavefront("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 1 1 2\n", scene, log) == Status::Ok);
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
        REQUIRE(ReadWavefront(text, scene, log) == Status::Ok);
        CHECK(HasCode(log, codes::PRIMITIVE_SKIPPED));
        CHECK(scene.Meshes[0].Indices.GetSize() == 3);
    }
    SECTION("mixed attributes are made uniform per group with a diagnostic")
    {
        SceneIr scene;
        DiagnosticLog log;
        REQUIRE(ReadWavefront("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvt 0 0\nvn 0 0 1\nf 1/1/1 2/1/1 3/1/1\nf 2 4 3\n",
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
        CHECK(ReadWavefront(item.Text, scene, log) == Status::Invalid);
        CHECK(HasCode(log, item.Code));
        REQUIRE(log.GetEntries().IsEmpty() == false);
        CHECK(log.GetEntries()[0].File.GetView() == "memory.obj");
    }
    // An overlong line is a located error, not an allocation.
    SceneIr scene;
    DiagnosticLog log;
    const std::string overlong = "v 0 0 0" + std::string(obj::MAX_LINE_BYTES, ' ') + "\n";
    CHECK(ReadWavefront(overlong, scene, log) == Status::Invalid);
    CHECK(HasCode(log, codes::MALFORMED_INPUT));
    // Line continuations join physical lines and keep the first line number.
    SceneIr joined;
    DiagnosticLog joinedLog;
    CHECK(ReadWavefront("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 \\\n 2 3\n", joined, joinedLog) == Status::Ok);
    CHECK(joined.Meshes[0].Indices.GetSize() == 3);
}

TEST_CASE("OBJ dependencies stay inside the import root and report problems", "[scene_cook][obj]")
{
    const std::string_view mtl = "newmtl Red\nKd 1 0 0\nmap_Kd ../outside.png\nmap_Ks spec.png\nKa 1 1 1\nfoo 1\n";
    const MemorySourceFile files[] = {{"lib.mtl", AsBytes(mtl)}};
    SceneIr scene;
    DiagnosticLog log;
    REQUIRE(ReadWavefront("mtllib lib.mtl missing.mtl ../escape.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
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
    CHECK(ReadWavefront(text, scene, log, {}, &cancel) == Status::Cancelled);
}

TEST_CASE("OBJ surface maps beyond the texture count are averaged into scalar terms", "[scene_cook][obj]")
{
    namespace format = ludus::content::scene_format;
    // One base texture per slot leaves room for one surface map; the rest are averaged.
    constexpr int32 MATERIAL_COUNT = static_cast<int32>(format::IMPORT_LIMITS.Textures) - 1;
    ScratchDirectory scratch;
    std::filesystem::create_directories(scratch.Path / "textures");
    std::filesystem::copy_file(GetFixture("obj/room/textures/rough.png"), scratch.Path / "textures/rough.png");
    std::string wavefrontText = "mtllib m.mtl\n";
    std::string mtl;
    for (int32 i = 0; i < MATERIAL_COUNT; ++i)
    {
        mtl += "newmtl M" + std::to_string(i) + "\nKd 0.5 0.5 0.5\nPr 0.5\nmap_Pr -imfchan g textures/rough.png\n";
        const std::string x = std::to_string(i * 2);
        wavefrontText +=
            "v " + x + " 0 0\nv " + x + ".5 0 0\nv " + x + " 1 0\nusemtl M" + std::to_string(i) + "\nf -3 -2 -1\n";
    }
    WriteFile(scratch.Path / "scene.obj", wavefrontText);
    WriteFile(scratch.Path / "m.mtl", mtl);
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportObjBestEffort((scratch.Path / "scene.obj").string(), {}, result, error) == true);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    CHECK(decoded.Textures.GetSize() == format::IMPORT_LIMITS.Textures);
    usize mapped = 0;
    for (const ludus::resources::SceneMaterial& material : decoded.Materials)
    {
        if (material.SurfaceTexture != format::NO_TEXTURE)
        {
            ++mapped;
            CHECK(material.Roughness == .5F);
            continue;
        }
        // Averaged: Pr 0.5 times the green channel's mean 128/255.
        CHECK(std::fabs(material.Roughness - .5F * 128.0F / 255.0F) < 1e-6F);
    }
    CHECK(mapped == 1);
    CHECK(ReportHasCode(result, codes::TEXTURE_APPROXIMATED));
}

TEST_CASE("OBJ map_d cooks LUSCN v5 alpha masks into the base texture", "[scene_cook][obj]")
{
    namespace format = ludus::content::scene_format;
    constexpr usize ALPHA = 3;
    constexpr usize TEXEL_BYTES = format::TEXEL_BYTES;
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportObjBestEffort(GetFixture("obj/cutout/cutout.obj").string(), {}, result, error) == true);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    CHECK(result.Summary.Errors == 0);
    CHECK(ReportHasCode(result, codes::ALPHA_APPROXIMATED)); // map_d read at the 0.5 cutoff.
    // Slots follow world area: Leaf (4), Halo (2), Solid (1); each slot owns its texture.
    REQUIRE(decoded.Materials.GetSize() == 3);

    // Leaf: the 4x4 grey mask is sampled nearest at the 8x8 base resolution.
    const ludus::resources::SceneMaterial& leaf = decoded.Materials[0];
    CHECK(leaf.IsAlphaMasked == true);
    CHECK(leaf.AlphaCutoff == .5F);
    const ludus::resources::SceneTexture& leafTexture = decoded.Textures[leaf.Texture];
    REQUIRE(leafTexture.Width == 8);
    for (uint32 x = 0; x < leafTexture.Width; ++x)
    {
        CHECK(leafTexture.Pixels[x * TEXEL_BYTES + ALPHA] == (x < 4 ? 0 : 255));
    }
    CHECK(leafTexture.Pixels[0] == 230); // Base colour is unchanged.

    // Halo: an untextured red swatch takes the 2x2 mask's resolution and alpha channel.
    const ludus::resources::SceneMaterial& halo = decoded.Materials[1];
    CHECK(halo.IsAlphaMasked == true);
    const ludus::resources::SceneTexture& haloTexture = decoded.Textures[halo.Texture];
    REQUIRE(haloTexture.Width == 2);
    REQUIRE(haloTexture.Height == 2);
    CHECK(haloTexture.Pixels[0] == 255);
    CHECK(haloTexture.Pixels[1] == 0);
    CHECK(haloTexture.Pixels[ALPHA] == 0);
    CHECK(haloTexture.Pixels[2 * TEXEL_BYTES + ALPHA] == 255);

    // Solid: a mask that keeps every texel cooks opaque.
    CHECK(decoded.Materials[2].IsAlphaMasked == false);
    CHECK(decoded.Materials[2].AlphaCutoff == 0);
}

TEST_CASE("OBJ budget: excess materials merge and decorative emitters drop instead of failing", "[scene_cook][obj]")
{
    namespace format = ludus::content::scene_format;
    // More materials, and more distinct emitters, than the version 5 import profile has slots.
    constexpr int32 SLOTS = static_cast<int32>(format::IMPORT_LIMITS.Materials);
    constexpr int32 MATERIAL_COUNT = SLOTS + 12;
    constexpr int32 EMITTER_COUNT = SLOTS + 2;
    ScratchDirectory scratch;
    std::string wavefrontText = "mtllib m.mtl\n";
    std::string mtl;
    for (int32 i = 0; i < MATERIAL_COUNT; ++i)
    {
        const std::string x = std::to_string(i * 2);
        mtl += "newmtl M" + std::to_string(i) + "\nKd 0." + std::to_string(i % 10) + " 0.5 0.5\n";
        // Distinct emitters exceed the emitter slots; they are decorative in OBJ.
        if (i < EMITTER_COUNT)
        {
            mtl += "Ke " + std::to_string(i + 1) + " 1 1\n";
        }
        wavefrontText += "v ";
        wavefrontText += x;
        wavefrontText += " 0 0\nv ";
        wavefrontText += x;
        wavefrontText += ".5 0 0\nv ";
        wavefrontText += x;
        wavefrontText += " 1 0\nusemtl M";
        wavefrontText += std::to_string(i);
        wavefrontText += "\nf -3 -2 -1\n";
    }
    WriteFile(scratch.Path / "scene.obj", wavefrontText);
    WriteFile(scratch.Path / "m.mtl", mtl);
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportObjBestEffort((scratch.Path / "scene.obj").string(), {}, result, error) == true);
    CHECK(result.Summary.CookedMaterials <= format::IMPORT_LIMITS.Meshes);
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

TEST_CASE("OBJ review regressions: overlong numbers, huge illum, map_Pm, many groups", "[scene_cook][obj]")
{
    SECTION("a decimal word longer than the parser bound is malformed, not an overflow")
    {
        SceneIr scene;
        DiagnosticLog log;
        CHECK(ReadWavefront("v " + std::string(20000, '1') + " 0 0\n", scene, log) == Status::Invalid);
        CHECK(HasCode(log, codes::MALFORMED_INPUT));
    }
    SECTION("illum outside the int range is clamped")
    {
        const std::string_view mtl = "newmtl A\nillum 1e10\nnewmtl B\nillum -1e10\n";
        const MemorySourceFile files[] = {{"a.mtl", AsBytes(mtl)}};
        SceneIr scene;
        DiagnosticLog log;
        CHECK(ReadWavefront("mtllib a.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl A\nf 1 2 3\n", scene, log, files) ==
              Status::Ok);
    }
    SECTION("map_Pm without Pm carries the metalness itself")
    {
        const std::string_view mtl = "newmtl Metal\nmap_Pm metal.png\nnewmtl Scaled\nPm 0.25\nmap_Pm metal.png\n";
        const MemorySourceFile files[] = {{"a.mtl", AsBytes(mtl)}};
        SceneIr scene;
        DiagnosticLog log;
        REQUIRE(ReadWavefront("mtllib a.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Metal\nf 1 2 3\n", scene, log, files) ==
                Status::Ok);
        REQUIRE(scene.Materials.GetSize() == 2);
        CHECK(scene.Materials[0].Metallic == 1.0);
        CHECK(scene.Materials[0].MetallicTexture != IR_NONE);
        CHECK(scene.Materials[1].Metallic == 0.25);
    }
    SECTION("alternating groups resolve through the name index")
    {
        std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
        for (int i = 0; i < 20000; ++i)
        {
            text += "g g";
            text += std::to_string(i % 5000);
            text += "\nf 1 2 3\n";
        }
        SceneIr scene;
        DiagnosticLog log;
        REQUIRE(ReadWavefront(text, scene, log) == Status::Ok);
        CHECK(scene.Meshes.GetSize() == 5000);
        CHECK(scene.Meshes[0].Indices.GetSize() == 12); // Four faces of g0, welded into one mesh.
    }
}
