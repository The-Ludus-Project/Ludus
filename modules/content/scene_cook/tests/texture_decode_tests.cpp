// Serial-oracle tests for texture work on a caller-owned JobSystem (decoding, surface
// repacking and halving to the budget): the importer must cook identical bundles,
// reports and input digests for every worker count and without a job system
// (BestEffortOptions::JobsOrNull).

#include "import_test_support.h"

#include <ludus/content/scene_cook/best_effort_import.h>
#include <ludus/content/scene_format/limits.h>
#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/threading/jobs.hpp>
#include <ludus/runtime/resources/scene.h>

#include "importers/import_diagnostics.h"
#include "importers/scene_ir.h"
#include "importers/texture_decode_queue.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

namespace
{
using namespace ludus::scene_import_tests;
using namespace ludus::content::scene_cook;
using namespace ludus::content::scene_cook::importers;
namespace threading = ludus::foundation::threading;
using ludus::content::Status;
using ludus::foundation::core::int32;
using ludus::foundation::core::uint32;
using ludus::foundation::core::usize;

/// Worker budgets compared with the serial oracle: the zero-worker executor, one
/// worker, batches smaller than the plan, and the batch cap (seven workers and the caller).
constexpr usize WORKER_BUDGET_COUNT = 4;
const ludus::foundation::StaticArray<uint32, WORKER_BUDGET_COUNT> WORKER_BUDGETS{0, 1, 3, 7};

/// A PNG signature followed by bytes that are not a valid PNG stream.
constexpr std::string_view BROKEN_PNG = "\x89PNG\r\n\x1a\nbroken";

std::string_view GetView(const ludus::foundation::Array<ludus::foundation::core::uint8>& bytes)
{
    return {reinterpret_cast<const char*>(bytes.GetData()), bytes.GetSize()};
}

void CopyTexture(const ScratchDirectory& scratch, std::string_view fixture, std::string_view name)
{
    std::filesystem::copy_file(GetFixture(fixture), scratch.Path / "textures" / name);
}

/// Append one material and a triangle that uses it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the OBJ and MTL texts are distinct outputs.
void AddMaterial(std::string& inoutWavefront, std::string& inoutMtl, int32 index, std::string_view statements)
{
    const std::string name = "M" + std::to_string(index);
    inoutMtl += "newmtl " + name + "\n" + std::string(statements);
    const std::string x = std::to_string(index * 2);
    inoutWavefront += "v " + x + " 0 0\nv " + x + ".5 0 0\nv " + x + " 1 0\nusemtl " + name + "\nf -3 -2 -1\n";
}

void Import(const std::string& entry, threading::JobSystem* jobsOrNull, BestEffortResult& outResult)
{
    BestEffortOptions options;
    options.JobsOrNull = jobsOrNull;
    CookError error;
    REQUIRE(ImportSceneBestEffort(entry, options, outResult, error) == true);
}

void RequireSameImport(const BestEffortResult& expected, const BestEffortResult& actual)
{
    // Compare as booleans: a mismatch would otherwise print megabytes of bundle bytes.
    const bool isBundleSame = GetView(expected.Bundle) == GetView(actual.Bundle);
    CHECK(isBundleSame == true);
    CHECK(GetView(expected.Report) == GetView(actual.Report));
    REQUIRE(expected.Inputs.GetSize() == actual.Inputs.GetSize());
    REQUIRE(expected.InputDigests.GetSize() == actual.InputDigests.GetSize());
    for (usize i = 0; i < expected.Inputs.GetSize(); ++i)
    {
        CHECK(expected.Inputs[i].GetView() == actual.Inputs[i].GetView());
        CHECK(expected.InputDigests[i] == actual.InputDigests[i]);
    }
}

/// Import entry serially, then on each worker budget, and require identical outputs.
void RequireWorkerCountIndependence(const std::string& entry, BestEffortResult& outSerial)
{
    Import(entry, nullptr, outSerial);
    for (const uint32 workers : WORKER_BUDGETS)
    {
        INFO("workers " << workers);
        threading::JobSystem system;
        REQUIRE(system.Initialize({ .WorkerCount = workers }) == threading::JobStatus::Ok);
        BestEffortResult parallel;
        Import(entry, &system, parallel);
        RequireSameImport(outSerial, parallel);
    }
}

bool HasReportCode(const BestEffortResult& result, std::string_view code)
{
    return GetView(result.Report).find("\"code\": \"" + std::string(code) + "\"") != std::string_view::npos;
}

/// The report's first diagnostic object with code, or empty when there is none.
std::string_view GetReportDiagnostic(const BestEffortResult& result, std::string_view code)
{
    const std::string_view report = GetView(result.Report);
    const usize begin = report.find("\"code\": \"" + std::string(code) + "\"");
    if (begin == std::string_view::npos)
    {
        return {};
    }
    const usize end = report.find('}', begin);
    return report.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
}

/// Cancellation that answers false for a fixed number of polls, then true.
struct CountdownCancel final
{
    std::atomic<int32> Remaining{0};
};

bool RequestAfterCountdown(void* context) noexcept
{
    return static_cast<CountdownCancel*>(context)->Remaining.fetch_sub(1) <= 0;
}

threading::JobOutcome SucceedImmediately(void*) noexcept
{
    return threading::JobOutcome::Succeeded;
}

/// Decode texture inline, without a job system, as the reference result.
TextureDecode DecodeSerially(const SceneIr& scene, std::string_view root, uint32 texture)
{
    TextureDecodeQueue serial(scene, root, nullptr, nullptr);
    TextureDecode decode;
    REQUIRE(serial.Take(texture, decode) == Status::Ok);
    return decode;
}

/// RGBA8 channels of a decoded texel.
constexpr usize TEXEL_CHANNELS = 4;

/// Channel (RGBA) of texel (x, y) of a WriteTga image: hashed, so box sums take every
/// remainder and nearest sampling of odd sizes shows any change of rounding or selection.
ludus::foundation::core::uint8 GetTgaTexel(uint32 x, uint32 y, usize channel)
{
    uint32 value = x * 0x9E3779B1U + y * 0x85EBCA77U + static_cast<uint32>(channel) * 0xC2B2AE3DU;
    value ^= value >> 15;
    value *= 0x2C1B3C6DU;
    value ^= value >> 12;
    return static_cast<ludus::foundation::core::uint8>(value & 0xff);
}

/// Write an uncompressed, top-down 32-bit TGA of GetTgaTexel texels.
void WriteTga(const std::filesystem::path& path, uint32 width, uint32 height)
{
    constexpr usize HEADER_BYTES = 18;
    std::string bytes(HEADER_BYTES, '\0');
    bytes[2] = 2; // Uncompressed true colour.
    bytes[12] = static_cast<char>(width & 0xff);
    bytes[13] = static_cast<char>(width >> 8);
    bytes[14] = static_cast<char>(height & 0xff);
    bytes[15] = static_cast<char>(height >> 8);
    bytes[16] = 32;
    bytes[17] = 0x28; // Top-down rows, eight alpha bits.
    bytes.reserve(HEADER_BYTES + usize{width} * height * TEXEL_CHANNELS);
    const ludus::foundation::StaticArray<usize, TEXEL_CHANNELS> fileChannels{2, 1, 0, 3};
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            for (const usize channel : fileChannels)
            {
                bytes.push_back(static_cast<char>(GetTgaTexel(x, y, channel)));
            }
        }
    }
    WriteFile(path, bytes);
}

/// An RGBA8 image computed independently of the importer.
struct ReferenceImage final
{
    uint32 Width = 0;
    uint32 Height = 0;
    ludus::foundation::Array<ludus::foundation::core::uint8> Texels;
};

ReferenceImage MakeTgaReference(uint32 width, uint32 height)
{
    ReferenceImage image{ .Width = width, .Height = height, .Texels = {} };
    REQUIRE(image.Texels.TryResize(usize{width} * height * TEXEL_CHANNELS) == true);
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            for (usize k = 0; k < TEXEL_CHANNELS; ++k)
            {
                image.Texels[(usize{y} * width + x) * TEXEL_CHANNELS + k] = GetTgaTexel(x, y, k);
            }
        }
    }
    return image;
}

/// The 2x2 box halving of the import profile, clamping each texel coordinate at odd edges.
ReferenceImage HalveReference(const ReferenceImage& source)
{
    ReferenceImage image
    {
        .Width = source.Width > 1 ? source.Width / 2 : 1,
        .Height = source.Height > 1 ? source.Height / 2 : 1,
        .Texels = {},
    };
    REQUIRE(image.Texels.TryResize(usize{image.Width} * image.Height * TEXEL_CHANNELS) == true);
    for (uint32 y = 0; y < image.Height; ++y)
    {
        for (uint32 x = 0; x < image.Width; ++x)
        {
            for (usize k = 0; k < TEXEL_CHANNELS; ++k)
            {
                uint32 sum = 0;
                for (uint32 dy = 0; dy < 2; ++dy)
                {
                    for (uint32 dx = 0; dx < 2; ++dx)
                    {
                        const uint32 sx = ludus::foundation::math::Min(x * 2 + dx, source.Width - 1);
                        const uint32 sy = ludus::foundation::math::Min(y * 2 + dy, source.Height - 1);
                        sum += source.Texels[(usize{sy} * source.Width + sx) * TEXEL_CHANNELS + k];
                    }
                }
                image.Texels[(usize{y} * image.Width + x) * TEXEL_CHANNELS + k] =
                    static_cast<ludus::foundation::core::uint8>((sum + 2) / 4);
            }
        }
    }
    return image;
}

/// One source of a reference surface map: a WriteTga image's channel, or a constant.
struct ReferenceSurfaceChannel final
{
    uint32 Width = 0;
    uint32 Height = 0;
    usize Channel = 0;
    ludus::foundation::core::uint8 Constant = 0;
};

/// A LUSCN v6 surface map at width x height whose channels sample their WriteTga
/// sources nearest (x * source width / width), or hold their constants without one.
ReferenceImage
MakeSurfaceReference(uint32 width,
                     uint32 height,
                     const ludus::foundation::StaticArray<ReferenceSurfaceChannel, TEXEL_CHANNELS>& channels)
{
    ReferenceImage image{ .Width = width, .Height = height, .Texels = {} };
    REQUIRE(image.Texels.TryResize(usize{width} * height * TEXEL_CHANNELS) == true);
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            for (usize k = 0; k < TEXEL_CHANNELS; ++k)
            {
                const ReferenceSurfaceChannel& channel = channels[k];
                image.Texels[(usize{y} * width + x) * TEXEL_CHANNELS + k] =
                    channel.Width == 0 ? channel.Constant
                                       : GetTgaTexel(static_cast<uint32>(usize{x} * channel.Width / width),
                                                     static_cast<uint32>(usize{y} * channel.Height / height),
                                                     channel.Channel);
            }
        }
    }
    return image;
}

/// True when the scene holds a texture equal to expected (colour textures in RGB only).
bool HasTexture(const ludus::resources::SceneDefinition& scene, const ReferenceImage& expected, bool isLinear)
{
    for (const ludus::resources::SceneTexture& texture : scene.Textures)
    {
        if (texture.Width != expected.Width || texture.Height != expected.Height || texture.IsLinear != isLinear)
        {
            continue;
        }
        bool isEqual = true;
        for (usize i = 0; i < expected.Texels.GetSize() && isEqual == true; ++i)
        {
            isEqual = (isLinear == false && i % TEXEL_CHANNELS == TEXEL_CHANNELS - 1) ||
                      texture.Pixels[i] == expected.Texels[i];
        }
        if (isEqual == true)
        {
            return true;
        }
    }
    return false;
}

void RequireSameDecode(const TextureDecode& expected, const TextureDecode& actual)
{
    CHECK(expected.Read == actual.Read);
    CHECK(expected.IsFileRead == actual.IsFileRead);
    CHECK(expected.ReadDigest == actual.ReadDigest);
    CHECK(expected.Decoded == actual.Decoded);
    CHECK(expected.Message == actual.Message);
    CHECK(expected.Image.Width == actual.Image.Width);
    CHECK(expected.Image.Height == actual.Image.Height);
    CHECK(GetView(expected.Image.Texels) == GetView(actual.Image.Texels));
}

/// Scene IR with file, embedded, missing and undecodable texture images below root.
void MakeQueueScene(const ScratchDirectory& scratch, SceneIr& outScene)
{
    std::filesystem::create_directories(scratch.Path / "textures");
    CopyTexture(scratch, "obj/room/textures/checker.png", "checker.png");
    CopyTexture(scratch, "obj/room/textures/normal.png", "normal.png");
    CopyTexture(scratch, "obj/cutout/textures/leaf.png", "leaf.png");
    WriteFile(scratch.Path / "textures/broken.png", BROKEN_PNG);
    for (const std::string_view file :
         {"textures/checker.png", "textures/normal.png", "textures/missing.png", "textures/broken.png", ""})
    {
        IrTexture* textureOrNull = outScene.Textures.TryAddInPlace();
        REQUIRE(textureOrNull != nullptr);
        REQUIRE(textureOrNull->File.TryAssign(file) == ludus::foundation::StringStatus::Ok);
    }
    // The last texture is an embedded copy of leaf.png.
    IrEmbeddedImage* embeddedOrNull = outScene.EmbeddedImages.TryAddInPlace();
    REQUIRE(embeddedOrNull != nullptr);
    REQUIRE(embeddedOrNull->DecoderName.TryAssign("embedded.png") == ludus::foundation::StringStatus::Ok);
    const std::string leaf = ReadFileText(scratch.Path / "textures/leaf.png");
    REQUIRE(embeddedOrNull->Bytes.TryAddRange(AsBytes(leaf)) == true);
    outScene.Textures[outScene.Textures.GetSize() - 1].EmbeddedImage = 0;
}
} // namespace

TEST_CASE("Texture decoding cooks identical outputs for every worker count", "[scene_cook][obj][threading]")
{
    ScratchDirectory scratch;
    std::filesystem::create_directories(scratch.Path / "textures");
    CopyTexture(scratch, "obj/room/textures/checker.png", "checker.png");
    CopyTexture(scratch, "obj/room/textures/normal.png", "normal.png");
    CopyTexture(scratch, "obj/room/textures/rough.png", "rough.png");
    CopyTexture(scratch, "obj/cutout/textures/leaf.png", "leaf.png");
    CopyTexture(scratch, "obj/cutout/textures/leaf_mask.png", "leaf_mask.png");
    WriteFile(scratch.Path / "textures/broken.png", BROKEN_PNG);
    // Every map kind, shared images, unreadable and undecodable images, repeated so the
    // plan spans several batches for every budget.
    constexpr usize MATERIAL_KIND_COUNT = 10;
    const ludus::foundation::StaticArray<std::string_view, MATERIAL_KIND_COUNT> kinds{
        "Kd 1 1 1\nmap_Kd textures/checker.png\n",
        "Kd 1 1 1\nmap_Kd textures/leaf.png\nmap_d textures/leaf_mask.png\n",
        "Kd 0.5 0.5 0.5\nmap_Kd textures/checker.png\nnorm textures/normal.png\nPr 0.5\n"
        "map_Pr -imfchan g textures/rough.png\nPm 1\nmap_Pm textures/rough.png\n",
        "Kd 0.2 0.2 0.2\nKe 1 1 1\nmap_Ke textures/checker.png\n",
        "Kd 1 1 1\nmap_Kd textures/missing.png\n",
        "Kd 1 1 1\nmap_Kd textures/broken.png\n",
        "Kd 0.3 0.3 0.3\nnorm textures/broken.png\nPr 0.5\nmap_Pr textures/rough.png\n",
        "Kd 0.2 0.2 0.2\nKe 2 2 2\nmap_Ke textures/missing.png\n",
        "Kd 1 0 0\nmap_d textures/missing.png\n",
        "Kd 0 1 0\nmap_Kd textures/leaf.png\nnorm textures/normal.png\nKe 1 0 0\nmap_Ke textures/leaf.png\n",
    };
    constexpr int32 REPEATS = 3;
    std::string wavefront = "mtllib m.mtl\n";
    std::string mtl;
    int32 index = 0;
    for (int32 repeat = 0; repeat < REPEATS; ++repeat)
    {
        for (const std::string_view kind : kinds)
        {
            AddMaterial(wavefront, mtl, index++, kind);
        }
    }
    WriteFile(scratch.Path / "scene.obj", wavefront);
    WriteFile(scratch.Path / "m.mtl", mtl);
    BestEffortResult serial;
    RequireWorkerCountIndependence((scratch.Path / "scene.obj").string(), serial);
    CHECK(HasReportCode(serial, codes::TEXTURE_UNREADABLE) == true);
    // Shared swatches/maps can make the image count smaller than the material count;
    // verify that both detail-map roles survived instead of assuming one image per slot.
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(serial.Bundle.GetData(), serial.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    bool hasSurfaceMap = false;
    bool hasEmissionMap = false;
    for (const ludus::resources::SceneMaterial& material : decoded.Materials)
    {
        hasSurfaceMap = hasSurfaceMap == true || material.SurfaceTexture != ludus::content::scene_format::NO_TEXTURE;
        hasEmissionMap = hasEmissionMap == true || material.EmissionTexture != ludus::content::scene_format::NO_TEXTURE;
    }
    CHECK(hasSurfaceMap == true);
    CHECK(hasEmissionMap == true);
}

TEST_CASE("Texture decoding stays serial-identical where decode failures shift the texture limit",
          "[scene_cook][obj][threading]")
{
    namespace format = ludus::content::scene_format;
    // Three free texture slots after the base swatches. The first two slots' only surface
    // source fails, so the plan, which assumes success, expects the limit two maps early:
    // later slots take their maps off the plan and are decoded inline.
    constexpr int32 MATERIAL_COUNT = static_cast<int32>(format::IMPORT_LIMITS.Textures) - 3;
    constexpr int32 FAILING_SLOTS = 2;
    ScratchDirectory scratch;
    std::filesystem::create_directories(scratch.Path / "textures");
    CopyTexture(scratch, "obj/room/textures/normal.png", "normal.png");
    CopyTexture(scratch, "obj/room/textures/rough.png", "rough.png");
    WriteFile(scratch.Path / "textures/broken.png", BROKEN_PNG);
    std::string wavefront = "mtllib m.mtl\n";
    std::string mtl;
    for (int32 i = 0; i < MATERIAL_COUNT; ++i)
    {
        const std::string normalName = "normal" + std::to_string(i) + ".png";
        std::filesystem::copy_file(scratch.Path / "textures/normal.png", scratch.Path / "textures" / normalName);
        const std::string terms =
            i < FAILING_SLOTS ? "Kd 0.5 0.5 0.5\nmap_Kd textures/rough.png\nnorm textures/broken.png\n"
                              : "Kd 0.5 0.5 0.5\nmap_Kd textures/rough.png\nnorm textures/" + normalName +
                                    "\nPr 0.5\nmap_Pr -imfchan g textures/rough.png\nPm 1\nmap_Pm textures/rough.png\n";
        AddMaterial(wavefront, mtl, i, terms);
    }
    WriteFile(scratch.Path / "scene.obj", wavefront);
    WriteFile(scratch.Path / "m.mtl", mtl);
    BestEffortResult serial;
    RequireWorkerCountIndependence((scratch.Path / "scene.obj").string(), serial);
    CHECK(serial.Summary.CookedTextures == format::IMPORT_LIMITS.Textures);
    CHECK(HasReportCode(serial, codes::TEXTURE_APPROXIMATED) == true);
    CHECK(HasReportCode(serial, codes::NORMALMAP_DROPPED) == true);
}

TEST_CASE("Texture halving and surface repacking cook identical outputs for every worker count",
          "[scene_cook][obj][threading]")
{
    namespace format = ludus::content::scene_format;
    ScratchDirectory scratch;
    std::filesystem::create_directories(scratch.Path / "textures");
    // Odd sizes beyond the per-texture limits (dimension, and bytes for the square map)
    // need one or more halvings each, with clamped edges; the small odd data maps are
    // sampled nearest at the large normal map's resolution in row bands.
    WriteTga(scratch.Path / "textures/wide.tga", format::MAX_TEXTURE_DIMENSION * 2 + 3, 5);
    WriteTga(scratch.Path / "textures/tall.tga", 3, format::MAX_TEXTURE_DIMENSION + 7);
    WriteTga(scratch.Path / "textures/square.tga", 2049, 2051);
    WriteTga(scratch.Path / "textures/normal.tga", format::MAX_TEXTURE_DIMENSION + 5, 33);
    WriteTga(scratch.Path / "textures/rough.tga", 17, 41);
    WriteTga(scratch.Path / "textures/metal.tga", 29, 7);
    constexpr usize MATERIAL_KIND_COUNT = 5;
    const ludus::foundation::StaticArray<std::string_view, MATERIAL_KIND_COUNT> kinds{
        "Kd 1 1 1\nmap_Kd textures/wide.tga\n",
        "Kd 1 1 1\nmap_Kd textures/tall.tga\nnorm textures/normal.tga\nPr 0.5\n"
        "map_Pr -imfchan g textures/rough.tga\nPm 1\nmap_Pm -imfchan b textures/metal.tga\n",
        "Kd 1 1 1\nmap_Kd textures/square.tga\nPr 0.5\nmap_Pr -imfchan g textures/rough.tga\n"
        "Pm 1\nmap_Pm textures/rough.tga\n",
        "Kd 1 1 1\nmap_Kd textures/rough.tga\nPm 1\nmap_Pm -imfchan r textures/metal.tga\n",
        "Kd 0.5 0.5 0.5\nnorm textures/normal.tga\n",
    };
    std::string wavefront = "mtllib m.mtl\n";
    std::string mtl;
    int32 index = 0;
    for (const std::string_view kind : kinds)
    {
        AddMaterial(wavefront, mtl, index++, kind);
    }
    WriteFile(scratch.Path / "scene.obj", wavefront);
    WriteFile(scratch.Path / "m.mtl", mtl);
    BestEffortResult serial;
    RequireWorkerCountIndependence((scratch.Path / "scene.obj").string(), serial);
    CHECK(HasReportCode(serial, codes::TEXTURE_DOWNSCALED) == true);

    // The serial textures against references built texel by texel: halvings with
    // clamped odd edges, and nearest-sampled surface maps (RG normal, B roughness, A metallic).
    ludus::resources::SceneDefinition decoded;
    REQUIRE(ludus::resources::SceneDefinition::Decode(serial.Bundle.GetData(), serial.Bundle.GetSize(), decoded) ==
            ludus::resources::Status::Ok);
    CHECK(decoded.Textures.GetSize() == 9);
    const uint32 wideWidth = format::MAX_TEXTURE_DIMENSION * 2 + 3;
    const uint32 tallHeight = format::MAX_TEXTURE_DIMENSION + 7;
    const uint32 normalWidth = format::MAX_TEXTURE_DIMENSION + 5;
    CHECK(HasTexture(decoded, HalveReference(HalveReference(MakeTgaReference(wideWidth, 5))), false) == true);
    CHECK(HasTexture(decoded, HalveReference(MakeTgaReference(3, tallHeight)), false) == true);
    CHECK(HasTexture(decoded, HalveReference(MakeTgaReference(2049, 2051)), false) == true);
    CHECK(HasTexture(decoded, MakeTgaReference(17, 41), false) == true);
    constexpr ludus::foundation::core::uint8 FLAT = 128;
    constexpr ludus::foundation::core::uint8 UNIT = 255;
    const ReferenceSurfaceChannel normalRed{ .Width = normalWidth, .Height = 33, .Channel = 0, .Constant = 0 };
    const ReferenceSurfaceChannel normalGreen{ .Width = normalWidth, .Height = 33, .Channel = 1, .Constant = 0 };
    const ReferenceSurfaceChannel flat{ .Width = 0, .Height = 0, .Channel = 0, .Constant = FLAT };
    const ReferenceSurfaceChannel unit{ .Width = 0, .Height = 0, .Channel = 0, .Constant = UNIT };
    CHECK(HasTexture(decoded,
                     HalveReference(MakeSurfaceReference(normalWidth,
                                                         33,
                                                         {normalRed,
                                                          normalGreen,
                                                          { .Width = 17, .Height = 41, .Channel = 1, .Constant = 0 },
                                                          { .Width = 29, .Height = 7, .Channel = 2, .Constant = 0 }})),
                     true) == true);
    CHECK(HasTexture(decoded,
                     MakeSurfaceReference(17,
                                          41,
                                          {flat,
                                           flat,
                                           { .Width = 17, .Height = 41, .Channel = 1, .Constant = 0 },
                                           { .Width = 17, .Height = 41, .Channel = 0, .Constant = 0 }}),
                     true) == true);
    CHECK(HasTexture(decoded,
                     MakeSurfaceReference(29,
                                          7,
                                          {flat,
                                           flat,
                                           unit,
                                           {
                                               .Width = 29,
                                               .Height = 7,
                                               .Channel = 0,
                                               .Constant = 0,
                                           }}),
                     true) == true);
    CHECK(HasTexture(decoded,
                     HalveReference(MakeSurfaceReference(normalWidth, 33, {normalRed, normalGreen, unit, unit})),
                     true) == true);
}

TEST_CASE("PBRT normal maps and shape masks are identical across worker counts", "[scene_cook][pbrt][threading]")
{
    ScratchDirectory scratch;
    std::filesystem::create_directories(scratch.Path / "textures");
    CopyTexture(scratch, "obj/room/textures/normal.png", "normal.png");
    CopyTexture(scratch, "obj/cutout/textures/leaf.png", "base.png");
    CopyTexture(scratch, "obj/cutout/textures/leaf_mask.png", "mask.png");
    WriteFile(scratch.Path / "scene.pbrt", R"(WorldBegin
Texture "base" "spectrum" "imagemap" "string filename" "textures/base.png"
Texture "coverage" "float" "imagemap" "string filename" "textures/mask.png" "string encoding" "linear"
MakeNamedMaterial "leaf" "string type" "diffuse" "texture reflectance" "base" "string normalmap" "textures/normal.png"
NamedMaterial "leaf"
Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0] "point2 uv" [0 0 1 0 0 1] "integer indices" [0 1 2]
Shape "trianglemesh" "point3 P" [2 0 0 3 0 0 2 1 0] "point2 uv" [0 0 1 0 0 1] "integer indices" [0 1 2] "texture alpha" "coverage"
AreaLightSource "diffuse" "rgb L" [1 1 1]
Shape "trianglemesh" "point3 P" [4 0 0 5 0 0 4 1 0] "point2 uv" [0 0 1 0 0 1] "integer indices" [0 1 2] "texture alpha" "coverage"
)");
    BestEffortResult serial;
    RequireWorkerCountIndependence((scratch.Path / "scene.pbrt").string(), serial);
    CHECK(serial.Summary.CookedTextures == 4);
    CHECK(HasReportCode(serial, codes::NORMALMAP_DROPPED) == false);
    CHECK(HasReportCode(serial, codes::TEXTURE_UNREADABLE) == false);
}

TEST_CASE("A shared roughness and metallic texture is reported for each term", "[scene_cook][gltf][threading]")
{
    ScratchDirectory scratch;
    std::filesystem::create_directories(scratch.Path / "textures");
    CopyTexture(scratch, "obj/room/textures/normal.png", "normal.png");
    // One metallicRoughness image for both terms, missing in the first material.
    const std::string gltf =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)"
        R"("nodes":[{"mesh":0},{"mesh":1,"translation":[2,0,0]}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]},)"
        R"({"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":1}]}],)"
        R"("materials":[{"pbrMetallicRoughness":{"metallicRoughnessTexture":{"index":0}},"normalTexture":{"index":1}},)"
        R"({"pbrMetallicRoughness":{"metallicRoughnessTexture":{"index":1}}}],)"
        R"("textures":[{"source":0},{"source":1}],)"
        R"("images":[{"uri":"textures/missing.png"},{"uri":"textures/normal.png"}],)"
        R"("buffers":[{"byteLength":60,"uri":"data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
        R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}]})";
    WriteFile(scratch.Path / "scene.gltf", gltf);
    BestEffortResult serial;
    RequireWorkerCountIndependence((scratch.Path / "scene.gltf").string(), serial);
    // Both terms of the unreadable image are reported, as when each decoded its own copy.
    const std::string_view unreadable = GetReportDiagnostic(serial, codes::TEXTURE_UNREADABLE);
    CHECK(unreadable.find(R"("file": "textures/missing.png")") != std::string_view::npos);
    CHECK(unreadable.find(R"("count": 2,)") != std::string_view::npos);
    CHECK(serial.Summary.CookedTextures == 3); // One shared white swatch and two different surface maps.
}

TEST_CASE("Texture decode queue matches inline decoding in and out of planned order", "[scene_cook][threading]")
{
    ScratchDirectory scratch;
    SceneIr scene;
    MakeQueueScene(scratch, scene);
    const std::string root = scratch.Path.string();
    const uint32 textureCount = static_cast<uint32>(scene.Textures.GetSize());
    for (const uint32 workers : WORKER_BUDGETS)
    {
        INFO("workers " << workers);
        threading::JobSystem system;
        REQUIRE(system.Initialize({ .WorkerCount = workers }) == threading::JobStatus::Ok);
        TextureDecodeQueue queue(scene, root, &system, nullptr);
        // Plan every texture twice, then take them in order except for one skipped
        // and one swapped pair.
        for (uint32 texture = 0; texture < textureCount; ++texture)
        {
            REQUIRE(queue.TryPlan(texture) == true);
            REQUIRE(queue.TryPlan(texture) == true);
        }
        constexpr usize TAKE_COUNT = 9;
        const ludus::foundation::StaticArray<uint32, TAKE_COUNT> order{0, 0, 1, 2, 1, 3, 3, 4, 2};
        for (usize i = 0; i < TAKE_COUNT; ++i)
        {
            INFO("take " << i);
            TextureDecode decode;
            REQUIRE(queue.Take(order[i], decode) == Status::Ok);
            RequireSameDecode(DecodeSerially(scene, root, order[i]), decode);
        }
        // Every read file is recorded in take order; the missing and embedded images are not.
        constexpr usize FILE_READ_COUNT = 6;
        const ludus::foundation::StaticArray<uint32, FILE_READ_COUNT> reads{0, 0, 1, 1, 3, 3};
        const std::span<const TextureFileRead> fileReads = queue.GetFileReads();
        REQUIRE(fileReads.size() == FILE_READ_COUNT);
        for (usize i = 0; i < FILE_READ_COUNT; ++i)
        {
            CHECK(fileReads[i].Texture == reads[i]);
            CHECK(fileReads[i].FileDigest == DecodeSerially(scene, root, reads[i]).ReadDigest);
        }
    }
    const TextureDecode checker = DecodeSerially(scene, root, 0);
    CHECK(checker.IsFileRead == true);
    CHECK(checker.Decoded == ImageResult::Ok);
    CHECK(checker.ReadDigest == ludus::content::Hash(AsBytes(ReadFileText(scratch.Path / "textures/checker.png"))));
    // An undecodable file is still hashed: its bytes were read.
    CHECK(DecodeSerially(scene, root, 3).ReadDigest ==
          ludus::content::Hash(AsBytes(ReadFileText(scratch.Path / "textures/broken.png"))));
    const TextureDecode missing = DecodeSerially(scene, root, 2);
    CHECK(missing.Read == Status::NotFound);
    CHECK(missing.Message == "cannot read the image file");
    CHECK(DecodeSerially(scene, root, 3).Decoded == ImageResult::Invalid);
    const TextureDecode embedded = DecodeSerially(scene, root, 4);
    CHECK(embedded.IsFileRead == false);
    CHECK(embedded.ReadDigest == ludus::content::Digest{});
    CHECK(embedded.Decoded == ImageResult::Ok);
}

TEST_CASE("Texture decode queue decodes inline when the job system is busy", "[scene_cook][threading]")
{
    ScratchDirectory scratch;
    SceneIr scene;
    MakeQueueScene(scratch, scene);
    const std::string root = scratch.Path.string();
    threading::JobSystem system;
    REQUIRE(system.Initialize({ .WorkerCount = 2 }) == threading::JobStatus::Ok);
    // Another graph stays admitted until its Wait, so the queue's Submit is refused.
    threading::JobGraph other;
    threading::JobHandle handle;
    REQUIRE(other.Initialize({ .MaxJobs = 1, .MaxDependencies = 0 }) == threading::JobStatus::Ok);
    REQUIRE(other.Add(SucceedImmediately, nullptr, handle) == threading::JobStatus::Ok);
    REQUIRE(other.Seal() == threading::JobStatus::Ok);
    REQUIRE(system.Submit(other) == threading::JobStatus::Ok);
    {
        TextureDecodeQueue queue(scene, root, &system, nullptr);
        for (uint32 texture = 0; texture < scene.Textures.GetSize(); ++texture)
        {
            REQUIRE(queue.TryPlan(texture) == true);
        }
        for (uint32 texture = 0; texture < scene.Textures.GetSize(); ++texture)
        {
            TextureDecode decode;
            REQUIRE(queue.Take(texture, decode) == Status::Ok);
            RequireSameDecode(DecodeSerially(scene, root, texture), decode);
        }
    }
    threading::JobOutcome outcome = threading::JobOutcome::Pending;
    REQUIRE(system.Wait(outcome) == threading::JobStatus::Ok);
    CHECK(outcome == threading::JobOutcome::Succeeded);
}

TEST_CASE("Texture decoding honours cancellation before reading images", "[scene_cook][threading]")
{
    ScratchDirectory scratch;
    SceneIr scene;
    MakeQueueScene(scratch, scene);
    const std::string root = scratch.Path.string();
    threading::JobSystem system;
    REQUIRE(system.Initialize({ .WorkerCount = 1 }) == threading::JobStatus::Ok);
    for (threading::JobSystem* jobsOrNull : {static_cast<threading::JobSystem*>(nullptr), &system})
    {
        CountdownCancel countdown;
        countdown.Remaining = 1;
        const Cancellation cancel{RequestAfterCountdown, &countdown};
        TextureDecodeQueue queue(scene, root, jobsOrNull, &cancel);
        REQUIRE(queue.TryPlan(0) == true);
        REQUIRE(queue.TryPlan(1) == true);
        TextureDecode decode;
        CHECK(queue.Take(0, decode) == Status::Ok);
        CHECK(queue.Take(1, decode) == Status::Cancelled);
    }

    // The importer polls once more per texture image, and reports a cancellation there.
    WriteFile(scratch.Path / "scene.obj", "mtllib m.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl M\nf 1 2 3\n");
    const std::string entry = (scratch.Path / "scene.obj").string();
    for (threading::JobSystem* jobsOrNull : {static_cast<threading::JobSystem*>(nullptr), &system})
    {
        BestEffortOptions options;
        options.JobsOrNull = jobsOrNull;
        const auto countPolls = [&](std::string_view mtl) {
            WriteFile(scratch.Path / "m.mtl", mtl);
            constexpr int32 POLL_ALLOWANCE = 1000000;
            CountdownCancel counter;
            counter.Remaining = POLL_ALLOWANCE;
            const Cancellation counting{RequestAfterCountdown, &counter};
            BestEffortResult result;
            CookError error;
            REQUIRE(ImportObjBestEffort(entry, options, result, error, &counting) == true);
            return POLL_ALLOWANCE - counter.Remaining.load();
        };
        const int32 untexturedPolls = countPolls("newmtl M\nKd 1 1 1\n");
        const int32 texturedPolls = countPolls("newmtl M\nmap_Kd textures/checker.png\n");
        REQUIRE(texturedPolls == untexturedPolls + 1);
        // Cancel at the texture's poll: the last one, since none follows the texture stage.
        CountdownCancel countdown;
        countdown.Remaining = texturedPolls - 1;
        const Cancellation cancel{RequestAfterCountdown, &countdown};
        BestEffortResult cancelled;
        CookError error;
        CHECK(ImportObjBestEffort(entry, options, cancelled, error, &cancel) == false);
        CHECK(error.Result == Status::Cancelled);
        CHECK(cancelled.Bundle.IsEmpty() == true);
    }
}
