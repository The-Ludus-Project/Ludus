// Tests for the best-effort glTF 2.0 / GLB importer (gltf-best-effort-v1).
// Checked-in fixtures live in fixtures/gltf (see fixtures/generate_scene_fixtures.py);
// malformed, unsupported and limit cases are generated in memory.

#include "import_test_support.h"

#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/runtime/resources/scene.h>

#include "importers/gltf/gltf_reader.h"
#include "importers/transform_utilities.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
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

Status ReadGltf(std::string_view bytes,
                SceneIr& outScene,
                DiagnosticLog& outLog,
                std::span<const MemorySourceFile> files = {},
                const Cancellation* cancelOrNull = nullptr)
{
    MemorySourceResolver resolver(files);
    return gltf::ReadScene(AsBytes(bytes), "memory.gltf", resolver, outScene, outLog, cancelOrNull);
}

bool HasCode(const DiagnosticLog& log, std::string_view code)
{
    return log.CountOccurrences(code) != 0;
}

/// A minimal valid asset: one triangle from a base64 buffer, with optional extra
/// top-level members and replacement mesh/accessor JSON.
std::string
MakeTriangle(std::string_view extra = {}, std::string_view accessorExtra = {}, std::string_view primitiveExtra = {})
{
    // Three float32 positions: (0,0,0) (1,0,0) (0,1,0).
    const std::string buffer = "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA";
    return std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)") +
           R"("meshes":[{"primitives":[{"attributes":{"POSITION":0})" + std::string(primitiveExtra) + "}]}]," +
           R"("buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,)" + buffer + R"("}],)" +
           R"("bufferViews":[{"buffer":0,"byteLength":36}],)" +
           R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3")" + std::string(accessorExtra) +
           "}]" + std::string(extra) + "}";
}

bool CancelImmediately(void*) noexcept
{
    return true;
}

float64 GetDeterminant(const ludus::foundation::math::Matrix4d& matrix)
{
    return ComputeLinearDeterminant(matrix);
}
} // namespace

TEST_CASE("glTF alphaMode MASK cooks a LUSCN v5 mask; BLEND stays opaque", "[scene_cook][gltf]")
{
    namespace format = ludus::content::scene_format;
    constexpr usize ALPHA = 3;
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportGltfBestEffort(GetFixture("gltf/cutout/scene.gltf").string(), {}, result, error) == true);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    const std::string_view report(reinterpret_cast<const char*>(result.Report.GetData()), result.Report.GetSize());
    CHECK(report.find("\"code\": \"ALPHA_APPROXIMATED\"") != std::string_view::npos); // BLEND.
    REQUIRE(decoded.Materials.GetSize() == 2);
    usize maskedCount = 0;
    for (const ludus::resources::SceneMaterial& material : decoded.Materials)
    {
        if (material.IsAlphaMasked == false)
        {
            CHECK(material.AlphaCutoff == 0);
            continue;
        }
        ++maskedCount;
        CHECK(material.AlphaCutoff == .25F);
        // Texture alpha 0, 64, 128, 255 times baseColorFactor alpha 0.5.
        const ludus::resources::SceneTexture& texture = decoded.Textures[material.Texture];
        REQUIRE(texture.Pixels.GetSize() == usize{4} * format::TEXEL_BYTES);
        const uint32 expected[] = {0, 32, 64, 128};
        for (usize i = 0; i < 4; ++i)
        {
            CHECK(texture.Pixels[i * format::TEXEL_BYTES + ALPHA] == expected[i]);
        }
    }
    CHECK(maskedCount == 1);
}

TEST_CASE("glTF normal, roughness and emissive maps cook into LUSCN v6 maps with tangents", "[scene_cook][gltf]")
{
    namespace format = ludus::content::scene_format;
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportGltfBestEffort(GetFixture("gltf/maps/scene.gltf").string(), {}, result, error) == true);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    REQUIRE(decoded.Materials.GetSize() == 1);
    const ludus::resources::SceneMaterial& material = decoded.Materials[0];
    // The decorative emitter keeps its material's terms and maps.
    CHECK(material.Emission.X > 0.0F);
    CHECK(material.Roughness == .5F);
    REQUIRE(material.EmissionTexture != format::NO_TEXTURE);
    const ludus::resources::SceneTexture& emission = decoded.Textures[material.EmissionTexture];
    CHECK(emission.IsLinear == false);
    REQUIRE(emission.Pixels.GetSize() == usize{2} * format::TEXEL_BYTES);
    CHECK(emission.Pixels[0] == 255);
    CHECK(emission.Pixels[format::TEXEL_BYTES] == 0);
    REQUIRE(material.SurfaceTexture != format::NO_TEXTURE);
    CHECK(material.NormalScale == .5F);
    const ludus::resources::SceneTexture& surface = decoded.Textures[material.SurfaceTexture];
    CHECK(surface.IsLinear == true);
    CHECK(surface.Pixels[0] == 200); // Normal X and Y from the normal map.
    CHECK(surface.Pixels[1] == 128);
    CHECK(surface.Pixels[2] == 255); // No roughness or metallic texture: multipliers one.
    CHECK(surface.Pixels[3] == 255);
    // No TANGENT attribute: MikkTSpace tangents are generated and stored per vertex.
    REQUIRE(decoded.Meshes[0].Tangents.GetSize() == decoded.Meshes[0].Positions.GetSize());
    for (const ludus::foundation::math::Vector4& tangent : decoded.Meshes[0].Tangents)
    {
        CHECK(std::fabs(tangent.W) == 1.0F);
        CHECK(std::fabs(tangent.X) > .99F); // u increases along +x.
    }
    const std::string_view report(reinterpret_cast<const char*>(result.Report.GetData()), result.Report.GetSize());
    CHECK(report.find("NORMALMAP_DROPPED") == std::string_view::npos);
    CHECK(report.find("TANGENTS_GENERATED") != std::string_view::npos);
}

TEST_CASE("glTF fixture keeps hierarchy, instancing, cameras and punctual lights", "[scene_cook][gltf]")
{
    const fs::path root = fs::canonical(GetFixture("gltf/instanced"));
    const std::string rootText = root.string(); // The resolver borrows its root.
    NativeSourceResolver resolver(rootText);
    const std::string text = ReadFileText(root / "scene.gltf");
    SceneIr scene;
    DiagnosticLog log;
    REQUIRE(gltf::ReadScene(AsBytes(text), "scene.gltf", resolver, scene, log) == Status::Ok);
    CHECK(log.HasErrors() == false);
    REQUIRE(scene.DataFiles.GetSize() == 1);
    CHECK(scene.DataFiles[0].GetView() == "scene.bin");

    // The cube mesh is placed by two nodes: one object, two instances, one mirrored.
    REQUIRE(scene.Objects.GetSize() == 1);
    REQUIRE(scene.Instances.GetSize() == 2);
    CHECK(GetDeterminant(scene.Instances[0].Transform) > 0.0);
    CHECK(GetDeterminant(scene.Instances[1].Transform) < 0.0);
    // CubeA translation (-1, 0.5, 0); the mirrored cube sits under a 90-degree Y rotation.
    CHECK(scene.Instances[0].Transform.Elements[12] == -1.0);
    CHECK(std::fabs(scene.Instances[1].Transform.Elements[14] + 1.0) < 1e-12);
    CHECK(scene.WorldShapes.GetSize() == 3); // Floor, lamp, strip.

    // Interleaved cube: 24 vertices, uint8 indices, normals and uvs from stride 32.
    const IrMesh& cube = scene.Meshes[scene.Shapes[scene.Objects[0].Shapes[0]].Mesh];
    CHECK(cube.GetVertexCount() == 24);
    CHECK(cube.Indices.GetSize() == 36);
    CHECK(cube.Normals.GetSize() == 72);
    CHECK(cube.Uvs.GetSize() == 48);
    CHECK(cube.Normals[2] == 1.0F); // First face points +Z.

    // The four-vertex strip becomes two triangles (0 1 2) (2 1 3) with consistent
    // winding; without NORMAL every corner gets its own vertex for flat normals.
    bool hasStrip = false;
    for (const uint32 shape : scene.WorldShapes)
    {
        const IrMesh& mesh = scene.Meshes[scene.Shapes[shape].Mesh];
        if (mesh.GetVertexCount() == 6 && mesh.Indices.GetSize() == 6 && mesh.Positions[0] == 2.0F)
        {
            hasStrip = true;
            CHECK(mesh.Normals.IsEmpty());
            const float32 expectedX[6] = {2.0F, 2.0F, 2.5F, 2.5F, 2.0F, 2.5F};
            const float32 expectedY[6] = {0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F};
            for (usize corner = 0; corner < 6; ++corner)
            {
                CHECK(mesh.Indices[corner] == corner);
                CHECK(mesh.Positions[corner * 3] == expectedX[corner]);
                CHECK(mesh.Positions[corner * 3 + 1] == expectedY[corner]);
            }
        }
    }
    CHECK(hasStrip);

    REQUIRE(scene.Camera.IsPresent);
    CHECK(scene.Camera.Eye.Z == 6.0);
    CHECK(scene.Camera.Target.Z == 5.0);
    CHECK(scene.Camera.VerticalFov == 0.8);
    CHECK(scene.Camera.AspectRatio == 1.5);
    CHECK(scene.Camera.FarClip == 100.0);

    REQUIRE(scene.AnalyticLights.GetSize() == 2);
    const IrAnalyticLight& spot = scene.AnalyticLights[0];
    CHECK(spot.Kind == IrLightKind::Spot);
    CHECK(spot.PositionWorld.Y == 3.0);
    CHECK(std::fabs(spot.DirectionWorld.Y + 1.0) < 1e-12); // -Z rotated to point down.
    CHECK(std::fabs(spot.InnerAngleDegrees - 0.3 * 180.0 / 3.141592653589793) < 1e-9);
    CHECK(spot.Scale == 50.0); // White: intensity x luminance(1, 1, 1).
    CHECK(scene.AnalyticLights[1].Kind == IrLightKind::Directional);
    CHECK(scene.Lights.Spot == 1);
    CHECK(scene.Lights.Distant == 1);

    // Materials: metallic-roughness texture channels, emissive strength, defaults.
    bool hasMetal = false;
    bool hasGlow = false;
    bool hasDefault = false;
    for (const IrMaterial& material : scene.Materials)
    {
        if (material.Name.GetView() == "Metal")
        {
            hasMetal = true;
            CHECK(material.MetallicTexture == material.RoughnessTexture);
            CHECK(material.MetallicChannel == 2);
            CHECK(material.RoughnessChannel == 1);
            CHECK(material.IsDoubleSided);
            CHECK(material.NormalTexture != IR_NONE);
            CHECK(scene.Textures[material.MetallicTexture].EmbeddedImage != IR_NONE); // Data URI.
        }
        if (material.Name.GetView() == "Glow")
        {
            hasGlow = true;
            CHECK(material.Emission[0] == 4.0);
            CHECK(material.Emission[2] == 1.0);
            CHECK(material.AlphaMode == IrAlphaMode::Mask);
        }
        if (material.Name.GetView() == "(glTF default)")
        {
            hasDefault = true;
            CHECK(material.BaseColour[0] == 1.0);
            CHECK(material.Metallic == 1.0);
        }
    }
    CHECK(hasMetal);
    CHECK(hasGlow);
    CHECK(hasDefault);
    CHECK(HasCode(log, codes::EXTENSION_IGNORED));
    CHECK(HasCode(log, codes::NORMALMAP_DROPPED) == false); // Tangent-space maps are stored (LUSCN v6).
    CHECK(HasCode(log, codes::SIDEDNESS_IGNORED));
}

TEST_CASE("glTF JSON with external files and the equivalent GLB cook identical bundles", "[scene_cook][gltf]")
{
    BestEffortResult separate;
    BestEffortResult embedded;
    BestEffortResult detected;
    CookError error;
    REQUIRE(ImportGltfBestEffort(GetFixture("gltf/instanced/scene.gltf").string(), {}, separate, error) == true);
    REQUIRE(ImportGltfBestEffort(GetFixture("gltf/embedded/scene.glb").string(), {}, embedded, error) == true);
    REQUIRE(ImportSceneBestEffort(GetFixture("gltf/embedded/scene.glb").string(), {}, detected, error) == true);
    CHECK(detected.Format == SceneSourceFormat::Gltf);
    REQUIRE(separate.Bundle.GetSize() == embedded.Bundle.GetSize());
    CHECK(std::memcmp(separate.Bundle.GetData(), embedded.Bundle.GetData(), separate.Bundle.GetSize()) == 0);
    REQUIRE(embedded.Bundle.GetSize() == detected.Bundle.GetSize());
    CHECK(std::memcmp(embedded.Bundle.GetData(), detected.Bundle.GetData(), embedded.Bundle.GetSize()) == 0);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(separate.Bundle.GetData(), separate.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    CHECK(separate.Summary.SourcePlacements == 5);
    CHECK(separate.Summary.CookedMaterials == decoded.Materials.GetSize());
    CHECK(decoded.Lights.GetSize() == 2);
    bool hasMetal = false;
    for (const ludus::resources::SceneMaterial& material : decoded.Materials)
    {
        // Metal: factors 1 stay the multipliers of the surface map, which carries the normal map
        // (RG), metallicRoughnessTexture green 128 (B, roughness) and blue 255 (A, metallic).
        if (material.Metallic == 1.0F && material.Roughness == 1.0F &&
            material.SurfaceTexture != ludus::content::scene_format::NO_TEXTURE)
        {
            const ludus::resources::SceneTexture& surface = decoded.Textures[material.SurfaceTexture];
            hasMetal = surface.IsLinear == true && surface.Pixels[2] == 128 && surface.Pixels[3] == 255 &&
                       material.NormalScale == 1.0F;
        }
    }
    CHECK(hasMetal);
    bool hasTangent = false;
    for (const ludus::resources::SceneMesh& mesh : decoded.Meshes)
    {
        for (const ludus::foundation::math::Vector4& tangent : mesh.Tangents)
        {
            hasTangent = hasTangent == true || tangent.W != 0.0F;
        }
    }
    CHECK(hasTangent == true);
    const std::string_view report(reinterpret_cast<const char*>(separate.Report.GetData()), separate.Report.GetSize());
    CHECK(report.find("\"importer\": \"gltf-best-effort-v1\"") != std::string_view::npos);
    CHECK(report.find("\"source\": \"scene\"") != std::string_view::npos); // The authored camera.
    CHECK(report.find("TANGENTS_GENERATED") != std::string_view::npos);
    // The data-URI JPEG decodes to its 8x8 texture.
    BestEffortResult jpeg;
    REQUIRE(ImportGltfBestEffort(GetFixture("gltf/data-uri/scene.gltf").string(), {}, jpeg, error) == true);
    const std::string_view jpegReport(reinterpret_cast<const char*>(jpeg.Report.GetData()), jpeg.Report.GetSize());
    CHECK(jpegReport.find("\"texture\": \"scene.gltf#images[0]\"") != std::string_view::npos);
    CHECK(jpegReport.find("\"texture_width\": 8") != std::string_view::npos);
}

TEST_CASE("glTF extension policy: required unsupported fails, optional is reported", "[scene_cook][gltf]")
{
    SceneIr scene;
    DiagnosticLog log;
    CHECK(ReadGltf(MakeTriangle(R"(,"extensionsUsed":["KHR_draco_mesh_compression"],)"
                                R"("extensionsRequired":["KHR_draco_mesh_compression"])"),
                   scene,
                   log) == Status::Invalid);
    CHECK(HasCode(log, codes::EXTENSION_REQUIRED));

    SceneIr optional;
    DiagnosticLog optionalLog;
    CHECK(ReadGltf(MakeTriangle(R"(,"extensionsUsed":["KHR_materials_clearcoat"])"), optional, optionalLog) ==
          Status::Ok);
    CHECK(HasCode(optionalLog, codes::EXTENSION_IGNORED));

    SceneIr quantized;
    DiagnosticLog quantizedLog;
    CHECK(ReadGltf(MakeTriangle(R"(,"extensionsRequired":["KHR_mesh_quantization"])"), quantized, quantizedLog) ==
          Status::Ok);

    SceneIr animated;
    DiagnosticLog animatedLog;
    CHECK(ReadGltf(MakeTriangle(R"(,"animations":[{"channels":[],"samplers":[]}])", {}, R"(,"targets":[{}])"),
                   animated,
                   animatedLog) == Status::Ok);
    CHECK(animatedLog.CountOccurrences(codes::ANIMATION_IGNORED) == 2);

    SceneIr sparse;
    DiagnosticLog sparseLog;
    CHECK(ReadGltf(MakeTriangle({}, R"(,"sparse":{"count":1})"), sparse, sparseLog) == Status::Invalid);
    CHECK(HasCode(sparseLog, codes::PRIMITIVE_SKIPPED));

    SceneIr lines;
    DiagnosticLog linesLog;
    CHECK(ReadGltf(MakeTriangle({}, {}, R"(,"mode":1)"), lines, linesLog) == Status::Invalid);
    CHECK(HasCode(linesLog, codes::PRIMITIVE_SKIPPED));

    SceneIr version;
    DiagnosticLog versionLog;
    CHECK(ReadGltf(R"({"asset":{"version":"1.0"}})", version, versionLog) == Status::Invalid);
    CHECK(HasCode(versionLog, codes::UNSUPPORTED_FEATURE));
}

TEST_CASE("glTF malformed containers, JSON and references fail without partial output", "[scene_cook][gltf]")
{
    const std::string glb = ReadFileText(GetFixture("gltf/embedded/scene.glb"));
    for (const usize length : {usize{3}, usize{11}, usize{19}, usize{100}, glb.size() - 1})
    {
        INFO("GLB truncated to " << length);
        SceneIr scene;
        DiagnosticLog log;
        CHECK(ReadGltf(std::string_view(glb).substr(0, length), scene, log) == Status::Invalid);
        CHECK(log.HasErrors());
    }
    struct Case
    {
        std::string Text;
        std::string_view Code;
    };
    const Case cases[] = {
        {R"({"asset":{"version":"2.0"},)", codes::MALFORMED_INPUT},
        {R"([1,2,3])", codes::MALFORMED_INPUT},
        {R"({"asset":{}})", codes::MALFORMED_INPUT},
        // Accessor count beyond its view (bounded before any allocation).
        {MakeTriangle({}, R"(,"count":4000000000)"), codes::MALFORMED_INPUT},
        // Out-of-range accessor and buffer view offsets.
        {MakeTriangle({}, R"(,"byteOffset":4)"), codes::MALFORMED_INPUT},
        // Node cycle.
        {R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"children":[1]},{"children":[0]}]})",
         codes::MALFORMED_INPUT},
        // Index beyond the vertex count.
        {MakeTriangle(R"(,"extras":0)", {}, R"(,"indices":0)"), codes::MALFORMED_INPUT},
        // Non-finite node transform.
        {R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"scale":[1e308,1e308,1e308],)"
         R"("matrix":[1e308,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"children":[1]},{"scale":[1e308,1,1]}]})",
         codes::MALFORMED_INPUT},
        // Material index out of range (found by the import fuzzer): reported, never indexed.
        {MakeTriangle({}, {}, R"(,"material":5)"), codes::MALFORMED_INPUT},
        // A zero-filled accessor (no bufferView) is bounded like buffer data (review finding).
        {R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
         R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
         R"("accessors":[{"componentType":5126,"count":1000000000,"type":"VEC3"}]})",
         codes::MALFORMED_INPUT},
        // Nothing drawable.
        {R"({"asset":{"version":"2.0"}})", codes::LIMIT_EXCEEDED},
    };
    for (const Case& item : cases)
    {
        INFO(item.Text);
        SceneIr scene;
        DiagnosticLog log;
        CHECK(ReadGltf(item.Text, scene, log) == Status::Invalid);
        CHECK(HasCode(log, item.Code));
    }
}

TEST_CASE("glTF dependencies resolve inside the root and missing ones are reported", "[scene_cook][gltf]")
{
    const std::string escaping = R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                                 R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
                                 R"("buffers":[{"byteLength":36,"uri":"../outside.bin"}],)"
                                 R"("bufferViews":[{"buffer":0,"byteLength":36}],)"
                                 R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}]})";
    SceneIr scene;
    DiagnosticLog log;
    CHECK(ReadGltf(escaping, scene, log) == Status::Invalid);
    CHECK(HasCode(log, codes::DEPENDENCY_UNREADABLE));

    // The same asset with a percent-encoded external buffer that does exist.
    std::string bytes(36, '\0');
    const float32 positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::memcpy(bytes.data(), positions, sizeof(positions));
    std::string external = escaping;
    external.replace(external.find("../outside.bin"), 14, "data%20dir/tri.bin");
    const MemorySourceFile files[] = {{"data dir/tri.bin", AsBytes(bytes)}};
    SceneIr resolved;
    DiagnosticLog resolvedLog;
    REQUIRE(ReadGltf(external, resolved, resolvedLog, files) == Status::Ok);
    REQUIRE(resolved.DataFiles.GetSize() == 1);
    CHECK(resolved.DataFiles[0].GetView() == "data dir/tri.bin");
    CHECK(resolved.Meshes[0].Positions[3] == 1.0F);

    // A shorter-than-declared buffer is malformed.
    const MemorySourceFile shortFiles[] = {{"data dir/tri.bin", AsBytes(std::string_view(bytes).substr(0, 20))}};
    SceneIr truncated;
    DiagnosticLog truncatedLog;
    CHECK(ReadGltf(external, truncated, truncatedLog, shortFiles) == Status::Invalid);
}

TEST_CASE("glTF accessors convert normalized and quantized components", "[scene_cook][gltf]")
{
    // int16 normalized positions: 32767 -> 1, -32768 -> -1 (clamped), 0 -> 0.
    // Bytes: (32767,0,0,pad) (0,-32768,0,pad) (0,0,32767,pad) with stride 8.
    const std::string buffer = "/38AAAAAAAAAAACAAAAAAAAAAAD/fwAA";
    const std::string text =
        std::string(R"({"asset":{"version":"2.0"},"extensionsRequired":["KHR_mesh_quantization"],)") +
        R"("extensionsUsed":["KHR_mesh_quantization"],"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)" +
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)" +
        R"("buffers":[{"byteLength":24,"uri":"data:application/gltf-buffer;base64,)" + buffer + R"("}],)" +
        R"("bufferViews":[{"buffer":0,"byteLength":24,"byteStride":8}],)" +
        R"("accessors":[{"bufferView":0,"componentType":5122,"normalized":true,"count":3,"type":"VEC3"}]})";
    SceneIr scene;
    DiagnosticLog log;
    REQUIRE(ReadGltf(text, scene, log) == Status::Ok);
    const IrMesh& mesh = scene.Meshes[0];
    CHECK(mesh.Positions[0] == 1.0F);
    CHECK(mesh.Positions[4] == -1.0F);
    CHECK(mesh.Positions[8] == 1.0F);
    CHECK(mesh.Indices.GetSize() == 3); // Non-indexed geometry gets an identity list.
}

TEST_CASE("glTF reading honours cancellation", "[scene_cook][gltf]")
{
    const Cancellation cancel{CancelImmediately, nullptr};
    SceneIr scene;
    DiagnosticLog log;
    CHECK(ReadGltf(MakeTriangle(), scene, log, {}, &cancel) == Status::Cancelled);
}

TEST_CASE("local smoke test: licensed glTF/GLB and OBJ collections import or explain why not",
          "[scene_cook][gltf][obj][licensed]")
{
    // LUDUS_GLTF_SCENES_DIR (for example glTF-Sample-Assets/Models) and
    // LUDUS_OBJ_SCENES_DIR (for example the McGuire Computer Graphics Archive) are
    // searched recursively for entry files; nothing is checked into the repository.
    bool hasCollection = false;
    for (const char* variable : {"LUDUS_GLTF_SCENES_DIR", "LUDUS_OBJ_SCENES_DIR"})
    {
        const char* directory = std::getenv(variable);
        if (directory == nullptr || fs::is_directory(directory) == false)
        {
            continue;
        }
        hasCollection = true;
        for (const fs::directory_entry& file : fs::recursive_directory_iterator(directory))
        {
            const std::string extension = file.path().extension().string();
            if (file.is_regular_file() == false || (extension != ".gltf" && extension != ".glb" && extension != ".obj"))
            {
                continue;
            }
            INFO(file.path().string());
            BestEffortResult result;
            CookError error;
            const bool isImported = ImportSceneBestEffort(file.path().string(), {}, result, error);
            // A failure must come with a report that says why (or be an I/O problem).
            CHECK((isImported == true || result.Report.IsEmpty() == false || error.Result == Status::IoError));
            if (isImported == true)
            {
                ludus::resources::SceneDefinition decoded;
                CHECK(ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(),
                                                                result.Bundle.GetSize(),
                                                                decoded) == ludus::resources::Status::Ok);
            }
        }
    }
    if (hasCollection == false)
    {
        SKIP("set LUDUS_GLTF_SCENES_DIR and/or LUDUS_OBJ_SCENES_DIR to licensed scene collections");
    }
}
