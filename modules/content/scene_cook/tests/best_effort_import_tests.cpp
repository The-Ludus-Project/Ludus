// Tests for the best-effort pbrt-v4 importer (pbrt-best-effort-v1). Every
// fixture is generated here, so no licensed scene is needed; the staircase
// smoke test runs only when LUDUS_PBRT_SCENES_DIR names a directory holding
// staircase/scene-v4.pbrt.

#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/runtime/resources/scene.h>

#include "importers/budget_planner.h"
#include "importers/image/image_reader.h"
#include "importers/pbrt/pbrt_parser.h"
#include "importers/pbrt/pbrt_tokenizer.h"
#include "importers/pbrt/ply_reader.h"
#include "importers/pbrt/shape_tessellation.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdlib.h> // mkdtemp (POSIX).
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
namespace fs = std::filesystem;
using namespace ludus::content::scene_cook;
using namespace ludus::content::scene_cook::importers;
using ludus::foundation::core::float32;
using ludus::foundation::core::uint16;

std::span<const uint8> AsBytes(std::string_view text)
{
    return {reinterpret_cast<const uint8*>(text.data()), text.size()};
}

void WriteFile(const fs::path& path, std::string_view data)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string ReadFileText(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// Fresh scratch directory removed on destruction.
struct ScratchDirectory final
{
    fs::path Path;
    ScratchDirectory()
    {
        std::string pattern = (fs::temp_directory_path() / "ludus-best-effort-XXXXXX").string();
        Path = mkdtemp(pattern.data());
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        fs::remove_all(Path, ignored);
    }
};

void AppendFloat(std::string& inoutData, float32 value, bool isBigEndian)
{
    unsigned char bytes[4];
    std::memcpy(bytes, &value, 4);
    if (isBigEndian)
    {
        std::swap(bytes[0], bytes[3]);
        std::swap(bytes[1], bytes[2]);
    }
    inoutData.append(reinterpret_cast<const char*>(bytes), 4);
}

void AppendInt(std::string& inoutData, uint32 value, bool isBigEndian)
{
    for (int i = 0; i < 4; ++i)
    {
        const int shift = isBigEndian ? 8 * (3 - i) : 8 * i;
        inoutData.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

/// A unit quad in the z = 0 plane as a binary PLY (x y z nx ny nz s t, quad face).
std::string MakeBinaryQuad(bool isBigEndian, float32 offset)
{
    std::string data = "ply\nformat ";
    data += isBigEndian ? "binary_big_endian" : "binary_little_endian";
    data += " 1.0\ncomment generated\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
            "property float nx\nproperty float ny\nproperty float nz\nproperty float s\nproperty float t\n"
            "element face 1\nproperty list uchar int vertex_indices\nend_header\n";
    const float32 corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const float32(&corner)[2] : corners)
    {
        for (const float32 value : {corner[0] + offset, corner[1], 0.0F, 0.0F, 0.0F, 1.0F, corner[0], corner[1]})
        {
            AppendFloat(data, value, isBigEndian);
        }
    }
    data.push_back(4);
    for (uint32 index = 0; index < 4; ++index)
    {
        AppendInt(data, index, isBigEndian);
    }
    return data;
}

std::string MakeAsciiTriangle()
{
    return "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
           "property float u\nproperty float v\nelement face 1\nproperty list uchar uint vertex_indices\n"
           "end_header\n0 0 2 0 0\n1 0 2 1 0\n0 1 2 0 1\n3 0 1 2\n";
}

/// 4x2 RLE true-colour TGA, bottom-up: one repeated run and one raw run per row.
std::string MakeRleTga()
{
    std::string data(18, '\0');
    data[2] = 10;
    data[12] = 4;
    data[14] = 2;
    data[16] = 24;
    for (int row = 0; row < 2; ++row)
    {
        data.push_back(static_cast<char>(0x80 | 1)); // Two repeated pixels.
        data += std::string("\x00\x00\xff", 3);      // Red (BGR).
        data.push_back(1);                           // Two raw pixels.
        data += std::string("\x00\xff\x00\xff\x00\x00", 6);
    }
    return data;
}

std::string MakePfm()
{
    std::string data = "PF\n2 1\n-1.0\n";
    for (const float32 value : {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 4.0F})
    {
        AppendFloat(data, value, false);
    }
    return data;
}

/// Writes the synthetic mini-scene: includes (nested, cyclic, escaping), named
/// materials of every mapped type, binary LE/BE and ascii PLY, inline shapes,
/// instancing, area/infinite/point lights and an unknown directive.
fs::path WriteFixture(const fs::path& root, usize extraMaterials = 0)
{
    WriteFile(root / "models/quad_le.ply", MakeBinaryQuad(false, 0.0F));
    WriteFile(root / "models/quad_be.ply", MakeBinaryQuad(true, 2.0F));
    WriteFile(root / "models/triangle.ply", MakeAsciiTriangle());
    WriteFile(root / "textures/checker.tga", MakeRleTga());
    WriteFile(root / "textures/sky.pfm", MakePfm());
    WriteFile(root / "textures/unsupported.exr", "not an image");
    WriteFile(root / "LICENSE.txt", "CC0 synthetic fixture\n");
    WriteFile(root / "include/materials.pbrt",
              "# Materials live in an include.\n"
              "Texture \"checker\" \"spectrum\" \"imagemap\" \"string filename\" \"textures/checker.tga\"\n"
              "Texture \"broken\" \"spectrum\" \"imagemap\" \"string filename\" [ \"textures/unsupported.exr\" ]\n"
              "Texture \"rough\" \"float\" \"constant\" \"float value\" 0.25\n"
              "MakeNamedMaterial \"Floor\" \"string type\" \"coateddiffuse\" \"texture reflectance\" \"checker\"\n"
              "    \"float roughness\" 0.1 \"bool remaproughness\" false\n"
              "MakeNamedMaterial \"Copper\" \"string type\" \"conductor\" \"spectrum eta\" \"metal-Cu-eta\"\n"
              "    \"spectrum k\" \"metal-Cu-k\" \"texture roughness\" \"rough\"\n"
              "MakeNamedMaterial \"Glass\" \"string type\" \"dielectric\" \"float eta\" 1.5\n"
              "MakeNamedMaterial \"Strange\" \"string type\" \"hair\"\n"
              "MakeNamedMaterial \"Broken\" \"string type\" \"diffuse\" \"texture reflectance\" \"broken\"\n"
              "Include \"include/nested.pbrt\"\n");
    WriteFile(root / "include/nested.pbrt",
              "Include \"include/nested.pbrt\"\n" // Cycle.
              "Include \"../outside.pbrt\"\n"     // Escapes the root.
              "MakeNamedMaterial \"Paint\" \"string type\" \"diffuse\" \"rgb reflectance\" [ 0.2 0.4 0.8 ]\n");
    std::string main = "LookAt 0 1 -5  0 0 0  0 1 0\n"
                       "Camera \"perspective\" \"float fov\" [ 45 ]\n"
                       "Film \"rgb\" \"integer xresolution\" [ 400 ] \"integer yresolution\" [ 800 ]\n"
                       "Sampler \"sobol\" \"integer pixelsamples\" 16\n"
                       "WorldBegin\n"
                       "Include \"include/materials.pbrt\"\n";
    for (usize i = 0; i < extraMaterials; ++i)
    {
        main += "MakeNamedMaterial \"Extra" + std::to_string(i) +
                "\" \"string type\" \"diffuse\" \"rgb reflectance\" [ " +
                std::to_string(0.05 * static_cast<double>(i % 20)) + " 0.5 0.5 ]\n";
    }
    main +=
        "LightSource \"infinite\" \"rgb L\" [ 0.1 0.2 0.3 ] \"string filename\" \"textures/sky.pfm\"\n"
        "LightSource \"point\" \"rgb I\" [ 1 1 1 ]\n"
        "MakeNamedMedium \"fog\" \"string type\" \"homogeneous\"\n"
        "FancyNewDirective \"x\" 1 2 3\n"
        "AttributeBegin\n"
        "  NamedMaterial \"Floor\"\n"
        "  Scale 4 4 1\n"
        "  Shape \"plymesh\" \"string filename\" \"models/quad_le.ply\" \"float edgelength\" 1\n"
        "AttributeEnd\n"
        "AttributeBegin\n"
        "  NamedMaterial \"Copper\"\n"
        "  Shape \"plymesh\" \"string filename\" \"models/quad_be.ply\"\n"
        "  Shape \"plymesh\" \"string filename\" \"models/triangle.ply\"\n"
        "AttributeEnd\n"
        "TransformBegin\n"
        "  Translate 0 2 0\n"
        "  NamedMaterial \"Glass\"\n"
        "  Shape \"sphere\" \"float radius\" 0.5\n"
        "TransformEnd\n"
        "AttributeBegin\n"
        "  AreaLightSource \"diffuse\" \"rgb L\" [ 20 20 20 ] \"float scale\" 2\n"
        "  Translate 0 3 0\n"
        "  Shape \"disk\" \"float radius\" 0.25\n"
        "AttributeEnd\n"
        "NamedMaterial \"Paint\"\n"
        "Shape \"trianglemesh\" \"point3 P\" [ 0 0 0 1 0 0 0 0 1 ] \"integer indices\" [ 0 1 2 ]\n"
        "Shape \"bilinearmesh\" \"point3 P\" [ 0 0 0 1 0 0 0 1 0 1 1 0 ] \"point2 uv\" [ 0 0 1 0 0 1 1 1 ]\n"
        "NamedMaterial \"Strange\"\n"
        "Shape \"loopsubdiv\" \"point3 P\" [ 0 0 0 1 0 0 0 1 0 ] \"integer indices\" [ 0 1 2 ] \"integer levels\" 3\n"
        "Shape \"curve\" \"point3 P\" [ 0 0 0 1 1 1 2 2 2 3 3 3 ]\n"
        "NamedMaterial \"Broken\"\n"
        "Shape \"trianglemesh\" \"point3 P\" [ 5 0 0 60 0 0 5 60 0 ]\n"
        "ObjectBegin \"pair\"\n"
        "  NamedMaterial \"Paint\"\n"
        "  Shape \"trianglemesh\" \"point3 P\" [ 0 0 0 0.1 0 0 0 0.1 0 ]\n"
        "ObjectEnd\n"
        "AttributeBegin\n  Translate 10 0 0\n  ObjectInstance \"pair\"\nAttributeEnd\n"
        "AttributeBegin\n  Translate 11 0 0\n  ObjectInstance \"pair\"\nAttributeEnd\n"
        "ObjectInstance \"missing\"\n";
    for (usize i = 0; i < extraMaterials; ++i)
    {
        main += "NamedMaterial \"Extra" + std::to_string(i) + "\"\nShape \"trianglemesh\" \"point3 P\" [ " +
                std::to_string(i) + " 5 0 " + std::to_string(i) + ".5 5 0 " + std::to_string(i) + " 5.5 0 ]\n";
    }
    WriteFile(root / "scene.pbrt", main);
    return root / "scene.pbrt";
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

bool DecodesAtRuntime(const BestEffortResult& result, ludus::resources::SceneDefinition& outScene)
{
    return ludus::resources::SceneDefinition::Decode(result.Bundle.GetData(), result.Bundle.GetSize(), outScene) ==
           ludus::resources::Status::Ok;
}
} // namespace

TEST_CASE("pbrt tokenizer is locale-free and tracks lines", "[scene_cook][pbrt]")
{
    const std::string_view text = "# comment\nShape \"a\\\"b\" [ .5 -1. +2 1e-60 ]\n  true\n";
    MemoryByteSource source(AsBytes(text));
    pbrt::Tokenizer tokens(source);
    REQUIRE(tokens.Advance() == pbrt::TokenizerStatus::Ok);
    CHECK(tokens.GetCurrent().Kind == pbrt::TokenKind::Word);
    CHECK(tokens.GetCurrent().Line == 2);
    REQUIRE(tokens.Advance() == pbrt::TokenizerStatus::Ok);
    CHECK(tokens.GetCurrent().Kind == pbrt::TokenKind::String);
    CHECK(tokens.GetCurrent().Text.GetView() == "a\"b");
    REQUIRE(tokens.Advance() == pbrt::TokenizerStatus::Ok);
    CHECK(tokens.GetCurrent().Kind == pbrt::TokenKind::OpenBracket);
    const float64 expected[] = {.5, -1.0, 2.0, 0.0};
    for (const float64 value : expected)
    {
        REQUIRE(tokens.Advance() == pbrt::TokenizerStatus::Ok);
        float64 parsed = 99.0;
        REQUIRE(ParseNumber(tokens.GetCurrent().Text.GetView(), parsed) == true);
        CHECK(parsed == value);
    }
    REQUIRE(tokens.Advance() == pbrt::TokenizerStatus::Ok);
    CHECK(tokens.GetCurrent().Kind == pbrt::TokenKind::CloseBracket);
    REQUIRE(tokens.Advance() == pbrt::TokenizerStatus::Ok);
    CHECK(tokens.GetCurrent().Line == 3);
    REQUIRE(tokens.Advance() == pbrt::TokenizerStatus::Ok);
    CHECK(tokens.GetCurrent().Kind == pbrt::TokenKind::End);

    float64 ignored = 0.0;
    CHECK(ParseNumber("1e999", ignored) == false);
    CHECK(ParseNumber("1.2.3", ignored) == false);
    CHECK(ParseNumber("-", ignored) == false);

    MemoryByteSource unterminated(AsBytes("Shape \"open"));
    pbrt::Tokenizer broken(unterminated);
    REQUIRE(broken.Advance() == pbrt::TokenizerStatus::Ok);
    CHECK(broken.Advance() == pbrt::TokenizerStatus::SyntaxError);
}

TEST_CASE("PLY reader handles ascii and both binary byte orders", "[scene_cook][pbrt]")
{
    for (const bool isBigEndian : {false, true})
    {
        const std::string data = MakeBinaryQuad(isBigEndian, 0.0F);
        IrMesh mesh;
        PlyHeaderSummary summary;
        std::string_view message;
        REQUIRE(ReadPly(AsBytes(data), mesh, summary, message) == true);
        CHECK(mesh.GetVertexCount() == 4);
        CHECK(mesh.Indices.GetSize() == 6); // Fan-triangulated quad.
        CHECK(summary.HasNormals == true);
        CHECK(summary.HasUvs == true);
        CHECK(mesh.Positions[3] == 1.0F);
        CHECK(mesh.Uvs[5] == 0.0F); // v = 1 flipped to the top-down convention.
    }
    IrMesh mesh;
    PlyHeaderSummary summary;
    std::string_view message;
    REQUIRE(ReadPly(AsBytes(MakeAsciiTriangle()), mesh, summary, message) == true);
    CHECK(mesh.Positions[2] == 2.0F);
    CHECK(mesh.Normals.IsEmpty() == true);

    std::string truncated = MakeBinaryQuad(false, 0.0F);
    truncated.resize(truncated.size() - 3);
    CHECK(ReadPly(AsBytes(truncated), mesh, summary, message) == false);
    std::string badIndex = MakeAsciiTriangle();
    badIndex.replace(badIndex.size() - 2, 1, "7");
    CHECK(ReadPly(AsBytes(badIndex), mesh, summary, message) == false);
}

TEST_CASE("image decoders cover TGA RLE and PFM, with unknown containers unsupported", "[scene_cook][pbrt]")
{
    DecodedImage image;
    std::string_view message;
    REQUIRE(DecodeImage("a.tga", AsBytes(MakeRleTga()), image, message) == ImageResult::Ok);
    CHECK(image.Width == 4);
    CHECK(image.Height == 2);
    CHECK(image.Texels[0] == 255); // Red first, opaque.
    CHECK(image.Texels[3] == 255);
    CHECK(image.Texels[8 + 1] == 255); // Third pixel green.
    REQUIRE(DecodeImage("sky.PFM", AsBytes(MakePfm()), image, message) == ImageResult::Ok);
    CHECK(image.Width == 2);
    CHECK(image.Texels[0] == 255);
    CHECK(image.Texels[4 + 2] == 255); // Clamped HDR blue.
    // A truncated OpenEXR header is malformed; containers without a decoder are unsupported.
    CHECK(DecodeImage("sky.exr", AsBytes("v/1\x01"), image, message) == ImageResult::Invalid);
    CHECK(DecodeImage("sky.tif", AsBytes("II*\x00\x08\x00\x00\x00"), image, message) == ImageResult::Unsupported);
    CHECK(DecodeImage("x.png", AsBytes("\x89PNG\r\n\x1a\nbroken"), image, message) == ImageResult::Invalid);
}

TEST_CASE("pbrt reader builds the IR with fallbacks and file:line diagnostics", "[scene_cook][pbrt]")
{
    ScratchDirectory scratch;
    const fs::path entry = WriteFixture(scratch.Path);
    SceneIr scene;
    DiagnosticLog log;
    ludus::foundation::String root;
    REQUIRE(pbrt::ReadScene(entry.string(), scene, log, root) == ludus::content::Status::Ok);
    CHECK(scene.SourceFiles.GetSize() == 3); // Entry, materials, nested (once).
    CHECK(scene.Materials.GetSize() == 6);
    CHECK(scene.Objects.GetSize() == 1);
    CHECK(scene.Instances.GetSize() == 2);
    CHECK(scene.Lights.Area == 1);
    CHECK(scene.Lights.Infinite == 1);
    CHECK(scene.Lights.Point == 1);
    CHECK(scene.Environment.IsPresent == true);
    CHECK(scene.Environment.File.GetView() == "textures/sky.pfm");
    CHECK(scene.Camera.IsPresent == true);
    // Portrait film: the 45 degree fov spans the horizontal axis.
    CHECK(scene.Camera.VerticalFov > 1.38); // 2 atan(2 tan(22.5 degrees)).
    // The PLY loaded twice in source is cached once; one mesh per inline shape.
    CHECK(scene.Meshes.GetSize() == 10);
    for (const std::string_view code : {codes::INCLUDE_FAILED,
                                        codes::UNKNOWN_DIRECTIVE,
                                        codes::MEDIUM_SKIPPED,
                                        codes::ENVIRONMENT_FALLBACK,
                                        codes::TRANSMISSION_DROPPED,
                                        codes::MATERIAL_DEFAULTED,
                                        codes::SHAPE_SKIPPED,
                                        codes::SUBDIV_SKIPPED,
                                        codes::NAME_UNDEFINED,
                                        codes::DEPRECATED_DIRECTIVE,
                                        codes::SPECTRUM_APPROXIMATED})
    {
        INFO(code);
        CHECK(HasCode(log, code) == true);
    }
    CHECK(log.CountOccurrences(codes::INCLUDE_FAILED) == 2); // Cycle and root escape.
    bool hasLocatedUnknown = false;
    for (const Diagnostic& diagnostic : log.GetEntries())
    {
        if (diagnostic.Code.GetView() == codes::UNKNOWN_DIRECTIVE)
        {
            hasLocatedUnknown = diagnostic.File.GetView() == "scene.pbrt" && diagnostic.Line == 10;
        }
        if (diagnostic.Code.GetView() == codes::INCLUDE_FAILED)
        {
            CHECK(diagnostic.File.GetView() == "include/nested.pbrt");
        }
    }
    CHECK(hasLocatedUnknown == true);
    // coateddiffuse roughness 0.1 without remapping: sqrt(alpha) = sqrt(0.1).
    CHECK_THAT(scene.Materials[0].Roughness, Catch::Matchers::WithinAbs(0.316227766, 1e-6));
    CHECK(scene.Materials[1].Metallic == 1.0);
    CHECK_THAT(scene.Materials[1].BaseColour[0], Catch::Matchers::WithinAbs(.955, 1e-9));
}

TEST_CASE("syntax errors fail with a located error; include errors stay local", "[scene_cook][pbrt]")
{
    SceneIr scene;
    DiagnosticLog log;
    const std::string_view text = "WorldBegin\nShape \"trianglemesh\" \"point3 P\" [ 0 0 0\n  \"unterminated\n";
    CHECK(pbrt::ReadSceneFromMemory({AsBytes(text), {}, "memory.pbrt"}, scene, log) == ludus::content::Status::Invalid);
    REQUIRE(log.HasErrors() == true);
    bool hasLocatedError = false;
    for (const Diagnostic& diagnostic : log.GetEntries())
    {
        hasLocatedError =
            hasLocatedError == true || (diagnostic.Severity == DiagnosticSeverity::Error &&
                                        diagnostic.File.GetView() == "memory.pbrt" && diagnostic.Line == 3);
    }
    CHECK(hasLocatedError == true);

    DiagnosticLog fileLog;
    CHECK(pbrt::ReadSceneFromMemory({AsBytes("Include \"x.pbrt\"\nWorldBegin\n"), {}, "memory.pbrt"}, scene, fileLog) ==
          ludus::content::Status::Ok);
    CHECK(fileLog.CountOccurrences(codes::INCLUDE_FAILED) == 1); // No root: file access is refused.
}

TEST_CASE("best-effort import cooks a decodable, deterministic bundle and report", "[scene_cook][pbrt]")
{
    ScratchDirectory scratch;
    const fs::path entry = WriteFixture(scratch.Path);
    BestEffortResult first;
    BestEffortResult second;
    CookError error;
    REQUIRE(ImportPbrtBestEffort(entry.string(), {}, first, error) == true);
    REQUIRE(ImportPbrtBestEffort(entry.string(), {}, second, error) == true);
    CHECK(first.Bundle.GetSize() == second.Bundle.GetSize());
    CHECK(std::memcmp(first.Bundle.GetData(), second.Bundle.GetData(), first.Bundle.GetSize()) == 0);
    CHECK(std::string_view(reinterpret_cast<const char*>(first.Report.GetData()), first.Report.GetSize()) ==
          std::string_view(reinterpret_cast<const char*>(second.Report.GetData()), second.Report.GetSize()));
    ludus::resources::SceneDefinition decoded;
    REQUIRE(DecodesAtRuntime(first, decoded) == true);
    const ImportSummary& summary = first.Summary;
    CHECK(summary.Errors == 0);
    CHECK(summary.CookedMaterials == decoded.Materials.GetSize());
    CHECK(summary.CookedMaterials <= ludus::content::scene_format::IMPORT_LIMITS.Meshes);
    CHECK(summary.SourcePlacements == 11);
    CHECK(summary.DroppedPlacements == 0);
    CHECK(first.License.GetSize() != 0);
    // Exactly one emissive slot, with radiance L x scale.
    usize emissiveSlots = 0;
    for (const ludus::resources::SceneMaterial& material : decoded.Materials)
    {
        if (material.Emission.X > 0.0F)
        {
            ++emissiveSlots;
            CHECK(material.Emission.X == 2.0F); // Frozen RGB/Y photometric proxy; scale is retained.
        }
    }
    CHECK(emissiveSlots == 1);
    CHECK(ReportHasCode(first, codes::TEXTURE_UNREADABLE) == true);
    const std::string_view report(reinterpret_cast<const char*>(first.Report.GetData()), first.Report.GetSize());
    CHECK(report.find("\"importer\": \"pbrt-best-effort-v1\"") != std::string_view::npos);
    CHECK(report.find(scratch.Path.string()) == std::string_view::npos); // Root-relative paths only.

    // The written outputs round-trip.
    REQUIRE(WriteBestEffortImport((scratch.Path / "out").string(), first, error) == true);
    CHECK(fs::file_size(scratch.Path / "out/scene.luscn") == first.Bundle.GetSize());
    CHECK(ReadFileText(scratch.Path / "out/LICENSE.txt") == "CC0 synthetic fixture\n");
}

TEST_CASE("dry run plans without decoding images or packing a bundle", "[scene_cook][pbrt]")
{
    ScratchDirectory scratch;
    const fs::path entry = WriteFixture(scratch.Path);
    BestEffortOptions options;
    options.IsDryRun = true;
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportPbrtBestEffort(entry.string(), options, result, error) == true);
    CHECK(result.Bundle.IsEmpty() == true);
    CHECK(result.Report.IsEmpty() == false);
    CHECK(result.Summary.CookedMaterials != 0);
    CHECK(ReportHasCode(result, codes::TEXTURE_UNREADABLE) == false);
    CHECK(WriteBestEffortImport((scratch.Path / "out").string(), result, error) == false);
}

TEST_CASE("budget planner keeps one slot per material within the import profile", "[scene_cook][pbrt]")
{
    ScratchDirectory scratch;
    const fs::path entry = WriteFixture(scratch.Path, 20);
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportPbrtBestEffort(entry.string(), {}, result, error) == true);
    CHECK(result.Summary.SourceMaterials == 26);
    // Every source material keeps its slot, plus one slot for the emitter group.
    CHECK(result.Summary.CookedMaterials == result.Summary.SourceMaterials + 1);
    CHECK(ReportHasCode(result, codes::MATERIALS_MERGED) == false);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(DecodesAtRuntime(result, decoded) == true);
    CHECK(decoded.Materials.GetLast().Emission.X > 0.0F); // Emissive slot kept last.
}

TEST_CASE("budget planner drops the smallest placements beyond the vertex budget", "[scene_cook][pbrt]")
{
    SceneIr scene;
    // Three placements exceed the vertex budget, two fit (a multiple of three for whole triangles).
    constexpr usize VERTICES = usize{ludus::content::scene_format::IMPORT_LIMITS.Vertices} / 9 * 3 + 3;
    static_assert(VERTICES % 3 == 0);
    for (usize m = 0; m < 3; ++m)
    {
        IrMesh mesh;
        // Model a compacted placement whose original records contained duplicates.
        mesh.SourceVertexCount = static_cast<uint32>(VERTICES * 2);
        REQUIRE(mesh.Positions.TryResize(VERTICES * 3) == true);
        for (usize v = 0; v < VERTICES; ++v)
        {
            mesh.Positions[v * 3] = static_cast<float32>(v % 3 == 1 ? m + 1 : 0);
            mesh.Positions[v * 3 + 1] = static_cast<float32>(v % 3 == 2 ? m + 1 : 0);
            mesh.Positions[v * 3 + 2] = 0.0F;
        }
        for (uint32 i = 0; i + 2 < VERTICES; i += 3)
        {
            REQUIRE(mesh.Indices.TryAdd(i) == true);
            REQUIRE(mesh.Indices.TryAdd(i + 1) == true);
            REQUIRE(mesh.Indices.TryAdd(i + 2) == true);
        }
        REQUIRE(scene.Meshes.TryAdd(static_cast<IrMesh&&>(mesh)) == true);
        IrShape shape;
        shape.Mesh = static_cast<uint32>(m);
        REQUIRE(scene.Shapes.TryAdd(shape) == true);
        REQUIRE(scene.WorldShapes.TryAdd(static_cast<uint32>(m)) == true);
    }
    DiagnosticLog log;
    CookPlan plan;
    REQUIRE(PlanBudget(scene, log, plan, nullptr) == ludus::content::Status::Ok);
    CHECK(plan.DroppedPlacements == 1);
    CHECK(plan.SourceVertices == VERTICES * 2 * 3);
    CHECK(plan.DroppedVertices == VERTICES * 2);
    CHECK(plan.Placements[0].IsKept == false); // Smallest area (scale 1) goes first.
    CHECK(plan.Placements[2].IsKept == true);
    CHECK(log.CountOccurrences(codes::GEOMETRY_DROPPED) == 1);
}

TEST_CASE("budget planner counts shared vertices once and bounds the combined index count", "[scene_cook][pbrt]")
{
    // A mesh of VertexCount positions whose IndexCount / 3 triangles reuse them.
    struct SharedMesh final
    {
        usize VertexCount = 0;
        usize IndexCount = 0;
        float32 Scale = 1.0F;
    };
    const auto addMesh = [](SceneIr& inoutScene, const SharedMesh& shared) {
        const usize vertexCount = shared.VertexCount;
        const usize indexCount = shared.IndexCount;
        const float32 scale = shared.Scale;
        IrMesh mesh;
        REQUIRE(mesh.Positions.TryResize(vertexCount * 3) == true);
        for (usize v = 0; v < vertexCount; ++v)
        {
            mesh.Positions[v * 3] = scale * static_cast<float32>(v % 3 == 1 ? 1 : 0);
            mesh.Positions[v * 3 + 1] = scale * static_cast<float32>(v % 3 == 2 ? 1 : 0);
            mesh.Positions[v * 3 + 2] = static_cast<float32>(v) * 1e-6F;
        }
        REQUIRE(mesh.Indices.TryResize(indexCount) == true);
        for (usize i = 0; i < indexCount; ++i)
        {
            mesh.Indices[i] = static_cast<uint32>((i / 3 * 3 + i % 3) % vertexCount);
        }
        const uint32 index = static_cast<uint32>(inoutScene.Meshes.GetSize());
        REQUIRE(inoutScene.Meshes.TryAdd(static_cast<IrMesh&&>(mesh)) == true);
        IrShape shape;
        shape.Mesh = index;
        REQUIRE(inoutScene.Shapes.TryAdd(shape) == true);
        REQUIRE(inoutScene.WorldShapes.TryAdd(index) == true);
    };
    SECTION("more triangle corners than the vertex budget fit when vertices are shared")
    {
        SceneIr scene;
        constexpr usize VERTICES = 100002;
        constexpr usize INDICES = (usize{ludus::content::scene_format::IMPORT_LIMITS.Vertices} / 3 + 1) * 3;
        static_assert(INDICES > ludus::content::scene_format::IMPORT_LIMITS.Vertices);
        addMesh(scene, { .VertexCount = VERTICES, .IndexCount = INDICES });
        DiagnosticLog log;
        CookPlan plan;
        REQUIRE(PlanBudget(scene, log, plan, nullptr) == ludus::content::Status::Ok);
        CHECK(plan.DroppedPlacements == 0);
        CHECK(plan.Placements[0].IsKept == true);
    }
    SECTION("the smallest placement leaves when the combined index count would overflow")
    {
        SceneIr scene;
        constexpr usize INDICES = usize{ludus::content::scene_format::IMPORT_LIMITS.TotalIndices} / 9 * 3 + 3;
        static_assert(3 * INDICES > ludus::content::scene_format::IMPORT_LIMITS.TotalIndices);
        static_assert(2 * INDICES <= ludus::content::scene_format::IMPORT_LIMITS.TotalIndices);
        for (usize m = 0; m < 3; ++m)
        {
            addMesh(scene, { .VertexCount = 3, .IndexCount = INDICES, .Scale = static_cast<float32>(m + 1) });
        }
        DiagnosticLog log;
        CookPlan plan;
        REQUIRE(PlanBudget(scene, log, plan, nullptr) == ludus::content::Status::Ok);
        CHECK(plan.DroppedPlacements == 1);
        CHECK(plan.Placements[0].IsKept == false); // Smallest area goes first.
        CHECK(plan.Placements[2].IsKept == true);
        CHECK(log.CountOccurrences(codes::GEOMETRY_DROPPED) == 1);
    }
}

TEST_CASE("pbrt-reference-v1 is unaffected: best-effort and reference ids differ", "[scene_cook][pbrt]")
{
    CHECK(BEST_EFFORT_PROFILE != PROFILE);
}

TEST_CASE("local smoke test: licensed staircase imports with readable limits", "[scene_cook][pbrt][licensed]")
{
    const char* directory = std::getenv("LUDUS_PBRT_SCENES_DIR");
    if (directory == nullptr || fs::exists(fs::path(directory) / "staircase/scene-v4.pbrt") == false)
    {
        SKIP("set LUDUS_PBRT_SCENES_DIR to a directory containing staircase/scene-v4.pbrt");
    }
    const fs::path entry = fs::path(directory) / "staircase/scene-v4.pbrt";
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportPbrtBestEffort(entry.string(), {}, result, error) == true);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(DecodesAtRuntime(result, decoded) == true);
    CHECK(result.Summary.CookedMaterials <= ludus::content::scene_format::MAX_MATERIALS);
    CHECK(result.Summary.SourcePlacements >= 775);
    CHECK(result.Summary.DroppedPlacements == 0);
    CHECK(result.Summary.Errors == 0);
    if (const char* output = std::getenv("LUDUS_PBRT_SMOKE_OUTPUT"); output != nullptr)
    {
        REQUIRE(WriteBestEffortImport(output, result, error) == true);
    }
}

TEST_CASE("PBRT compaction retains every triangle through runtime decoding", "[scene_cook][pbrt][compaction]")
{
    ScratchDirectory scratch;
    const fs::path entry = scratch.Path / "scene.pbrt";
    WriteFile(entry,
              "WorldBegin\nMaterial \"diffuse\"\n"
              "Shape \"trianglemesh\" \"point3 P\" [0 0 0 1 0 0 1 1 0 0 0 0 1 1 0 0 1 0]\n"
              "\"normal3 N\" [0 0 1 0 0 1 0 0 1 0 0 1 0 0 1 0 0 1]\n"
              "\"integer indices\" [0 1 2 3 4 5]\n");
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportPbrtBestEffort(entry.string(), {}, result, error) == true);
    CHECK(result.Summary.SourceVertices == 6);
    CHECK(result.Summary.CookedVertices == 4);
    CHECK(result.Summary.SourceIndices == 6);
    CHECK(result.Summary.CookedIndices == result.Summary.SourceIndices);
    CHECK(result.Summary.DroppedPlacements == 0);
    CHECK(ReportHasCode(result, codes::VERTICES_COMPACTED) == true);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(DecodesAtRuntime(result, decoded) == true);
    REQUIRE(decoded.Meshes.GetSize() == 1);
    CHECK(decoded.Meshes[0].Positions.GetSize() == 4);
    CHECK(decoded.Meshes[0].Indices.GetSize() == 6);
}

TEST_CASE("local Bistro import retains full geometry and decodes at runtime", "[scene_cook][pbrt][licensed][bistro]")
{
    const char* directoryOrNull = std::getenv("LUDUS_PBRT_SCENES_DIR");
    if (directoryOrNull == nullptr || fs::exists(fs::path(directoryOrNull) / "bistro/bistro_cafe.pbrt") == false)
    {
        SKIP("set LUDUS_PBRT_SCENES_DIR to a directory containing bistro/bistro_cafe.pbrt");
    }
    const fs::path entry = fs::path(directoryOrNull) / "bistro/bistro_cafe.pbrt";
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportPbrtBestEffort(entry.string(), {}, result, error) == true);
    CHECK(result.Summary.Errors == 0);
    CHECK(result.Summary.SourcePlacements == 1606);
    CHECK(result.Summary.SourceVertices == 8514702);
    CHECK(result.Summary.SourceIndices == 8516880);
    CHECK(result.Summary.CookedIndices == result.Summary.SourceIndices);
    CHECK(result.Summary.DroppedPlacements == 0);
    CHECK(ReportHasCode(result, codes::GEOMETRY_DROPPED) == false);
    CHECK(ReportHasCode(result, codes::SHAPE_FAILED) == false);
    CHECK(ReportHasCode(result, codes::SHAPE_SKIPPED) == false);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(DecodesAtRuntime(result, decoded) == true);
    usize vertices = 0;
    usize indices = 0;
    for (const ludus::resources::SceneMesh& mesh : decoded.Meshes)
    {
        vertices += mesh.Positions.GetSize();
        indices += mesh.Indices.GetSize();
    }
    CHECK(vertices == result.Summary.CookedVertices);
    CHECK(indices == result.Summary.SourceIndices);
    CHECK(result.Bundle.GetSize() <= ludus::content::scene_format::IMPORT_LIMITS.BundleBytes);
}

TEST_CASE("sphere and disk tessellation honour zmin, zmax and phimax", "[scene_cook][pbrt]")
{
    ShapeArguments arguments;
    arguments.Radius = 2.0;
    arguments.ZMax = 0.0;
    arguments.PhiMaxDegrees = 90.0;
    IrMesh mesh;
    std::string_view message;
    REQUIRE(TessellateShape("sphere", arguments, mesh, message) == ShapeResult::Ok);
    for (usize v = 0; v < mesh.GetVertexCount(); ++v)
    {
        CHECK(mesh.Positions[v * 3] >= -1e-6F);     // x >= 0 within the quarter sweep.
        CHECK(mesh.Positions[v * 3 + 1] >= -1e-6F); // y >= 0.
        CHECK(mesh.Positions[v * 3 + 2] <= 1e-6F);  // Lower hemisphere only.
    }
    ShapeArguments disk;
    disk.PhiMaxDegrees = 180.0;
    REQUIRE(TessellateShape("disk", disk, mesh, message) == ShapeResult::Ok);
    for (usize v = 0; v < mesh.GetVertexCount(); ++v)
    {
        CHECK(mesh.Positions[v * 3 + 1] >= -1e-6F);
    }
    ShapeArguments empty;
    empty.ZMin = 1.0;
    empty.ZMax = 0.5;
    CHECK(TessellateShape("sphere", empty, mesh, message) == ShapeResult::Invalid);
}

TEST_CASE("mirrored instances flip winding through the full placement transform", "[scene_cook][pbrt]")
{
    ScratchDirectory scratch;
    const auto cook = [&](std::string_view instanceTransform) {
        std::string text = "LookAt 0 0 5  0 0 0  0 1 0\nCamera \"perspective\"\nWorldBegin\n"
                           "ObjectBegin \"tri\"\nShape \"trianglemesh\" \"point3 P\" [0 0 0 1 0 0 0 1 0] "
                           "\"integer indices\" [0 1 2]\nObjectEnd\nAttributeBegin\n";
        text += instanceTransform;
        text += "\nObjectInstance \"tri\"\nAttributeEnd\n";
        WriteFile(scratch.Path / "mirror.pbrt", text);
        BestEffortResult result;
        CookError error;
        REQUIRE(ImportPbrtBestEffort((scratch.Path / "mirror.pbrt").string(), {}, result, error) == true);
        ludus::resources::SceneDefinition decoded;
        REQUIRE(DecodesAtRuntime(result, decoded) == true);
        return decoded;
    };
    const ludus::resources::SceneDefinition plain = cook("Translate 0 0 0");
    const ludus::resources::SceneDefinition mirrored = cook("Scale -1 1 1");
    REQUIRE(plain.Meshes.GetSize() == 1);
    REQUIRE(mirrored.Meshes.GetSize() == 1);
    // Deindexing gives every triangle its own chart. Check geometric facing,
    // rather than old source-index equality, after the mirrored corner swap.
    const ludus::resources::SceneMesh& mesh = mirrored.Meshes[0];
    const ludus::foundation::math::Vector3 normal =
        ludus::foundation::math::Cross(mesh.Positions[mesh.Indices[1]] - mesh.Positions[mesh.Indices[0]],
                                       mesh.Positions[mesh.Indices[2]] - mesh.Positions[mesh.Indices[0]]);
    CHECK(normal.Z > 0);
    CHECK(mesh.Positions[mesh.Indices[1]] == plain.Meshes[0].Positions[plain.Meshes[0].Indices[2]]);
    CHECK(mesh.Positions[mesh.Indices[2]].X == -plain.Meshes[0].Positions[plain.Meshes[0].Indices[1]].X);
}

TEST_CASE("PLY counts are bounded by the body size before allocating", "[scene_cook][pbrt]")
{
    const std::string text = "ply\nformat ascii 1.0\nelement vertex 2000000000\nproperty float x\n"
                             "property float y\nproperty float z\nend_header\n0 0 0\n";
    IrMesh mesh;
    PlyHeaderSummary summary;
    std::string_view message;
    CHECK(ReadPly(AsBytes(text), mesh, summary, message) == false);
    CHECK(message == "PLY body is truncated");
}

TEST_CASE("TGA colour maps with unsupported entry depths are rejected", "[scene_cook][pbrt]")
{
    // Colour-mapped, 8-bit indices, 8-bit palette entries (unsupported), 1x1.
    const uint8 data[] = {0, 1, 1, 0, 0, 1, 0, 8, 0, 0, 0, 0, 1, 0, 1, 0, 8, 0, 7, 0};
    DecodedImage image;
    std::string_view message;
    CHECK(DecodeImage("bad.tga", std::span<const uint8>(data, sizeof(data)), image, message) != ImageResult::Ok);
}

TEST_CASE("Authored analytic positions, cones and emitter sidedness survive PBRT parsing", "[scene-cook][lighting]")
{
    const std::string_view text =
        "WorldBegin\n"
        "AttributeBegin\nTranslate 2 3 4\n"
        "LightSource \"point\" \"rgb I\" [ 2 4 6 ] \"float scale\" 2 \"point3 from\" [ 1 0 0 ]\n"
        "LightSource \"spot\" \"point3 from\" [ 0 0 0 ] \"point3 to\" [ 0 1 0 ] \"float power\" 100 \"float "
        "coneangle\" 40 \"float conedeltaangle\" 10\n"
        "LightSource \"distant\" \"point3 from\" [ 0 0 1 ] \"point3 to\" [ 0 0 0 ] \"float illuminance\" 50\n"
        "AreaLightSource \"diffuse\" \"rgb L\" [ 1 2 3 ] \"bool twosided\" true \"float power\" 200\n"
        "Shape \"disk\"\nAttributeEnd\n"
        "AttributeBegin\nAreaLightSource \"diffuse\"\nShape \"disk\"\nAttributeEnd\n";
    SceneIr scene;
    DiagnosticLog log;
    REQUIRE(pbrt::ReadSceneFromMemory({AsBytes(text), {}, "lights.pbrt"}, scene, log) == ludus::content::Status::Ok);
    REQUIRE(scene.AnalyticLights.GetSize() == 3);
    const IrAnalyticLight& point = scene.AnalyticLights[0];
    CHECK(point.Kind == IrLightKind::Point);
    CHECK(point.PositionWorld.X == 3.0);
    CHECK(point.PositionWorld.Y == 3.0);
    CHECK(point.PositionWorld.Z == 4.0);
    CHECK(point.Strength.X == 4.0);
    CHECK(point.Strength.Y == 8.0);
    CHECK(point.Strength.Z == 12.0);
    const IrAnalyticLight& spot = scene.AnalyticLights[1];
    CHECK(spot.DirectionWorld.Y == 1.0);
    CHECK(spot.InnerAngleDegrees == 30.0);
    CHECK(spot.OuterAngleDegrees == 40.0);
    CHECK(spot.HasPhotometricNormalization == true);
    CHECK(spot.PhotometricNormalization == 100.0);
    const IrAnalyticLight& distant = scene.AnalyticLights[2];
    CHECK(distant.Kind == IrLightKind::Directional);
    CHECK(distant.DirectionWorld.Z == -1.0);
    CHECK(distant.PhotometricNormalization == 50.0);
    REQUIRE(scene.Shapes.GetSize() == 2);
    CHECK(scene.Shapes[0].AreaLight == 0);
    CHECK(scene.Shapes[0].IsEmissionTwoSided == true);
    CHECK(scene.Shapes[0].HasEmissionPower == true);
    CHECK(scene.Shapes[0].EmissionPowerLumens == 200.0);
    CHECK(scene.Shapes[1].AreaLight == 1);
    CHECK(scene.Shapes[1].IsEmissionTwoSided == false);
    CHECK(scene.Shapes[1].HasEmissionPower == false);
}

TEST_CASE("Emitter groups retain unequal power-normalized radiance and source sidedness", "[scene-cook][lighting]")
{
    const std::string_view text =
        "WorldBegin\nAreaLightSource \"diffuse\" \"rgb L\" [ 1 1 1 ] \"float power\" 100 \"float scale\" 2\n"
        "Shape \"trianglemesh\" \"point3 P\" [ 0 0 0  2 0 0  0 1 0 ] \"integer indices\" [ 0 1 2 ]\n"
        "Shape \"trianglemesh\" \"point3 P\" [ 0 0 2  1 0 2  0 1 2 ] \"integer indices\" [ 0 1 2 ]\n"
        "AreaLightSource \"diffuse\" \"rgb L\" [ 1 1 1 ] \"float power\" 100 \"float scale\" 2 \"bool twosided\" true\n"
        "Shape \"trianglemesh\" \"point3 P\" [ 0 0 4  2 0 4  0 1 4 ] \"integer indices\" [ 0 1 2 ]\n";
    SceneIr scene;
    DiagnosticLog log;
    REQUIRE(pbrt::ReadSceneFromMemory({AsBytes(text), {}, "emitters.pbrt"}, scene, log) == ludus::content::Status::Ok);
    CookPlan plan;
    REQUIRE(PlanBudget(scene, log, plan, nullptr) == ludus::content::Status::Ok);
    REQUIRE(plan.Materials.GetSize() == 3);
    CHECK(plan.Materials[0].IsEmissionTwoSided == false);
    CHECK(plan.Materials[1].IsEmissionTwoSided == false);
    CHECK(plan.Materials[2].IsEmissionTwoSided == true);
    CHECK(ludus::foundation::math::Abs(plan.Materials[0].Emission[0] - 200.0 / ludus::foundation::math::kPi) < 1e-5);
    CHECK(ludus::foundation::math::Abs(plan.Materials[1].Emission[0] - 400.0 / ludus::foundation::math::kPi) < 1e-5);
    CHECK(ludus::foundation::math::Abs(plan.Materials[2].Emission[0] - 100.0 / ludus::foundation::math::kPi) < 1e-5);
    CHECK(plan.Materials[0].EmissionPowerLumens == 200.0);
    REQUIRE(plan.Materials[0].EmitterSources.GetSize() == 1);
    CHECK(plan.Materials[0].EmitterSources[0] == 0);
}
TEST_CASE("Emitter capacity rejects unequal radiance instead of keeping only the brightest light",
          "[scene-cook][lighting]")
{
    // One more required (pbrt) emitter group than the import profile has material slots.
    std::string text = "WorldBegin\n";
    for (usize i = 1; i <= ludus::content::scene_format::IMPORT_LIMITS.Materials + 1; ++i)
    {
        text += "AreaLightSource \"diffuse\" \"float scale\" " + std::to_string(i) + "\nShape \"disk\"\n";
    }
    SceneIr scene;
    DiagnosticLog log;
    REQUIRE(pbrt::ReadSceneFromMemory({AsBytes(text), {}, "too-many-emitters.pbrt"}, scene, log) ==
            ludus::content::Status::Ok);
    CookPlan plan;
    CHECK(PlanBudget(scene, log, plan, nullptr) == ludus::content::Status::Invalid);
    CHECK(HasCode(log, codes::LIMIT_EXCEEDED) == true);
}

TEST_CASE("Lighting PFM preserves HDR energy, byte order, scale and row orientation", "[scene-cook][lighting]")
{
    const std::string hdr = MakePfm();
    DecodedLightingImage image;
    std::string_view message;
    REQUIRE(DecodeLightingImage("sky.pfm", {reinterpret_cast<const uint8*>(hdr.data()), hdr.size()}, image, message) ==
            ImageResult::Ok);
    REQUIRE(image.Rgb.GetSize() == 6);
    CHECK(image.Rgb[5] == 4.0F);
    std::string grey = "Pf\n1 2\n2.0\n";
    AppendFloat(grey, 1.0F, true);
    AppendFloat(grey, 3.0F, true);
    REQUIRE(
        DecodeLightingImage("grey.pfm", {reinterpret_cast<const uint8*>(grey.data()), grey.size()}, image, message) ==
        ImageResult::Ok);
    REQUIRE(image.Rgb.GetSize() == 6);
    CHECK(image.Rgb[0] == 6.0F);
    CHECK(image.Rgb[3] == 2.0F);
    grey.resize(grey.size() - 1);
    CHECK(
        DecodeLightingImage("broken.pfm", {reinterpret_cast<const uint8*>(grey.data()), grey.size()}, image, message) ==
        ImageResult::Invalid);
    CHECK(image.Rgb[0] == 6.0F);
    CHECK(image.Height == 2);
}

TEST_CASE("Cooked v3 lighting preserves photometric lights, HDR environment and chart provenance",
          "[scene-cook][lighting]")
{
    ScratchDirectory scratch;
    std::string hdr = "PF\n2 2\n-1.0\n";
    for (uint32 i = 0; i < 4; ++i)
    {
        AppendFloat(hdr, 4.0F, false);
        AppendFloat(hdr, 2.0F, false);
        AppendFloat(hdr, 1.0F, false);
    }
    WriteFile(scratch.Path / "sky.pfm", hdr);
    const std::string text =
        "WorldBegin\n"
        "LightSource \"point\" \"float power\" 100 \"float scale\" 2 \"point3 from\" [ 1 2 3 ]\n"
        "LightSource \"distant\" \"float illuminance\" 50\n"
        "LightSource \"infinite\" \"string filename\" \"sky.pfm\" \"float scale\" 2\n"
        "AreaLightSource \"diffuse\" \"float power\" 100 \"bool twosided\" true\n"
        "Shape \"trianglemesh\" \"point3 P\" [ 0 0 0  2 0 0  0 1 0 ] \"integer indices\" [ 0 1 2 ]\n";
    WriteFile(scratch.Path / "lighting.pbrt", text);
    BestEffortResult result;
    CookError error;
    REQUIRE(ImportPbrtBestEffort((scratch.Path / "lighting.pbrt").string(), {}, result, error) == true);
    ludus::resources::SceneDefinition decoded;
    REQUIRE(DecodesAtRuntime(result, decoded) == true);
    REQUIRE(decoded.Lights.GetSize() == 2);
    CHECK(decoded.Lights[0].SourceId != 0);
    CHECK(decoded.Lights[0].SourceId != decoded.Lights[1].SourceId);
    CHECK(decoded.Lights[0].PositionWorld == ludus::foundation::math::Vector3{1, 2, 3});
    CHECK_THAT(decoded.Lights[0].IntensityRgb.X,
               Catch::Matchers::WithinAbs(200.0 / (4 * ludus::foundation::math::kPi), 1e-5));
    CHECK(decoded.Lights[1].IntensityRgb.X == 50.0F);
    REQUIRE(decoded.Environment.Pixels.GetSize() == 8);
    CHECK(decoded.Environment.Width == 4);
    CHECK(decoded.Environment.Height == 2);
    CHECK(decoded.Environment.Pixels[0] == ludus::foundation::math::Vector3{8, 4, 2});
    CHECK(decoded.Environment.Rgb == ludus::foundation::math::Vector3{1, 1, 1});
    REQUIRE(decoded.Materials.GetSize() == 1);
    CHECK(decoded.Materials[0].IsEmissionTwoSided == true);
    CHECK(decoded.Materials[0].HasEmissionPower == true);
    CHECK(decoded.Materials[0].EmissionPower == 100.0F);
    CHECK(decoded.Materials[0].AreaSourceId != 0);
    CHECK(decoded.Objects[0].SourceId != 0);
    // Version 4 keeps the mesh indexed; bake charts are derived per triangle corner.
    CHECK(decoded.Meshes[0].BakeTexcoords.IsEmpty() == true);
    CHECK(decoded.Meshes[0].Positions.GetSize() == 3);
    ludus::content::scene_format::BakeChartLayout layout;
    CHECK(ludus::content::scene_format::TryMakeBakeChartLayout(
              static_cast<ludus::foundation::core::uint32>(decoded.Meshes[0].Indices.GetSize() / 3),
              layout) == true);
    bool hasEnvironmentDigest = false;
    for (const ludus::foundation::String& input : result.Inputs)
    {
        hasEnvironmentDigest = hasEnvironmentDigest || input.GetView() == "sky.pfm";
    }
    CHECK(hasEnvironmentDigest == true);
    const std::string_view report(reinterpret_cast<const char*>(result.Report.GetData()), result.Report.GetSize());
    CHECK(report.find("not cooked") == std::string_view::npos);
    CHECK(report.find("\"analytic_lights\": 2") != std::string_view::npos);
    CHECK(report.find("\"scene_format_version\": 6") != std::string_view::npos);
    BestEffortResult repeat;
    REQUIRE(ImportPbrtBestEffort((scratch.Path / "lighting.pbrt").string(), {}, repeat, error) == true);
    REQUIRE(result.Bundle.GetSize() == repeat.Bundle.GetSize());
    CHECK(std::memcmp(result.Bundle.GetData(), repeat.Bundle.GetData(), result.Bundle.GetSize()) == 0);
}

TEST_CASE("Authored lighting rejects power and cone terms that cannot round-trip the frozen profile",
          "[scene-cook][lighting]")
{
    ScratchDirectory scratch;
    const std::string geometry =
        "Shape \"trianglemesh\" \"point3 P\" [0 0 0 100 0 0 0 100 0] \"integer indices\" [0 1 2]\n";
    for (const std::string& light :
         {std::string("AreaLightSource \"diffuse\" \"float power\" 2000000\n"),
          std::string("LightSource \"spot\" \"float coneangle\" 0.001 \"float conedeltaangle\" 0.0001\n")})
    {
        std::string scene = "WorldBegin\n";
        scene += light;
        scene += geometry;
        WriteFile(scratch.Path / "bounds.pbrt", scene);
        BestEffortResult result;
        CookError error;
        CHECK(ImportPbrtBestEffort((scratch.Path / "bounds.pbrt").string(), {}, result, error) == false);
        CHECK(ReportHasCode(result, codes::LIMIT_EXCEEDED) == true);
    }
}
